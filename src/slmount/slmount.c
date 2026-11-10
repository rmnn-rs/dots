/* slmount - ncurses mounter */
#define _GNU_SOURCE
#include <blkid/blkid.h>
#include <errno.h>
#include <fcntl.h>
#include <libcryptsetup.h>
#include <libudev.h>
#include <linux/loop.h>
#include <mntent.h>
#include <ncurses.h>
#include <poll.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define LENGTH(x) (sizeof (x) / sizeof (x)[0])

typedef struct {
    int ch;              /* a char or an ncurses KEY_* */
    void (*func)(void);
    const char *help;    /* entry in the help lines, NULL = alias */
} Key;

void quit(void);
void selup(void);
void seldown(void);
void toggleall(void);
void togglero(void);
void mountsel(void);
void mountcustom(void);
void umountsel(void);
void umountall(void);
void ejectsel(void);
void do_image(void);

#include "config.h"

typedef struct {
    char node[32];   /* /dev/sdb1 */
    char name[32];   /* sdb1 */
    char disk[32];   /* whole disk it lives on: sdb */
    char fs[16];     /* fs we mount: the inner one for an open LUKS, empty while locked */
    char label[32];  /* fs label, else model, else image file */
    char mnt[256];   /* mountpoint, empty if unmounted */
    char map[48];    /* LUKS only: mapper path, empty for everything else */
    int open;        /* LUKS only: mapping is active */
    int usb;         /* disk sits on USB (the only thing we eject) */
    int hid;         /* disk is on the hide list */
    unsigned long long bytes, avail;
} BlockDev;

static struct udev *udev;
static BlockDev devs[MAX_DEVS];
static int running = 1, count, sel, top, showall, rdonly, kfd = -1; /* kfd: /dev/kmsg */
static char homedir[256], base[256], status[256];
static uid_t ruid;   /* the human behind sudo */
static gid_t rgid;

/* what actually gets mounted */
static const char *src(const BlockDev *d) {
    return d->open ? d->map : d->node;
}

/* bounded concat of NULL-terminated args, memcpy only */
static void cat(char *dst, size_t cap, ...) {
    va_list ap;
    const char *s;
    size_t n = 0, l;

    va_start(ap, cap);
    while ((s = va_arg(ap, const char *))) {
        l = strlen(s);
        if (l > cap - 1 - n) l = cap - 1 - n;
        memcpy(dst + n, s, l);
        n += l;
    }
    va_end(ap);
    dst[n] = '\0';
}

static const char *num(char *buf, unsigned v) { /* buf >= 11 bytes */
    char *p = buf + 11;

    *--p = '\0';
    do *--p = '0' + v % 10; while (v /= 10);
    return p;
}

static int inlist(const char *s, const char *const *l) {
    for (; *l; l++)
        if (!strcmp(s, *l)) return 1;
    return 0;
}

static const char *prop(struct udev_device *d, const char *k) {
    const char *v = udev_device_get_property_value(d, k);
    return v ? v : "";
}

/* result goes to the status bar, failures ring the bell (urgent hint in dwm) */
static void notify(int ok, const char *title, const char *node, const char *detail) {
    cat(status, sizeof status, title, ": ", node, " - ", detail, NULL);
    if (!ok) beep();
}

/* mount(2) errnos are terse, say what mount(8) would have */
static const char *mnterr(int e) {
    return e == EINVAL ? "bad or unclean filesystem (see dmesg)"
         : e == ENODEV ? "filesystem not supported by kernel"
         : strerror(e);
}

/* mkdir -p, new dirs belong to the real user */
static int mkpath(const char *path) {
    char p[256], c;
    size_t i;

    cat(p, sizeof p, path, NULL);
    for (i = 1; ; i++) {
        if (p[i] != '/' && p[i]) continue;
        c = p[i];
        p[i] = '\0';
        if (!mkdir(p, 0755)) (void)!chown(p, ruid, rgid);
        else if (errno != EEXIST) return -1;
        p[i] = c;
        if (!c) return 0;
    }
}

/* remove the dir again, but only ones we'd have created under base */
static void rmmp(const char *path) {
    size_t n = strlen(base);

    if (!strncmp(path, base, n) && path[n] == '/')
        rmdir(path);
}

/* ~/foo -> /home/you/foo (no shell around to do it for us) */
static void expand(char *dst, size_t cap, const char *p) {
    if (p[0] == '~' && (p[1] == '/' || !p[1])) cat(dst, cap, homedir, p + 1, NULL);
    else cat(dst, cap, p, NULL);
}

/* the whole disk a partition lives on (or the device itself) */
static struct udev_device *diskof(struct udev_device *dev) {
    const char *type = udev_device_get_devtype(dev);

    return type && !strcmp(type, "partition")
         ? udev_device_get_parent_with_subsystem_devtype(dev, "block", "disk") : dev;
}

static int hidden(struct udev_device *disk) {
    return inlist(udev_device_get_sysname(disk), hide)
        || inlist(prop(disk, "ID_SERIAL_SHORT"), hide)
        || inlist(prop(disk, "ID_PATH"), hide);
}

static int wanted(struct udev_device *dev) {
    const char *type = udev_device_get_devtype(dev), *fs;
    struct udev_device *disk;

    if (!type || !udev_device_get_devnode(dev))
        return 0;
    if (!strcmp(type, "disk")) {
        /* bare disks and loop images only count with a fs straight on them */
        fs = prop(dev, "ID_FS_TYPE");
        if (!*fs) return 0;
        if (!strncmp(udev_device_get_sysname(dev), "loop", 4))
            return showall || strcmp(fs, "squashfs"); /* snaps live in squashfs loops */
        if (!*prop(dev, "ID_BUS")) return 0;          /* zram etc */
    } else if (strcmp(type, "partition")) {
        return 0;
    }
    disk = diskof(dev);
    return showall || !disk || !hidden(disk);
}

static void fill(BlockDev *d, struct udev_device *dev) {
    struct udev_device *in = NULL, *disk = diskof(dev);
    struct stat st;
    const char *sz = udev_device_get_sysattr_value(dev, "size");
    const char *bf = udev_device_get_sysattr_value(dev, "loop/backing_file");
    const char *lbl;
    int luks = !strcmp(prop(dev, "ID_FS_TYPE"), "crypto_LUKS");

    cat(d->node, sizeof d->node, udev_device_get_devnode(dev), NULL);
    cat(d->name, sizeof d->name, udev_device_get_sysname(dev), NULL);
    cat(d->disk, sizeof d->disk, disk ? udev_device_get_sysname(disk) : "", NULL);
    d->usb = disk && !strcmp(prop(disk, "ID_BUS"), "usb");
    d->hid = disk && hidden(disk);
    d->map[0] = '\0';
    d->open = 0;
    if (luks) {
        cat(d->map, sizeof d->map, MAPDIR "slm-", d->name, NULL);
        if ((d->open = !stat(d->map, &st)))
            in = udev_device_new_from_devnum(udev, 'b', st.st_rdev);
    }
    /* open LUKS: fs/label come from the mapped device; locked: nothing known yet */
    cat(d->fs, sizeof d->fs, luks ? (in ? prop(in, "ID_FS_TYPE") : "") : prop(dev, "ID_FS_TYPE"), NULL);
    lbl = prop(in ? in : dev, "ID_FS_LABEL");
    if (!*lbl) lbl = prop(dev, "ID_MODEL");
    if (!*lbl && bf) lbl = strrchr(bf, '/') ? strrchr(bf, '/') + 1 : bf;
    cat(d->label, sizeof d->label, lbl, NULL);
    udev_device_unref(in);
    d->bytes = sz ? strtoull(sz, NULL, 10) * 512 : 0;
    d->avail = 0;
    d->mnt[0] = '\0';
}

/* re-read one entry in place (after unlocking) */
static void refill(BlockDev *d) {
    struct udev_device *dev = udev_device_new_from_subsystem_sysname(udev, "block", d->name);

    if (dev) fill(d, dev);
    udev_device_unref(dev);
}

static void scan(void) {
    struct udev_enumerate *en = udev_enumerate_new(udev);
    struct udev_list_entry *e;
    struct udev_device *dev;
    struct mntent *m;
    struct statvfs sv;
    FILE *f;
    int i;

    udev_enumerate_add_match_subsystem(en, "block");
    udev_enumerate_scan_devices(en);
    count = 0;
    udev_list_entry_foreach(e, udev_enumerate_get_list_entry(en)) {
        if (count >= MAX_DEVS) break;
        dev = udev_device_new_from_syspath(udev, udev_list_entry_get_name(e));
        if (dev && wanted(dev)) fill(&devs[count++], dev);
        udev_device_unref(dev);
    }
    udev_enumerate_unref(en);

    if ((f = setmntent("/proc/self/mounts", "r"))) {
        while ((m = getmntent(f)))
            for (i = 0; i < count; i++)
                if (!strcmp(m->mnt_fsname, src(&devs[i]))) {
                    cat(devs[i].mnt, sizeof devs[i].mnt, m->mnt_dir, NULL);
                    if (!statvfs(m->mnt_dir, &sv))
                        devs[i].avail = (unsigned long long)sv.f_bavail * sv.f_frsize;
                }
        endmntent(f);
    }
}

/* the drive vanished under a mount / mapping: detach lazily so nothing hangs on dead I/O */
static void reap(const BlockDev *o) {
    if (o->mnt[0]) {
        umount2(o->mnt, MNT_DETACH);
        rmmp(o->mnt);
    }
    if (o->open)
        crypt_deactivate_by_name(NULL, o->map + sizeof MAPDIR - 1, CRYPT_DEACTIVATE_DEFERRED);
    notify(0, "Unplugged", o->node, "dead mount/mapping cleaned up");
}

/* rescan but keep the cursor on the same device */
static void rescan(void) {
    BlockDev old[MAX_DEVS];
    char keep[32] = "", p[64];
    int i, j, n = count;

    memcpy(old, devs, sizeof old);
    if (count) cat(keep, sizeof keep, devs[sel].node, NULL);
    scan();
    if (sel >= count) sel = count ? count - 1 : 0;
    for (i = 0; i < count; i++)
        if (!strcmp(devs[i].node, keep)) sel = i;

    /* mounted/unlocked and gone from sysfs (not just filtered out) = yanked */
    for (i = 0; i < n; i++) {
        if (!old[i].mnt[0] && !old[i].open) continue;
        for (j = 0; j < count && strcmp(devs[j].node, old[i].node); j++)
            ;
        cat(p, sizeof p, "/sys/class/block/", old[i].name, NULL);
        if (j == count && access(p, F_OK)) reap(&old[i]);
    }
}

static double human(unsigned long long b, char *unit) {
    const char *u = "BKMGT";
    double v = (double)b;
    int i = 0;

    while (v >= 1024 && u[i + 1]) { v /= 1024; i++; }
    *unit = u[i];
    return v;
}

/* flow the help entries of the key table over the lines under the title,
 * returns the last row used */
static int drawhelp(void) {
    size_t i;
    int x = 2, y = 1, n;

    for (i = 0; i < LENGTH(keys); i++) {
        if (!keys[i].help) continue;
        n = (int)strlen(keys[i].help);
        if (x > 2 && x + n > COLS - 2) { y++; x = 2; }
        if (COLS - 2 - x > 0) mvaddnstr(y, x, keys[i].help, COLS - 2 - x);
        x += n + 2;
    }
    return y;
}

static void draw(void) {
    BlockDev *d;
    double v, fr;
    char u, fu, tag[32];
    int i, y, w, hl, rows;

    erase();
    attron(A_BOLD);
    mvprintw(0, 2, "SLMOUNT%s%s", showall ? " [all]" : "", rdonly ? " [ro]" : "");
    attroff(A_BOLD);
    hl = drawhelp() + 1; /* the rule sits under however many help lines it took */
    mvhline(hl, 2, ACS_HLINE, COLS - 4);

    rows = LINES - hl - 3;
    if (rows < 1) rows = 1;
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;

    if (!count)
        mvaddstr(hl + 2, 4, showall ? "No devices." : "No removable devices (internal drives hidden).");
    for (i = top; i < count && i < top + rows; i++) {
        d = &devs[i];
        y = hl + 1 + i - top;
        v = human(d->bytes, &u);
        fr = human(d->avail, &fu);
        /* locked LUKS reads "luks", an open one "luks+<inner fs>" */
        cat(tag, sizeof tag, !d->map[0] ? "" : d->open ? "luks+" : "luks", d->fs, NULL);
        mvprintw(y, 2, "%c %-15s ", d->mnt[0] ? '*' : ' ', d->node);
        printw("%6.1f%c ", v, u);
        if (d->mnt[0]) printw("%4.0f%c free ", fr, fu);
        else           printw("%11s", "");
        printw("%-10.10s %-10.10s ", tag, d->label);
        w = COLS - getcurx(stdscr) - 1; /* whatever is left goes to the mountpoint */
        printw("%.*s", w < 0 ? 0 : w, d->mnt);
        if (i == sel) mvchgat(y, 0, -1, A_REVERSE, 0, NULL);
    }
    mvaddnstr(LINES - 1, 2, status, COLS - 2);
    refresh();
}

/* modal input box over the current screen. hidden = passphrase (shown as *).
 * Enter -> 1 (non-empty), Esc or empty -> 0 and buf is wiped */
static int prompt(const char *title, const char *msg, char *buf, int n, int hidden) {
    const char *label = hidden ? "Passphrase: " : "Path: ";
    int wd = COLS < 60 ? COLS : 60, x = 2 + (int)strlen(label), avail = wd - x - 2;
    int len = 0, ok = 0, shown, i, ch;
    WINDOW *w = newwin(7, wd, (LINES - 7) / 2, (COLS - wd) / 2);

    if (!w) return 0;
    if (avail < 1) avail = 1;
    keypad(w, TRUE);
    curs_set(1);
    buf[0] = '\0';

    for (;;) {
        werase(w);
        box(w, 0, 0);
        mvwprintw(w, 0, 2, " %.*s ", wd - 6, title);
        mvwaddnstr(w, 1, 2, msg, wd - 4);
        mvwaddstr(w, 3, 2, label);
        if (hidden) {
            for (i = shown = 0; i < len; i++)
                shown += (buf[i] & 0xC0) != 0x80; /* one * per character, not per byte */
            if (shown > avail) shown = avail;
            mvwhline(w, 3, x, '*', shown);
        } else {
            shown = len > avail ? avail : len;
            mvwaddnstr(w, 3, x, buf + len - shown, shown);
        }
        mvwaddnstr(w, 5, 2, "Enter ok   Esc cancel   ^U clear", wd - 4);
        wmove(w, 3, x + shown);
        wrefresh(w);

        ch = wgetch(w);
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) { ok = len > 0; break; }
        if (ch == 27) break;
        if (ch == 21) {
            len = 0;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (len) {
                do len--; while (len && (buf[len] & 0xC0) == 0x80); /* drop a whole UTF-8 char */
            }
        } else if (ch >= 32 && ch < 256 && len < n - 1) {
            buf[len++] = ch;
        }
        buf[len] = '\0';
    }

    delwin(w);
    curs_set(0);
    if (!ok) explicit_bzero(buf, n);
    return ok;
}

/* libcryptsetup prints to the terminal by default, which would trash ncurses */
static void nolog(int level, const char *msg, void *arg) {
    (void)level; (void)msg; (void)arg;
}

/* open the LUKS mapping: passphrase box, 3 tries; 1 on success */
static int unlock(BlockDev *d) {
    struct crypt_device *cd;
    char pw[256];
    const char *msg = d->node;
    int i, rc = -EPERM; /* -EPERM = wrong passphrase */

    for (i = 0; i < 3 && rc == -EPERM; i++) {
        if (!prompt("Unlock", msg, pw, sizeof pw, 1)) {
            status[0] = '\0';
            return 0;
        }
        cat(status, sizeof status, "Unlocking ", d->node, "...", NULL);
        draw(); /* argon2 takes a second or two, don't sit on a stale box */
        if (!(rc = crypt_init(&cd, d->node))) {
            if (!(rc = crypt_load(cd, CRYPT_LUKS, NULL)))
                rc = crypt_activate_by_passphrase(cd, d->map + sizeof MAPDIR - 1,
                                                  CRYPT_ANY_SLOT, pw, strlen(pw), 0);
            crypt_free(cd);
        }
        explicit_bzero(pw, sizeof pw);
        msg = "Wrong passphrase, try again";
    }
    if (rc < 0) {
        notify(0, "Unlock failed", d->node,
               rc == -EPERM ? "no key available with this passphrase" : strerror(-rc));
        return 0;
    }
    refill(d); /* pick up the inner fs/label */
    return 1;
}

/* what the kernel logged about this device since the lseek in do_mount,
 * e.g. "ntfs3: sdc4: volume is dirty and "force" flag is not set!" */
static void kwhy(const BlockDev *d, char *out, size_t cap) {
    char rec[8192], l[64], *m, *e;
    const char *key = strrchr(d->node, '/') + 1; /* sdb1, loop0 ... */
    ssize_t n;

    out[0] = '\0';
    if (d->open && (n = readlink(d->map, l, sizeof l - 1)) > 0) { /* mapper -> ../dm-0 */
        l[n] = '\0';
        key = strrchr(l, '/') ? strrchr(l, '/') + 1 : l;
    }
    while (kfd >= 0 && (n = read(kfd, rec, sizeof rec - 1)) > 0) { /* one record per read */
        rec[n] = '\0';
        if (!(m = strchr(rec, ';'))) continue;
        if ((e = strchr(++m, '\n'))) *e = '\0'; /* drop key=value continuation lines */
        if (strstr(m, key)) cat(out, cap, m, NULL);
    }
}

static int mount_dev(BlockDev *d, const char *path, unsigned long flags) {
    char opt[40], u[11], g[11];
    const char *type = !strcmp(d->fs, "ntfs") ? "ntfs3" : d->fs; /* in-kernel driver, no ntfs-3g helper */

    if (inlist(d->fs, ownfs))
        cat(opt, sizeof opt, "uid=", num(u, ruid), ",gid=", num(g, rgid), NULL);
    else
        opt[0] = '\0';
    return mount(src(d), path, type, flags, *opt ? opt : NULL);
}

static void do_mount(BlockDev *d, int custom) {
    char path[256], in[256], det[300], why[256] = "";
    const char *ro = rdonly ? " [read-only]" : "";
    int rc, e;

    if (d->mnt[0]) {
        cat(status, sizeof status, d->node, " already mounted at ", d->mnt, NULL);
        return;
    }
    if (d->map[0] && !d->open && !unlock(d))
        return;
    if (custom) {
        if (!prompt("Mount path", d->node, in, sizeof in, 0)) return;
        expand(path, sizeof path, in);
    } else {
        cat(path, sizeof path, base, "/", d->name, NULL);
    }

    rc = mkpath(path);
    e = errno;
    if (!rc) {
        if (kfd >= 0) lseek(kfd, 0, SEEK_END); /* only what the kernel says from now on */
        rc = mount_dev(d, path, MFLAGS | (rdonly ? MS_RDONLY : 0));
        e = errno;
        /* read-only fallback: write-protected media (like mount(8)), or an unclean
         * NTFS (Windows fast startup, unplugged dirty) which ntfs3 won't take rw */
        if (rc && !rdonly && (e == EROFS || e == EACCES || (e == EINVAL && !strcmp(d->fs, "ntfs")))) {
            ro = e == EINVAL ? " [read-only: unclean NTFS]" : " [read-only]";
            rc = mount_dev(d, path, MFLAGS | MS_RDONLY);
            e = errno;
        }
        if (rc) {
            kwhy(d, why, sizeof why);
            rmmp(path);
        }
    }
    if (rc) {
        notify(0, "Mount failed", d->node, *why ? why : mnterr(e));
    } else {
        cat(det, sizeof det, path, ro, NULL);
        notify(1, "Mounted", d->node, det);
    }
}

/* umount, and for LUKS also close the mapping (works on unlocked-but-unmounted too).
 * returns 1 if something failed */
static int do_unmount(BlockDev *d) {
    int rc;

    if (!d->mnt[0] && !d->open) {
        cat(status, sizeof status, d->node, " is not mounted", NULL);
        return 0;
    }
    if (d->mnt[0]) {
        if (umount2(d->mnt, 0)) {
            notify(0, "Unmount failed", d->node, strerror(errno));
            return 1;
        }
        rmmp(d->mnt);
    }
    if (d->open && (rc = crypt_deactivate(NULL, d->map + sizeof MAPDIR - 1)) < 0) {
        notify(0, "Lock failed", d->node, strerror(-rc));
        return 1;
    }
    notify(1, d->open ? "Locked" : "Unmounted", d->node, d->mnt[0] ? d->mnt : "mapping closed");
    d->mnt[0] = '\0'; /* keep our copy honest until the next rescan */
    d->open = 0;
    return 0;
}

/* unmount/lock the whole USB disk, then drop it from the kernel: that flushes
 * and spins the drive down, so it can be pulled */
static void do_eject(BlockDev *d) {
    char p[64];
    ssize_t n = -1;
    int i, f = 0, fd, e = 0;

    if (!d->usb) {
        notify(0, "Eject failed", d->node, "not a USB disk");
        return;
    }
    for (i = 0; i < count; i++)
        if (!strcmp(devs[i].disk, d->disk) && (devs[i].mnt[0] || devs[i].open))
            f += do_unmount(&devs[i]);
    if (f) return; /* do_unmount already said why */
    cat(p, sizeof p, "/sys/block/", d->disk, "/device/delete", NULL);
    if ((fd = open(p, O_WRONLY | O_CLOEXEC)) < 0 || (n = write(fd, "1", 1)) != 1) e = errno;
    if (fd >= 0) close(fd);
    if (e) notify(0, "Eject failed", d->node, strerror(e));
    else   notify(1, "Ejected", d->node, "safe to unplug");
}

/* filesystem type straight from the superblock (a fresh loop device has no udev data yet) */
static int probe(const char *dev, char *out, size_t cap) {
    blkid_probe pr = blkid_new_probe_from_filename(dev);
    const char *t = NULL;

    out[0] = '\0';
    if (!pr) return -1;
    blkid_probe_enable_superblocks(pr, 1);
    blkid_probe_set_superblocks_flags(pr, BLKID_SUBLKS_TYPE);
    if (!blkid_do_safeprobe(pr)) blkid_probe_lookup_value(pr, "TYPE", &t, NULL);
    if (t) cat(out, cap, t, NULL);
    blkid_free_probe(pr);
    return !*out;
}

/* attach img to a free loop device with autoclear, so it dies with its last user.
 * returns an fd that keeps it alive (node = /dev/loopN), -1 on error */
static int attach(const char *img, char *node, size_t cap) {
    struct loop_info64 li;
    char nb[11];
    int ctl, n, lfd, ifd;

    if ((ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC)) < 0) return -1;
    n = ioctl(ctl, LOOP_CTL_GET_FREE);
    close(ctl);
    if (n < 0) return -1;
    cat(node, cap, "/dev/loop", num(nb, n), NULL);
    if ((lfd = open(node, O_RDWR | O_CLOEXEC)) < 0) return -1;
    ifd = rdonly ? -1 : open(img, O_RDWR | O_CLOEXEC);
    if (ifd < 0) ifd = open(img, O_RDONLY | O_CLOEXEC); /* the kernel marks the loop ro by itself then */
    if (ifd < 0 || ioctl(lfd, LOOP_SET_FD, ifd) < 0) {
        if (ifd >= 0) close(ifd);
        close(lfd);
        return -1;
    }
    close(ifd);
    memset(&li, 0, sizeof li);
    li.lo_flags = LO_FLAGS_AUTOCLEAR;
    if (ioctl(lfd, LOOP_SET_STATUS64, &li) < 0) {
        ioctl(lfd, LOOP_CLR_FD, 0);
        close(lfd);
        return -1;
    }
    return lfd;
}

/* loop-mount an image that carries a filesystem directly (iso, raw fs images) */
static void mount_image(const char *img) {
    BlockDev d;
    int fd;

    memset(&d, 0, sizeof d);
    if ((fd = attach(img, d.node, sizeof d.node)) < 0) {
        notify(0, "Image failed", img, strerror(errno));
        return;
    }
    cat(d.name, sizeof d.name, strrchr(img, '/') ? strrchr(img, '/') + 1 : img, NULL);
    if (probe(d.node, d.fs, sizeof d.fs))
        notify(0, "Image failed", img, "no filesystem found (partitioned image?)");
    else
        do_mount(&d, 0);
    close(fd); /* autoclear: the loop device now lives exactly as long as the mount */
}

/* key actions, bound in config.h */
void do_image(void) {
    char in[256], img[256];

    if (!prompt("Disk image", "path to an .iso / .img with a filesystem", in, sizeof in, 0)) return;
    expand(img, sizeof img, in);
    mount_image(img);
}

void quit(void)        { running = 0; }
void selup(void)       { if (sel > 0) sel--; }
void seldown(void)     { if (sel < count - 1) sel++; }
void toggleall(void)   { showall = !showall; rescan(); }
void togglero(void)    { rdonly = !rdonly; }
void mountsel(void)    { if (count) do_mount(&devs[sel], 0); }
void mountcustom(void) { if (count) do_mount(&devs[sel], 1); }
void umountsel(void)   { if (count) do_unmount(&devs[sel]); }
void ejectsel(void)    { if (count) do_eject(&devs[sel]); }

/* everything we show, but never the hidden internal disks */
void umountall(void) {
    char b[11];
    int i, f;

    for (i = f = 0; i < count; i++)
        if ((devs[i].mnt[0] || devs[i].open) && !devs[i].hid)
            f += do_unmount(&devs[i]);
    if (f) cat(status, sizeof status, num(b, f), " failed to unmount", NULL);
    else   cat(status, sizeof status, "Everything unmounted", NULL);
}

static void key(int ch) {
    size_t i;

    for (i = 0; i < LENGTH(keys); i++)
        if (keys[i].ch == ch) {
            keys[i].func();
            return;
        }
}

int main(void) {
    struct pollfd p[3];
    struct udev_monitor *mon;
    struct udev_device *e;
    const char *s = getenv("SUDO_UID"), *u = getenv("DOAS_USER");
    struct passwd *pw;
    int ch, rc;

    if (geteuid()) {
        fputs("slmount: needs root, run it with sudo or doas\n", stderr);
        return 1;
    }
    /* who invoked us? (sudo/doas keep $HOME pointing at root's) */
    pw = s ? getpwuid(strtoul(s, NULL, 10)) : u ? getpwnam(u) : getpwuid(getuid());
    if (!pw || !(udev = udev_new())) {
        fputs("slmount: can't resolve user / init udev\n", stderr);
        return 1;
    }
    ruid = pw->pw_uid;
    rgid = pw->pw_gid;
    cat(homedir, sizeof homedir, pw->pw_dir, NULL);
    cat(base, sizeof base, homedir, "/mnt", NULL);
    crypt_set_log_callback(NULL, nolog, NULL);
    kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);

    /* hotplug: udev netlink events + mount table changes, all in one poll() */
    mon = udev_monitor_new_from_netlink(udev, "udev");
    if (mon) {
        udev_monitor_filter_add_match_subsystem_devtype(mon, "block", NULL);
        udev_monitor_enable_receiving(mon);
    }
    p[0] = (struct pollfd){ STDIN_FILENO, POLLIN, 0 };
    p[1] = (struct pollfd){ mon ? udev_monitor_get_fd(mon) : -1, POLLIN, 0 };
    p[2] = (struct pollfd){ open("/proc/self/mounts", O_RDONLY | O_CLOEXEC), POLLPRI, 0 };

    initscr();
    cbreak();
    noecho();
    curs_set(0);
    set_escdelay(25); /* Esc cancels the prompt without a 1s lag */
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    rescan();

    while (running) {
        draw();
        rc = poll(p, 3, 5000); /* the timeout keeps the free-space column fresh */
        if (p[1].revents)
            while ((e = udev_monitor_receive_device(mon)))
                udev_device_unref(e);
        if (!rc || p[1].revents || p[2].revents)
            rescan();
        while (running && (ch = getch()) != ERR) /* also eats KEY_RESIZE */
            key(ch);
    }

    endwin();
    if (mon) udev_monitor_unref(mon);
    udev_unref(udev);
    return 0;
}

