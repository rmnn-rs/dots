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
#include <libgen.h>
#include <ctype.h>
#include <sys/resource.h>

#include <dirent.h>
#include "wall.h"
#include "walldb.h"
#include "wallwatch.h"

#define USAGE \
    "Usage: wallman [-S :display] [-y dur] [-j idx] [-i] <cmd>\n" \
    "       wallman -D [-M] | daemon   (start the daemon; it keeps the database in step with the folder\n" \
    "                                   by itself, -M = manual: only scan / prune when you ask)\n" \
    "Commands: next prev rand save reload fnext fprev\n" \
    "          purity <1-3|SFW|Sketchy|NSFW|+|->   view <*|?|Category|@tag>\n" \
    "          set <path>            (draw any image; a library file also becomes the current one)\n" \
    "          info | rate <0-3|SFW|..> [Category|@tag] | name <text> | tag <t..> | untag <t..>\n" \
    "          ct <Category|-> [folder [SFW|..]] | unct <Category>   (set / remove a category)\n" \
    "          list [view] | cats | tiers   (print wallpapers / categories / scan folders to stdout, tab-separated)\n" \
    "          scan [Folder=N|Folder=- ...]   (add new files; Folder=N makes a folder a purity level and is\n" \
    "                                          remembered for later scans, Folder=- forgets it)\n" \
    "          prune                 (drop database rows whose file was deleted)\n"

char sock_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static int serv_fd = -1;
static int slideshow_interval = 0;
static char current_mode = 'n';
extern char current_folder[256];
extern int count, cur;

/* ── Helpers ──────────────────────────────────────────────────────────── */

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
    long val;
    if (!d || *d != ':') return 0;
    d++;
    if (!isdigit((unsigned char)*d)) return 0;
    val = 0;
    while (isdigit((unsigned char)*d)) {
	int digit = *d - '0';
      if (val > 65535 / 10) return 0;
      val = val * 10 + digit;
      if (val > 65535) return 0;
        d++;
    }
    if (*d == '\0') return 1;
    if (*d != '.') return 0;
    d++;
    if (!isdigit((unsigned char)*d)) return 0;
    val = 0;
    while (isdigit((unsigned char)*d)) {

       int digit = *d - '0';
       if (val > 255 / 10) return 0;
       val = val * 10 + digit;
       if (val > 255) return 0;
        d++;
    }
    return *d == '\0';
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

    if (!display) display = ":0";
    if (!valid_display(display)) {
        (void)write(STDERR_FILENO, "Error: Invalid DISPLAY value\n", 29);
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

/* ── Landlock sandbox (Linux only) ────────────────────────────────────── */

#ifdef __linux__
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <stdint.h>
#include <stddef.h>

static inline int landlock_create_ruleset(const struct landlock_ruleset_attr *attr,
                                          size_t size, uint32_t flags) {
    return (int)syscall(SYS_landlock_create_ruleset, attr, size, flags);
}
static inline int landlock_add_rule(int rfd, enum landlock_rule_type type,
                                    const void *attr, uint32_t flags) {
    return (int)syscall(SYS_landlock_add_rule, rfd, type, attr, flags);
}
static inline int landlock_restrict_self(int rfd, uint32_t flags) {
    return (int)syscall(SYS_landlock_restrict_self, rfd, flags);
}

/*
 * LL_ALLOW — open path with O_PATH, add a landlock rule, then close.
 * Logs a warning if the path cannot be opened or the rule cannot be added.
 * Silent failures here are acceptable: sandbox degrades gracefully rather
 * than preventing the daemon from starting.
 */
#define LL_ALLOW(path, access) do {                                          \
    int _fd = open((path), O_PATH | O_CLOEXEC);                             \
    if (_fd < 0) {                                                           \
        fprintf(stderr, "landlock: cannot open '%s': %s\n",                 \
                (path), strerror(errno));                                    \
    } else {                                                                 \
        struct landlock_path_beneath_attr _pb = {                           \
            .allowed_access = (access),                                     \
            .parent_fd      = _fd,                                          \
        };                                                                   \
        if (landlock_add_rule(rs_fd, LANDLOCK_RULE_PATH_BENEATH,            \
                              &_pb, 0) < 0)                                 \
            fprintf(stderr, "landlock: add_rule failed for '%s': %s\n",     \
                    (path), strerror(errno));                                \
        close(_fd);                                                          \
    }                                                                        \
} while (0)

static void landlock_apply(const char *master_dir,
                           const char *save_path) {
    int abi = landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0) {
        if (errno == ENOSYS)
            fprintf(stderr, "landlock: not supported by this kernel, skipping sandbox\n");
        else
            fprintf(stderr, "landlock: version probe failed: %s\n", strerror(errno));
        return;
    }

    struct landlock_ruleset_attr rs_attr = {
        .handled_access_fs =
            LANDLOCK_ACCESS_FS_READ_FILE   | LANDLOCK_ACCESS_FS_READ_DIR  |
            LANDLOCK_ACCESS_FS_WRITE_FILE  | LANDLOCK_ACCESS_FS_MAKE_REG  |
            LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_EXECUTE,
    };

    int rs_fd = landlock_create_ruleset(&rs_attr, sizeof(rs_attr), 0);
    if (rs_fd < 0) {
        fprintf(stderr, "landlock: create_ruleset failed: %s\n", strerror(errno));
        return;
    }

    /* Execution & dynamic linker */
    uint64_t exec_bits = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_EXECUTE;
    LL_ALLOW("/lib64/ld-linux-x86-64.so.2",exec_bits);
    LL_ALLOW("/usr/bin/sh",                exec_bits);
    LL_ALLOW("/etc/ld.so.cache",           LANDLOCK_ACCESS_FS_READ_FILE);

    /* System libraries */
    uint64_t lib_bits = LANDLOCK_ACCESS_FS_READ_FILE |
                        LANDLOCK_ACCESS_FS_READ_DIR  |
                        LANDLOCK_ACCESS_FS_EXECUTE;
    LL_ALLOW("/lib",     lib_bits);
    LL_ALLOW("/usr/lib", lib_bits);
    LL_ALLOW("/lib64",   lib_bits);
    LL_ALLOW("/usr/lib/x86_64-linux-gnu/imlib2", lib_bits);

    /* X11 & auth */
    LL_ALLOW("/tmp/.X11-unix",
             LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE);
    LL_ALLOW("/usr/share/X11", LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR);
    const char *xauth = getenv("XAUTHORITY");
    if (xauth) LL_ALLOW(xauth, LANDLOCK_ACCESS_FS_READ_FILE);

    /* Wallpaper directory */
    LL_ALLOW(master_dir,
             LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR);

    /* Socket directory */
    char s_buf[PATH_MAX];
    size_t slen = strlen(sock_path);
    if (slen < PATH_MAX) {
        memcpy(s_buf, sock_path, slen + 1);
        char *s_dir = dirname(s_buf);
        LL_ALLOW(s_dir,
                 LANDLOCK_ACCESS_FS_READ_FILE  | LANDLOCK_ACCESS_FS_WRITE_FILE |
                 LANDLOCK_ACCESS_FS_MAKE_REG   | LANDLOCK_ACCESS_FS_REMOVE_FILE);
    } else {
        fprintf(stderr, "landlock: sock_path too long, skipping socket dir rule\n");
    }

    /* Save-file directory */
    if (save_path) {
        char sv_buf[PATH_MAX];
        size_t svlen = strlen(save_path);
        if (svlen < PATH_MAX) {
            memcpy(sv_buf, save_path, svlen + 1);
            char *sv_dir = dirname(sv_buf);
            LL_ALLOW(sv_dir,
                     LANDLOCK_ACCESS_FS_READ_FILE  | LANDLOCK_ACCESS_FS_WRITE_FILE |
                     LANDLOCK_ACCESS_FS_READ_DIR   | LANDLOCK_ACCESS_FS_MAKE_REG   |
                     LANDLOCK_ACCESS_FS_REMOVE_FILE);
        } else {
            fprintf(stderr, "landlock: save_path too long, skipping save dir rule\n");
        }
    }

    /* Enforce */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        fprintf(stderr, "landlock: PR_SET_NO_NEW_PRIVS failed: %s\n", strerror(errno));

    if (landlock_restrict_self(rs_fd, 0) < 0)
        fprintf(stderr, "landlock: restrict_self failed: %s\n", strerror(errno));

    close(rs_fd);
}

#undef LL_ALLOW

/* ── Seccomp-BPF sandbox ──────────────────────────────────────────────── */

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

        /* Memory */
        SC_ALLOW(__NR_mmap),
        SC_ALLOW(__NR_munmap),
        SC_ALLOW(__NR_mprotect),
        SC_ALLOW(__NR_mremap),      /* Imlib2 */
        SC_ALLOW(__NR_brk),

        /* Sockets — X11 + Unix domain */
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
//	SC_ALLOW(__NR_prctl),
        SC_ALLOW(__NR_prlimit64),
	SC_ALLOW(__NR_newfstatat),

        //BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO | EACCES),
	BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS),
    };

    struct sock_fprog prog = {
        .len    = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };

    /* PR_SET_NO_NEW_PRIVS already set by landlock_apply */
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
//	case 'f':
//        if (path && path[0] != '\0') {
            // Bypass index logic: just set the file directly
//            run_setter(path);
//        }
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
    if (u > 0) printf("%d unrated: hidden outside view '?' until rated (wallman rate <1-3> <Category|@tag>)\n", u);
}

/* ── set <path>: show any image ─────────────────────────────────────────── */

#define STAGE_NAME ".wallman-show"

/* The daemon is sandboxed to the master dir, so a file from elsewhere is copied to
 * <master>/.wallman-show<.ext> (hidden, so scan ignores it) and drawn from there. The real
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
    static const char *const cmds[] = { "info", "scan", "prune", "unct", "rate", "ct", "name", "tag", "untag", "set", "list", "cats", "tiers" };
    const char *c = argv[0];
    size_t k;
    for (k = 0; k < sizeof(cmds) / sizeof(*cmds); k++)
        if (!strcmp(c, cmds[k])) break;
    if (k == sizeof(cmds) / sizeof(*cmds)) return -1;           /* not a database command */

    if (!strcmp(c, "list") || !strcmp(c, "cats") || !strcmp(c, "tiers")) {   /* read-only: works without the daemon */
        if (walldb_open(get_save_path()) < 0) return 1;
        int n = c[0] == 'l' ? walldb_print_walls(argc > 1 ? argv[1] : "*")
              : c[0] == 'c' ? walldb_print_cats() : walldb_print_tiers();
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
    if (!strcmp(c, "scan")) {
        int skipped = 0, n = walldb_scan(resp, (const char *const *)argv + 1, argc - 1, &skipped);
        if (n < 0) { fputs("Error: scan failed\n", stderr); return 1; }
        printf("scan: %d new", n);
        if (skipped) printf(", %d skipped (not jpg/png/jpeg/webp/ff)", skipped);
        printf("\n");
        unrated_hint(NULL);
    } else if (!strcmp(c, "set")) {                            /* set <path>: a library file becomes the current one, anything else is just drawn */
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
        else if (rel) fputs("note: not in the database yet (wallman scan adds it)\n", stderr);
        else if (stage_copy(rr, rp, staged, sizeof(staged)) == 0) send = staged;
        else { fprintf(stderr, "Error: could not copy %s into %s\n", rp, rr); free(rp); free(rr); return 1; }

        size_t sl = strlen(send);
        int fits = sl < sizeof(s.arg);
        if (fits) memcpy(s.arg, send, sl + 1);
        else fputs("Error: path too long\n", stderr);
        free(rp); free(rr);
        return (fits && ipc(addr, &s, NULL, 0) >= 0) ? 0 : 1;
    } else if (!strcmp(c, "prune")) {
        int n = walldb_prune(resp);
        if (n < 0) return 1;
        printf("pruned %d\n", n);
    } else if (!strcmp(c, "unct")) {                          /* unct <Category>: remove it from every wallpaper */
        int n = argc > 1 ? walldb_clear_category(argv[1]) : -1;
        if (n < 0) { fputs("Error: usage: unct <Category>\n", stderr); return 1; }
        printf("cleared %d\n", n);
    } else if (!strcmp(c, "ct") && argc > 2) {                /* ct <Category> <folder> [level]: a physical folder; level rates the unrated ones */
        int lvl = argc > 3 ? walldb_level(argv[3]) : 0, rated = 0;
        int n = (argc > 3 && lvl < 1) ? -1 : walldb_set_category_folder(argv[2], argv[1], lvl, &rated);
        if (n < 0) { fputs("Error: ct failed (usage: ct <Category|-> <folder> [SFW|Sketchy|NSFW])\n", stderr); return 1; }
        printf("categorized %d\n", n);
        if (rated) printf("rated %d unrated as %s\n", rated, argv[3]);
        unrated_hint(argv[1]);
    } else if (!strcmp(c, "rate") && argc > 2) {               /* rate <0-3> <Category|@tag>: bulk */
        int n = walldb_rate_view(argv[2], walldb_level(argv[1]));
        if (n < 0) { fputs("Error: usage: rate <0-3> <Category|@tag>\n", stderr); return 1; }
        printf("rated %d\n", n);
    } else if (!path[0]) {
        fputs("Error: no current wallpaper\n", stderr);
        return 1;
    } else if (!strcmp(c, "info")) {
        if (walldb_info(path) < 0) { fputs("Error: not in the database, run: wallman scan\n", stderr); return 1; }
        return 0;
    } else if (argc < 2) {
        fprintf(stderr, "Error: %s needs an argument\n", c);
        return 1;
    } else if (!strcmp(c, "rate")) {
        rc = walldb_rate(path, walldb_level(argv[1]));
    } else if (!strcmp(c, "ct")) {
        rc = walldb_set_category(path, argv[1]);
    } else if (!strcmp(c, "name")) {
        rc = walldb_set_name(path, argv[1]);
    } else {                                                    /* tag / untag, any number of tags */
        for (int i = 1; i < argc; i++)
            if (walldb_tag(path, argv[i], c[0] == 't') < 0) rc = -1;
    }
    if (rc < 0) { fputs("Error: update failed (not in the database, or bad value)\n", stderr); return 1; }

    WallMsg r = { .cmd = 'R', .interval = -1 };
    ipc(addr, &r, NULL, 0);
    return 0;
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    const char *display   = getenv("DISPLAY");
    int target_interval   = 0;
    int jump_target       = 0;
    int query_mode        = 0;
    int start_daemon      = 0;
    int manual            = 0;                   /* -M: no automatic scan / prune */
    int argi              = 1;

    while (argi < argc && argv[argi][0] == '-') {
        if      (!strcmp(argv[argi], "-S") && argi + 1 < argc) display = argv[++argi];
        else if (!strcmp(argv[argi], "-y") && argi + 1 < argc) target_interval = parse_duration(argv[++argi]);
        else if (!strcmp(argv[argi], "-j") && argi + 1 < argc) {
            char *end;
            long val = strtol(argv[++argi], &end, 10);
            jump_target = (*end != '\0' || val <= 0) ? -1 : (int)val;
        }
        else if (!strcmp(argv[argi], "-i")) query_mode   = 1;
        else if (!strcmp(argv[argi], "-D")) start_daemon = 1;
        else if (!strcmp(argv[argi], "-M")) manual       = 1;
        else if (!strcmp(argv[argi], "-h") || !strcmp(argv[argi], "--help")) {
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
            if      (!strcmp(arg, "next")   || !strcmp(arg, "-n")) msg.cmd = 'n';
            else if (!strcmp(arg, "prev")   || !strcmp(arg, "-p")) msg.cmd = 'p';
            else if (!strcmp(arg, "rand")   || !strcmp(arg, "-r")) msg.cmd = 'r';
            else if (!strcmp(arg, "fnext")  || !strcmp(arg, "-u")) msg.cmd = 'u';
            else if (!strcmp(arg, "fprev")  || !strcmp(arg, "-d")) msg.cmd = 'd';
            else if (!strcmp(arg, "save")   || !strcmp(arg, "-s")) msg.cmd = 's';
            else if (!strcmp(arg, "reload") || !strcmp(arg, "-R")) msg.cmd = 'R';
            else if (!strcmp(arg, "purity") || !strcmp(arg, "-P")) {
                msg.interval = -1;                              /* leave the slideshow timer alone */
                if      (!strcmp(val, "+")) msg.cmd = '+';
                else if (!strcmp(val, "-")) msg.cmd = '-';
                else { msg.cmd = 'P'; msg.target = walldb_level(val); }
            }
            else if (!strcmp(arg, "view")   || !strcmp(arg, "-v")) {
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

    if (!manual && master_dir[0]) {                  /* before the sandbox: the watcher is its own, unsandboxed process */
        watch_addr = addr;
        wallwatch_start(master_dir, get_save_path(), notify_daemon);
    }

#ifdef __linux__
    landlock_apply(master_dir, get_save_path());

    struct rlimit rl_zero = {0, 0};
    setrlimit(RLIMIT_NPROC, &rl_zero);

    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

    seccomp_apply();
#endif

    /* ── Event loop ────────────────────────────────────────────────────── */
    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(serv_fd, &fds);

        struct timeval  tv      = { .tv_sec = slideshow_interval, .tv_usec = 0 };
        struct timeval *timeout = (slideshow_interval > 0) ? &tv : NULL;

        int activity = select(serv_fd + 1, &fds, NULL, NULL, timeout);
        if (activity < 0) { if (errno == EINTR) continue; break; }
        if (activity == 0) { run_command(current_mode, 0, NULL); continue; }

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

        WallMsg m = {0};

        if (/*recv(client_fd, &m, sizeof(WallMsg), MSG_WAITALL)*/ recv(client_fd, &m, sizeof(WallMsg), 0) == sizeof(WallMsg)) {
            if (m.cmd == 'i') {
                char resp[512];
                char cur_str[12], tot_str[12];
                int  clen = fast_utoa(cur + 1, cur_str);
                int  tlen = fast_utoa(count,   tot_str);
                size_t flen = strlen(current_folder);
		size_t needed = 3 + clen + 1 + tlen;

                if (flen > sizeof(resp) - needed)
                    flen = sizeof(resp) - needed;

                char *ptr = resp;
                memcpy(ptr, current_folder, flen); ptr += flen;
                memcpy(ptr, " | ", 3);             ptr += 3;
                memcpy(ptr, cur_str, (size_t)clen);ptr += clen;
                *ptr++ = '/';
                memcpy(ptr, tot_str, (size_t)tlen);ptr += tlen;
                (void)write(client_fd, resp, (size_t)(ptr - resp));

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
            else if (m.cmd && strchr("nprsmudRjP+-VJS", m.cmd)) {
                if (m.interval >= 0)
                    slideshow_interval = (m.interval > 86400) ? 86400 : m.interval;
                if (strchr("npr", m.cmd))
                    current_mode = m.cmd;
                m.arg[sizeof(m.arg) - 1] = '\0';
                run_command(m.cmd, m.target, m.arg);
            }
        }
        close(client_fd);
    }

    unlink(sock_path);
    close(serv_fd);
    return 0;
}

