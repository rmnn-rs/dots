
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

#include "wall.h"

#define USAGE \
    "Usage: wallman [-S wayland-N] [-y dur] [-j idx] [-i] <cmd>\n" \
    "       wallman -D | daemon    (start background daemon)\n" \
    "Commands: next prev rand save reload fnext fprev\n"

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

/* ── IPC message ──────────────────────────────────────────────────────── */

typedef struct {
    char cmd;
    int  interval;
    int  target;
//    char path[PATH_MAX];
} WallMsg;

/* ── Signal handling ──────────────────────────────────────────────────── */

static volatile sig_atomic_t running = 1;
void handle_sig(int sig) { (void)sig; running = 0; }

/* ── Commands ---------─────────────────────────────────────────────────── */

void run_command(char cmd, int target) {
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

/* ── main ─────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    const char *display   = getenv("WAYLAND_DISPLAY");
    int target_interval   = 0;
    int jump_target       = 0;
    int query_mode        = 0;
    int start_daemon      = 0;
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
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            (void)write(STDERR_FILENO,
                        "Error: could not connect to daemon. Is it running?\n", 51);
            if (fd >= 0) close(fd);
            return 1;
        }

        WallMsg msg = { .interval = target_interval, .target = jump_target };
        if (query_mode)      msg.cmd = 'i';
        else if (jump_target > 0) msg.cmd = 'j';
        else {
            const char *arg = argv[argi];
            if      (!strcmp(arg, "next")   || !strcmp(arg, "-n")) msg.cmd = 'n';
            else if (!strcmp(arg, "prev")   || !strcmp(arg, "-p")) msg.cmd = 'p';
            else if (!strcmp(arg, "rand")   || !strcmp(arg, "-r")) msg.cmd = 'r';
            else if (!strcmp(arg, "fnext")  || !strcmp(arg, "-u")) msg.cmd = 'u';
            else if (!strcmp(arg, "fprev")  || !strcmp(arg, "-d")) msg.cmd = 'd';
            else if (!strcmp(arg, "save")   || !strcmp(arg, "-s")) msg.cmd = 's';
            else if (!strcmp(arg, "reload") || !strcmp(arg, "-R")) msg.cmd = 'R';
//            else if (!strcmp(arg, "fset")   || !strcmp(arg, "-f")) msg.cmd = 'f';
            else msg.cmd = arg[0];
        }

        if (write(fd, &msg, sizeof(WallMsg)) != sizeof(WallMsg)) { close(fd); return 1; }

        if (query_mode) {
            char resp[512];
            int n = (int)read(fd, resp, sizeof(resp) - 1);
            if (n > 0) {
                resp[n] = '\0';
                (void)write(STDOUT_FILENO, resp, (size_t)n);
                (void)write(STDOUT_FILENO, "\n", 1);
            }
        }
        close(fd);
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
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa,      NULL);
    sigaction(SIGTERM, &sa,      NULL);
    sigaction(SIGCHLD, &sa_chld, NULL);

    umask(0077);
    if (bind(serv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(serv_fd, 5) < 0) {
        close(serv_fd);
        return 1;
    }

    wall_setup_renderer();
    wall_restore();

    /* ── Event loop ────────────────────────────────────────────────────── */
    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(serv_fd, &fds);
        int wfd = wall_wl_fd();
        FD_SET(wfd, &fds);
        int maxfd = serv_fd > wfd ? serv_fd : wfd;

        struct timeval  tv      = { .tv_sec = slideshow_interval, .tv_usec = 0 };
        struct timeval *timeout = (slideshow_interval > 0) ? &tv : NULL;

        int activity = select(maxfd + 1, &fds, NULL, NULL, timeout);
        if (activity < 0) { if (errno == EINTR) continue; break; }
        if (activity == 0) { run_command(current_mode, 0); continue; }

        if (FD_ISSET(wfd, &fds) && wall_wl_dispatch() < 0) break; /* compositor gone */
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
	    else if (strchr("nprsmudRj", m.cmd)) {
                if (m.interval >= 0)
                    slideshow_interval = (m.interval > 86400) ? 86400 : m.interval;
                if (strchr("npr", m.cmd))
                    current_mode = m.cmd;
                run_command(m.cmd, m.target);
            }
        }
        close(client_fd);
    }

    unlink(sock_path);
    close(serv_fd);
    return 0;
}

