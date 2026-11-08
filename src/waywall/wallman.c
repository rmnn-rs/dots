#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <limits.h>
#include <errno.h>
#include <ctype.h>
#include <sys/resource.h>

#include <dirent.h>
#include "wall.h"
#include "walldb.h"
#include "wallwatch.h"

#define USAGE \
    "Usage: wallman [options] [command]\n" \
    "Options:\n" \
    "  -D, --daemon          start the daemon (it keeps the database in step with the folder by itself)\n" \
    "  -M, --manual          with -D: no automatic scan / prune, only when you ask\n" \
    "  -S, --display <d>     Wayland display to use (default $WAYLAND_DISPLAY, e.g. wayland-0)\n" \
    "  -y, --interval <t>    slideshow: change every <t> (30, 5m); give it with -n, -p or -r\n" \
    "  -j, --jump <n>        jump to wall n in the current view\n" \
    "  -i, --status          print \"view | n/total\"\n" \
    "  -h, --help            this text\n" \
    "Commands:\n" \
    "  -n, --next  -p, --prev  -r, --rand  -u, --fnext  -d, --fprev  -s, --save  -R, --reload\n" \
    "  -v, --view <*|?|Category|@tag>      -P, --purity <1-3|SFW|Sketchy|NSFW|+|->\n" \
    "  -o, --set <path>      draw any image; a library file also becomes the current one\n" \
    "  -I, --info            details of the wall on screen\n" \
    "  -g, --rate <0-3|SFW|..> [Category|@tag]   rate it, or a whole category / tag\n" \
    "  -C, --ct <Category|-> [folder [SFW|..]]   set its category (or a whole folder's)\n" \
    "  -U, --unct <Category> remove a category from every wall\n" \
    "  -m, --mov <n|+n|-n|first|last>            move it inside its category\n" \
    "  -N, --name <text>   -t, --tag <t..>   -T, --untag <t..>\n" \
    "  -l, --list [view]   -c, --cats   -e, --tiers    print to stdout, tab-separated\n" \
    "  -f, --scan [Folder=N|Folder=- ...]   add new files (Folder=N: that folder is a purity level, remembered)\n" \
    "  -x, --prune           drop database rows whose file is gone\n"

char sock_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static int serv_fd = -1;
static int slideshow_interval = 0;
static time_t next_slide;                    /* CLOCK_MONOTONIC second the slideshow advances at */
static char current_mode = 'n';
extern char current_folder[256];
extern int count, cur;

/* ── Helpers ──────────────────────────────────────────────────────────── */

static time_t now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec;
}

static int fast_utoa(int val, char *buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char tmp[12];
    int i = 0;
    while (val > 0) { tmp[i++] = (char)((val % 10) + '0'); val /= 10; }
    int len = i;
    char *p = buf;
    while (i > 0) { *p++ = tmp[--i]; }
    *p = '\0';
    return len;
}

static int valid_display(const char *d) {
    size_t n = 0;
    if (!d || *d == '\0' || *d == '.') return 0;
    for (; *d; d++, n++)
        if (!isalnum((unsigned char)*d) && *d != '-' && *d != '_' && *d != '.') return 0;
    return n < 64;
}

/*
 * get_sock_path — builds the UNIX socket path via memcpy.
 * Format: <XDG_RUNTIME_DIR>/wl-<uid>.<display>.sock
 */
void get_sock_path(char *dest, size_t len, const char *display) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || runtime[0] != '/') {
        (void)write(STDERR_FILENO, "Error: XDG_RUNTIME_DIR not set\n", 31);
        exit(1);
    }

    if (!display) display = "wayland-0";
    if (!valid_display(display)) {
        (void)write(STDERR_FILENO, "Error: Invalid WAYLAND_DISPLAY value\n", 37);
        exit(1);
    }

    char uid_str[12];
    int  ulen    = fast_utoa((int)getuid(), uid_str);
    size_t rlen  = strlen(runtime);
    size_t dlen  = strlen(display);

    /* runtime + "/wl-" + uid + "." + display + ".sock" + '\0' */
    size_t total = rlen + 4 + (size_t)ulen + 1 + dlen + 5;
    if (total >= len) {
        (void)write(STDERR_FILENO, "Error: Socket path too long\n", 28);
        exit(1);
    }

    char *ptr = dest;
    memcpy(ptr, runtime,  rlen); ptr += rlen;
    memcpy(ptr, "/wl-",   4);    ptr += 4;
    memcpy(ptr, uid_str,  ulen); ptr += ulen;
    *ptr++ = '.';
    memcpy(ptr, display,  dlen); ptr += dlen;
    memcpy(ptr, ".sock",  6);    /* includes '\0' */
}

/* ── Seccomp-BPF sandbox ──────────────────────────────────────────────── */

#ifdef __linux__
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <stdint.h>
#include <stddef.h>

#define SC_ALLOW(nr) \
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, (nr), 0, 1), \
    BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW)

#define SC_KILL(nr) \
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, (nr), 0, 1), \
    BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS)

static void seccomp_apply(void) {
    struct sock_filter filter[] = {
        /* Verify arch, kill if not x86-64 */
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
                 offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS),

        /* Load syscall number */
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
                 offsetof(struct seccomp_data, nr)),

        /* ── Hard kills ── */
        SC_KILL(__NR_ptrace),
        SC_KILL(__NR_process_vm_readv),
        SC_KILL(__NR_process_vm_writev),
        SC_KILL(__NR_kexec_load),
        SC_KILL(__NR_init_module),
        SC_KILL(__NR_finit_module),
        SC_KILL(__NR_delete_module),
        SC_KILL(__NR_create_module),

        /* ── Allowlist ── */

        /* Core I/O */
        SC_ALLOW(__NR_read),
        SC_ALLOW(__NR_write),
        SC_ALLOW(__NR_readv),
        SC_ALLOW(__NR_writev),
        SC_ALLOW(__NR_close),
        SC_ALLOW(__NR_fstat),
        SC_ALLOW(__NR_lstat),
        SC_ALLOW(__NR_stat),

        /* File access */
        SC_ALLOW(__NR_open),
        SC_ALLOW(__NR_openat),
        SC_ALLOW(__NR_getdents64),  /* scandir */
        SC_ALLOW(__NR_lseek),
        SC_ALLOW(__NR_pread64),

        /* Wayland shm buffers (memfd + mmap, fd passed over sendmsg) */
        SC_ALLOW(__NR_memfd_create),
        SC_ALLOW(__NR_ftruncate),

        /* Memory */
        SC_ALLOW(__NR_mmap),
        SC_ALLOW(__NR_munmap),
        SC_ALLOW(__NR_mprotect),
        SC_ALLOW(__NR_mremap),      /* Imlib2 */
        SC_ALLOW(__NR_brk),

        /* Sockets — Wayland + Unix domain */
        SC_ALLOW(__NR_socket),
        SC_ALLOW(__NR_connect),
        SC_ALLOW(__NR_accept4),
        SC_ALLOW(__NR_bind),
        SC_ALLOW(__NR_listen),
        SC_ALLOW(__NR_getsockopt),
        SC_ALLOW(__NR_setsockopt),
        SC_ALLOW(__NR_getsockname),
        SC_ALLOW(__NR_sendmsg),
        SC_ALLOW(__NR_recvmsg),
        SC_ALLOW(__NR_sendto),
        SC_ALLOW(__NR_recvfrom),

        /* select/poll/signal */
        SC_ALLOW(__NR_select),
        SC_ALLOW(__NR_pselect6),
        SC_ALLOW(__NR_poll),
        SC_ALLOW(__NR_ppoll),
        SC_ALLOW(__NR_rt_sigreturn),
        SC_ALLOW(__NR_rt_sigaction),
        SC_ALLOW(__NR_rt_sigprocmask),
        SC_ALLOW(__NR_signalfd4),

        /* Process/misc */
        SC_ALLOW(__NR_getuid),
        SC_ALLOW(__NR_geteuid),
        SC_ALLOW(__NR_getpid),
        SC_ALLOW(__NR_exit),
        SC_ALLOW(__NR_exit_group),
        SC_ALLOW(__NR_futex),
        SC_ALLOW(__NR_set_robust_list),
        SC_ALLOW(__NR_fcntl),
        SC_ALLOW(__NR_ioctl),
        SC_ALLOW(__NR_unlink),      /* socket cleanup */
        SC_ALLOW(__NR_rename),      /* atomic save */
        SC_ALLOW(__NR_fsync),
        SC_ALLOW(__NR_umask),
        SC_ALLOW(__NR_getcwd),
        SC_ALLOW(__NR_clock_gettime),
        SC_ALLOW(__NR_nanosleep),
        SC_ALLOW(__NR_prlimit64),
        SC_ALLOW(__NR_newfstatat),

        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS),
    };

    struct sock_fprog prog = {
        .len    = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0)
        fprintf(stderr, "seccomp: filter failed: %s\n", strerror(errno));
}

#undef SC_ALLOW
#undef SC_KILL

#endif /* __linux__ */

/* ── IPC message ──────────────────────────────────────────────────────── */

typedef struct {
    char cmd;
    int  interval;
    int  target;
    char arg[256];
} WallMsg;

/* ── Status line + subscribers ────────────────────────────────────────── */

/* "view | n/total": what -i prints and what subscribers are pushed. Returns its length. */
static size_t status_line(char *resp, size_t rsz) {
    char cur_str[12], tot_str[12];
    int  clen = fast_utoa(cur + 1, cur_str);
    int  tlen = fast_utoa(count,   tot_str);
    size_t flen = strlen(current_folder);
    size_t needed = 3 + clen + 1 + tlen;

    if (flen > rsz - needed)
        flen = rsz - needed;

    char *ptr = resp;
    memcpy(ptr, current_folder, flen); ptr += flen;
    memcpy(ptr, " | ", 3);             ptr += 3;
    memcpy(ptr, cur_str, (size_t)clen);ptr += clen;
    *ptr++ = '/';
    memcpy(ptr, tot_str, (size_t)tlen);ptr += tlen;
    return (size_t)(ptr - resp);
}

/* A client that sends cmd 'w' stays connected and is pushed the status line plus '\n' every time it
 * changes (the bar listens instead of polling). Sockets are never blocked on: a subscriber that is
 * dead or not reading is dropped, and it resyncs from the line it gets when it reconnects. */
#define MAX_SUBS 8
#define STATUS_MSG_MAX 513                       /* status line (512) + '\n' */
static int    subs[MAX_SUBS];
static int    nsubs;
static char   last_msg[STATUS_MSG_MAX];
static size_t last_len;

static void sub_drop(int i) { close(subs[i]); subs[i] = subs[--nsubs]; }

static size_t status_msg(char *msg, size_t msz) {
    size_t n = status_line(msg, msz - 1);
    msg[n] = '\n';
    return n + 1;
}

/* after anything that may have changed the wall: tell the subscribers if the status line did */
static void push_status(void) {
    char msg[STATUS_MSG_MAX];
    size_t n = status_msg(msg, sizeof(msg));

    if (n == last_len && !memcmp(msg, last_msg, n)) return;
    memcpy(last_msg, msg, n);
    last_len = n;
    for (int i = nsubs; i-- > 0; )
        if (send(subs[i], msg, n, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)n)
            sub_drop(i);
}

/* ── Signal handling ──────────────────────────────────────────────────── */

static volatile sig_atomic_t running = 1;
void handle_sig(int sig) { (void)sig; running = 0; }

/* ── Commands ---------─────────────────────────────────────────────────── */

void run_command(char cmd, int target, const char *arg) {
    Arg fwd = { .i =  1 };
    Arg bwd = { .i = -1 };
    switch (cmd) {
        case 'n': wall_cycle(&fwd);        break;
        case 'p': wall_cycle(&bwd);        break;
        case 'r': wall_random(NULL);       break;
        case 's': wall_save(NULL);         break;
        case 'u': wall_folder_select(&fwd);break;
        case 'd': wall_folder_select(&bwd);break;
        case 'R': wall_reload(NULL);       break;
        case 'P': { Arg p = { .i = target }; wall_purity(&p); } break;
        case '+': wall_purity_step(&fwd);  break;
        case '-': wall_purity_step(&bwd);  break;
        case 'V': wall_view(arg);          break;
        case 'J': wall_jump(arg);          break;
        case 'S': wall_show(arg);          break;
        case 'j':
            if (target > 0 && target <= count) {
                cur = target - 1;
                wall_cycle(NULL);
            }
            break;
    }
}

/* ── Duration parser (e.g. "30", "5m") ───────────────────────────────── */

static int parse_duration(const char *str) {
    char *endptr;
    long val = strtol(str, &endptr, 10);
    if (endptr == str || val < 0) return 0;
    if (*endptr == 'm') return (val > INT_MAX / 60) ? INT_MAX : (int)(val * 60);
    return (val > INT_MAX) ? INT_MAX : (int)val;
}

/* a flag in both of its forms: is(arg, "-n", "--next") */
static int is(const char *a, const char *s, const char *l) {
    return !strcmp(a, s) || !strcmp(a, l);
}

/* ── IPC client ───────────────────────────────────────────────────────── */

/* One request per connection. Reads a reply only when resp is given. Returns bytes read, -1 on failure. */
static int ipc(const struct sockaddr_un *addr, const WallMsg *msg, char *resp, size_t rsz) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (const struct sockaddr *)addr, sizeof(*addr)) < 0) {
        fputs("Error: could not connect to daemon. Is it running?\n", stderr);
        if (fd >= 0) close(fd);
        return -1;
    }
    int n = 0;
    if (write(fd, msg, sizeof(WallMsg)) != (ssize_t)sizeof(WallMsg)) n = -1;
    else if (resp) {
        n = (int)read(fd, resp, rsz - 1);
        if (n < 0) n = 0;
        resp[n] = '\0';
    }
    close(fd);
    return n;
}

/* The watcher asks the daemon to reload through here (it runs in its own process, before the sandbox) */
static struct sockaddr_un watch_addr;
static void notify_daemon(void) {
    WallMsg r = { .cmd = 'R', .interval = -1 };
    ipc(&watch_addr, &r, NULL, 0);
}

/* Unrated wallpapers only show in view '?', which is easy to mistake for a command that did nothing */
static void unrated_hint(const char *cat) {
    int u = walldb_unrated(cat);
    if (u > 0) printf("%d unrated: hidden outside view '?' until rated (wallman --rate <1-3> <Category|@tag>)\n", u);
}

/* ── set <path>: show any image ─────────────────────────────────────────── */

#define STAGE_NAME ".wallman-temp"

/* The daemon is sandboxed to the master dir, so a file from elsewhere is copied to
 * <master>/.wallman-temp<.ext> (hidden, so scan ignores it) and drawn from there. The real
 * extension is kept so Imlib2 picks the same loader it uses for library files of that type.
 * Fills name with the path relative to the master dir. */
static int stage_copy(const char *root, const char *src, char *name, size_t namesz) {
    char dst[PATH_MAX];
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    const char *dot = strrchr(base, '.');
    size_t el = (dot && dot != base) ? strlen(dot) : 0;                 /* ".png" */
    for (size_t i = 1; i < el; i++)
        if (!isalnum((unsigned char)dot[i])) el = 0;
    if (el > 9) el = 0;                                                 /* not a real extension */

    size_t nl = sizeof(STAGE_NAME) - 1, rl = strlen(root);
    if (nl + el >= namesz || rl + 1 + nl + el >= sizeof(dst)) return -1;
    memcpy(name, STAGE_NAME, nl);
    for (size_t i = 0; i < el; i++) name[nl + i] = (char)tolower((unsigned char)dot[i]);
    name[nl + el] = '\0';

    DIR *d = opendir(root);                                             /* drop the previous staged copy (any extension) */
    for (struct dirent *e; d && (e = readdir(d)); )
        if (!strncmp(e->d_name, STAGE_NAME, nl)) unlinkat(dirfd(d), e->d_name, 0);
    if (d) closedir(d);

    memcpy(dst, root, rl);
    dst[rl] = '/';
    memcpy(dst + rl + 1, name, nl + el + 1);

    int rc = -1;
    int in = open(src, O_RDONLY | O_CLOEXEC);
    int out = in < 0 ? -1 : open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out >= 0) {
        char buf[65536];
        ssize_t n;
        rc = 0;
        while (rc == 0 && (n = read(in, buf, sizeof(buf))) > 0)
            if (write(out, buf, (size_t)n) != n) rc = -1;
        if (rc == 0 && n < 0) rc = -1;
    }
    if (in >= 0) close(in);
    if (out >= 0 && close(out) < 0) rc = -1;
    return rc;
}

/* ── Database commands: edit the current wallpaper's row, then tell the daemon ── */

static int db_command(const struct sockaddr_un *addr, int argc, char **argv) {
    static const struct { const char *s, *l; } cmds[] = {          /* short and long form of every database command */
        { "-I", "--info" }, { "-f", "--scan" }, { "-x", "--prune" }, { "-U", "--unct" }, { "-g", "--rate" },
        { "-C", "--ct" },   { "-N", "--name" }, { "-t", "--tag" },   { "-T", "--untag" }, { "-o", "--set" },
        { "-m", "--mov" },  { "-l", "--list" }, { "-c", "--cats" },  { "-e", "--tiers" },
    };
    const char *c = NULL;
    for (size_t k = 0; k < sizeof(cmds) / sizeof(*cmds); k++)
        if (is(argv[0], cmds[k].s, cmds[k].l)) c = cmds[k].l;      /* from here on only the long names exist */
    if (!c) return -1;                                              /* not a database command */

    if (!strcmp(c, "--list") || !strcmp(c, "--cats") || !strcmp(c, "--tiers")) {   /* works without the daemon */
        if (walldb_open_rw(get_save_path()) < 0) return 1;                  /* read-write only so an old database gets migrated */
        int n = !strcmp(c, "--list") ? walldb_print_walls(argc > 1 ? argv[1] : "*")
              : !strcmp(c, "--cats") ? walldb_print_cats() : walldb_print_tiers();
        return n < 0 ? 1 : 0;
    }

    char resp[1024 + PATH_MAX + 2];                             /* "<master dir>\n<current path>" */
    WallMsg q = { .cmd = 'c', .interval = -1 };
    if (ipc(addr, &q, resp, sizeof(resp)) < 0) return 1;
    char *path = strchr(resp, '\n');
    if (!path) return 1;
    *path++ = '\0';

    if (walldb_open_rw(get_save_path()) < 0) return 1;

    int rc = 0;
    if (!strcmp(c, "--scan")) {
        int skipped = 0, n = walldb_scan(resp, (const char *const *)argv + 1, argc - 1, &skipped);
        if (n < 0) { fputs("Error: scan failed\n", stderr); return 1; }
        printf("scan: %d new", n);
        if (skipped) printf(", %d skipped (not jpg/png/jpeg/webp/ff)", skipped);
        printf("\n");
        unrated_hint(NULL);
    } else if (!strcmp(c, "--set")) {                            /* set <path>: a library file becomes the current one, anything else is just drawn */
        char *rp = argc > 1 ? realpath(argv[1], NULL) : NULL, *rr = realpath(resp, NULL);
        if (!rp || !rr) {
            fprintf(stderr, "Error: cannot read %s\n", argc > 1 ? argv[1] : "(no path given)");
            free(rp); free(rr);
            return 1;
        }
        size_t rl = strlen(rr);
        int slash = rr[rl - 1] == '/';                          /* only when the master dir is "/" */
        const char *rel = (!strncmp(rp, rr, rl) && (slash ? rp[rl] : rp[rl] == '/')) ? rp + rl + !slash : NULL;

        WallMsg s = { .interval = -1, .cmd = 'S' };
        char staged[32];
        const char *send = rel;
        if (rel && walldb_exists(rel)) s.cmd = 'J';
        else if (rel) fputs("note: not in the database yet (wallman --scan adds it)\n", stderr);
        else if (stage_copy(rr, rp, staged, sizeof(staged)) == 0) send = staged;
        else { fprintf(stderr, "Error: could not copy %s into %s\n", rp, rr); free(rp); free(rr); return 1; }

        size_t sl = strlen(send);
        int fits = sl < sizeof(s.arg);
        if (fits) memcpy(s.arg, send, sl + 1);
        else fputs("Error: path too long\n", stderr);
        free(rp); free(rr);
        return (fits && ipc(addr, &s, NULL, 0) >= 0) ? 0 : 1;
    } else if (!strcmp(c, "--prune")) {
        int n = walldb_prune(resp);
        if (n < 0) return 1;
        printf("pruned %d\n", n);
    } else if (!strcmp(c, "--unct")) {                          /* unct <Category>: remove it from every wallpaper */
        int n = argc > 1 ? walldb_clear_category(argv[1]) : -1;
        if (n < 0) { fputs("Error: usage: --unct <Category>\n", stderr); return 1; }
        printf("cleared %d\n", n);
    } else if (!strcmp(c, "--ct") && argc > 2) {                /* ct <Category> <folder> [level]: a physical folder; level rates the unrated ones */
        int lvl = argc > 3 ? walldb_level(argv[3]) : 0, rated = 0;
        int n = (argc > 3 && lvl < 1) ? -1 : walldb_set_category_folder(argv[2], argv[1], lvl, &rated);
        if (n < 0) { fputs("Error: --ct failed (usage: --ct <Category|-> <folder> [SFW|Sketchy|NSFW])\n", stderr); return 1; }
        printf("categorized %d\n", n);
        if (rated) printf("rated %d unrated as %s\n", rated, argv[3]);
        unrated_hint(argv[1]);
    } else if (!strcmp(c, "--rate") && argc > 2) {               /* rate <0-3> <Category|@tag>: bulk */
        int n = walldb_rate_view(argv[2], walldb_level(argv[1]));
        if (n < 0) { fputs("Error: usage: --rate <0-3> <Category|@tag>\n", stderr); return 1; }
        printf("rated %d\n", n);
    } else if (!path[0]) {
        fputs("Error: no current wallpaper\n", stderr);
        return 1;
    } else if (!strcmp(c, "--info")) {
        if (walldb_info(path) < 0) { fputs("Error: not in the database, run: wallman --scan\n", stderr); return 1; }
        return 0;
    } else if (argc < 2) {
        fprintf(stderr, "Error: %s needs an argument\n", c);
        return 1;
    } else if (!strcmp(c, "--rate")) {
        rc = walldb_rate(path, walldb_level(argv[1]));
    } else if (!strcmp(c, "--mov")) {                            /* mov <n|+n|-n|first|last>: move the current wallpaper inside its category */
        int r = walldb_move(path, argv[1]);
        if (r == -2) { fputs("Error: no category, use --ct first\n", stderr); return 1; }
        if (r > 0) printf("position: %d\n", r);
        rc = r > 0 ? 0 : -1;
    } else if (!strcmp(c, "--ct")) {
        rc = walldb_set_category(path, argv[1]);
    } else if (!strcmp(c, "--name")) {
        rc = walldb_set_name(path, argv[1]);
    } else {                                                    /* tag / untag, any number of tags */
        for (int i = 1; i < argc; i++)
            if (walldb_tag(path, argv[i], !strcmp(c, "--tag")) < 0) rc = -1;
    }
    if (rc < 0) { fputs("Error: update failed (not in the database, or bad value)\n", stderr); return 1; }

    WallMsg r = { .cmd = 'R', .interval = -1 };
    ipc(addr, &r, NULL, 0);
    return 0;
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    const char *display   = getenv("WAYLAND_DISPLAY");
    int target_interval   = 0;
    int jump_target       = 0;
    int query_mode        = 0;
    int start_daemon      = 0;
    int manual            = 0;                   /* -M: no automatic scan / prune */
    int argi              = 1;

    while (argi < argc && argv[argi][0] == '-') {
        if      (is(argv[argi], "-S", "--display")  && argi + 1 < argc) display = argv[++argi];
        else if (is(argv[argi], "-y", "--interval") && argi + 1 < argc) target_interval = parse_duration(argv[++argi]);
        else if (is(argv[argi], "-j", "--jump")     && argi + 1 < argc) {
            char *end;
            long val = strtol(argv[++argi], &end, 10);
            jump_target = (*end != '\0' || val <= 0) ? -1 : (int)val;
        }
        else if (is(argv[argi], "-i", "--status"))   query_mode   = 1;
        else if (is(argv[argi], "-D", "--daemon"))   start_daemon = 1;
        else if (is(argv[argi], "-M", "--manual"))   manual       = 1;
        else if (is(argv[argi], "-h", "--help")) {
            (void)write(STDOUT_FILENO, USAGE, sizeof(USAGE) - 1);
            return 0;
        } else break;
        argi++;
    }

    if (argi < argc && !strcmp(argv[argi], "daemon")) { start_daemon = 1; argi++; }

    if (argc == 1) {
        (void)write(STDOUT_FILENO, USAGE, sizeof(USAGE) - 1);
        return 0;
    }

    if (jump_target == -1) {
        (void)write(STDERR_FILENO, "Error: -j requires a positive integer\n", 38);
        return 1;
    }

    get_sock_path(sock_path, sizeof(sock_path), display);

    /* Build sockaddr */
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    size_t slen = strlen(sock_path);
    if (slen >= sizeof(addr.sun_path)) return 1;
    memcpy(addr.sun_path, sock_path, slen + 1);

    /* ── Client mode ───────────────────────────────────────────────────── */
    if (argi < argc || jump_target > 0 || query_mode) {
        int rc = (argi < argc && !query_mode) ? db_command(&addr, argc - argi, argv + argi) : -1;
        if (rc >= 0) return rc;

        WallMsg msg = { .interval = target_interval, .target = jump_target };
        if (query_mode)      msg.cmd = 'i';
        else if (jump_target > 0) msg.cmd = 'j';
        else {
            const char *arg = argv[argi];
            const char *val = (argi + 1 < argc) ? argv[argi + 1] : "";
            if      (is(arg, "-n", "--next")) msg.cmd = 'n';
            else if (is(arg, "-p", "--prev")) msg.cmd = 'p';
            else if (is(arg, "-r", "--rand")) msg.cmd = 'r';
            else if (is(arg, "-u", "--fnext")) msg.cmd = 'u';
            else if (is(arg, "-d", "--fprev")) msg.cmd = 'd';
            else if (is(arg, "-s", "--save")) msg.cmd = 's';
            else if (is(arg, "-R", "--reload")) msg.cmd = 'R';
            else if (is(arg, "-P", "--purity")) {
                msg.interval = -1;                              /* leave the slideshow timer alone */
                if      (!strcmp(val, "+")) msg.cmd = '+';
                else if (!strcmp(val, "-")) msg.cmd = '-';
                else { msg.cmd = 'P'; msg.target = walldb_level(val); }
            }
            else if (is(arg, "-v", "--view")) {
                msg.cmd = 'V';
                msg.interval = -1;
                size_t vl = strlen(val);
                if (vl < sizeof(msg.arg)) memcpy(msg.arg, val, vl + 1);
            }
            else if (arg[0] && !arg[1] && strchr("nprudsR", arg[0])) msg.cmd = arg[0];   /* the old one-letter forms */
            else {
                fprintf(stderr, "Error: unknown command '%s' (wallman -h)\n", arg);
                return 1;
            }
        }

        char resp[512];
        int n = ipc(&addr, &msg, query_mode ? resp : NULL, sizeof(resp));
        if (n < 0) return 1;
        if (query_mode && n > 0) {
            (void)write(STDOUT_FILENO, resp, (size_t)n);
            (void)write(STDOUT_FILENO, "\n", 1);
        }
        return 0;
    }

    /* ── Daemon mode ───────────────────────────────────────────────────── */
    if (!start_daemon) return 0;

    /* Bail out if a daemon is already running on this display */
    int test_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (test_fd >= 0) {
        if (connect(test_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            (void)write(STDERR_FILENO, "Error: daemon already running\n", 30);
            close(test_fd);
            return 1;
        }
        close(test_fd);
    }

    unlink(sock_path);
    serv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (serv_fd < 0) return 1;
    fcntl(serv_fd, F_SETFD, FD_CLOEXEC);
    srand((unsigned int)time(NULL));

    struct sigaction sa      = { .sa_handler = handle_sig };
    struct sigaction sa_chld = { .sa_handler = SIG_DFL    };
    struct sigaction sa_pipe = { .sa_handler = SIG_IGN    };   /* a client that left before the reply must not kill us */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa,      NULL);
    sigaction(SIGTERM, &sa,      NULL);
    sigaction(SIGCHLD, &sa_chld, NULL);
    sigaction(SIGPIPE, &sa_pipe, NULL);

    umask(0077);
    if (bind(serv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(serv_fd, 5) < 0) {
        close(serv_fd);
        return 1;
    }

    wall_setup_renderer();
    wall_restore();
    push_status();                                   /* baseline for change detection */

    if (!manual && master_dir[0]) {                  /* before the sandbox: the watcher is its own, unsandboxed process */
        watch_addr = addr;
        wallwatch_start(master_dir, get_save_path(), notify_daemon);
    }

#ifdef __linux__
    struct rlimit rl_zero = {0, 0};
    setrlimit(RLIMIT_NPROC, &rl_zero);

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        fprintf(stderr, "sandbox: PR_SET_NO_NEW_PRIVS failed: %s\n", strerror(errno));
    }

    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

    seccomp_apply();
#endif

    /* ── Event loop ────────────────────────────────────────────────────── */
    while (running) {
        if (wall_wl_dispatch() < 0) break;                /* compositor events first (before sleeping: some may be queued already); -1 = compositor gone */

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(serv_fd, &fds);
        int wfd = wall_wl_fd();
        FD_SET(wfd, &fds);
        int maxfd = serv_fd > wfd ? serv_fd : wfd;
        for (int i = 0; i < nsubs; i++) {
            FD_SET(subs[i], &fds);
            if (subs[i] > maxfd) maxfd = subs[i];
        }

        struct timeval  tv      = { 0 };
        struct timeval *timeout = NULL;
        if (slideshow_interval > 0) {                     /* a deadline, not a countdown: status polls and the like don't postpone it */
            time_t left = next_slide - now();
            tv.tv_sec = left > 0 ? left : 0;
            timeout = &tv;
        }

        int activity = select(maxfd + 1, &fds, NULL, NULL, timeout);
        if (activity < 0) { if (errno == EINTR) continue; break; }
        if (slideshow_interval > 0 && now() >= next_slide) {
            run_command(current_mode, 0, NULL);
            push_status();
            next_slide = now() + slideshow_interval;
        }
        if (activity == 0) continue;

        for (int i = nsubs; i-- > 0; )                    /* subscribers never send: readable means they hung up */
            if (FD_ISSET(subs[i], &fds)) sub_drop(i);

        if (!FD_ISSET(serv_fd, &fds)) continue;

        int client_fd = accept4(serv_fd, NULL, NULL, SOCK_CLOEXEC);
        if (client_fd < 0) continue;

        /* Reject connections from other users */
        struct ucred cred;
        socklen_t    ulen = sizeof(cred);
        if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &ulen) != 0 ||
            cred.uid != getuid()) {
            close(client_fd);
            continue;
        }

        struct timeval rcv_to = { .tv_sec = 1 };          /* a client that connects and never sends must not hang the daemon */
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to));

        WallMsg m = {0};
        int keep = 0;                                     /* a subscriber's connection stays open */

        if (recv(client_fd, &m, sizeof(WallMsg), 0) == sizeof(WallMsg)) {
            if (m.cmd == 'i') {
                char resp[512];
                (void)write(client_fd, resp, status_line(resp, sizeof(resp)));
            }
            else if (m.cmd == 'w') {                      /* subscribe: keep the connection, see push_status() */
                char msg[STATUS_MSG_MAX];
                size_t n = status_msg(msg, sizeof(msg));
                if (nsubs < MAX_SUBS && send(client_fd, msg, n, MSG_DONTWAIT | MSG_NOSIGNAL) == (ssize_t)n) {
                    subs[nsubs++] = client_fd;
                    keep = 1;
                }
            }
            else if (m.cmd == 'c') {                      /* "<master dir>\n<current path>", for the CLI */
                char resp[1024 + PATH_MAX + 2];
                const char *p = wall_current();
                size_t rl = strlen(master_dir), pl = p ? strlen(p) : 0;
                if (rl + 1 + pl <= sizeof(resp)) {
                    memcpy(resp, master_dir, rl);
                    resp[rl] = '\n';
                    if (pl) memcpy(resp + rl + 1, p, pl);
                    (void)write(client_fd, resp, rl + 1 + pl);
                }
            }
            else if (m.cmd && strchr("nprsudRjP+-VJS", m.cmd)) {
                if (m.interval >= 0) {
                    slideshow_interval = (m.interval > 86400) ? 86400 : m.interval;
                    next_slide = now() + slideshow_interval;
                }
                if (strchr("npr", m.cmd))
                    current_mode = m.cmd;
                m.arg[sizeof(m.arg) - 1] = '\0';
                run_command(m.cmd, m.target, m.arg);
                push_status();
            }
        }
        if (!keep) close(client_fd);
    }

    unlink(sock_path);
    close(serv_fd);
    return 0;
}

