#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_WATCHDOGS 32

typedef struct {
    char *command[8];   /* argv, NULL terminated */
    int restart_delay;  /* seconds, -1 = run once */
} AppConfig;

#include "config.h"

typedef struct {
    char cmd;           /* 'r' = reload, 'e' = exit, 's' = status */
    int target_idx;     /* -1 = all, >= 0 = specific watchdog index */
} StarterMsg;

static pid_t watchdogs[MAX_WATCHDOGS];
static int w_count = 0;
static sigset_t sigs;
static volatile sig_atomic_t running = 1;
static char sock_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static int serv_fd = -1;
static mode_t old_umask;

static void onsig(int s) { (void)s; running = 0; }

/* ── IPC / Socket Helpers ────────────────────────────────────────────── */

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
    if (!d || *d != ':') return 0;
    d++;
    if (!isdigit((unsigned char)*d)) return 0;
    while (isdigit((unsigned char)*d)) d++;
    if (*d == '\0') return 1;
    if (*d != '.') return 0;
    d++;
    if (!isdigit((unsigned char)*d)) return 0;
    while (isdigit((unsigned char)*d)) d++;
    return *d == '\0';
}

static void get_sock_path(char *dest, size_t len, const char *display) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || runtime[0] != '/') {
        fprintf(stderr, "Cerberus: XDG_RUNTIME_DIR not set\n");
        exit(1);
    }

    if (!display) display = ":0";
    if (!valid_display(display)) {
        fprintf(stderr, "Cerberus: invalid DISPLAY value\n");
        exit(1);
    }

    const char *vtnr = getenv("XDG_VTNR");
    if (!vtnr) vtnr = "1";

    char uid_str[12];
    int ulen = fast_utoa((int)getuid(), uid_str);
    size_t rlen = strlen(runtime);
    size_t dlen = strlen(display);
    size_t vlen = strlen(vtnr);

    size_t total = rlen + 10 + (size_t)ulen + 1 + dlen + 4 + vlen + 5;
    if (total >= len) {
        fprintf(stderr, "Cerberus: socket path too long\n");
        exit(1);
    }

    char *ptr = dest;
    memcpy(ptr, runtime, rlen);     ptr += rlen;
    memcpy(ptr, "/cerberus-", 10); ptr += 10;
    memcpy(ptr, uid_str, ulen);     ptr += ulen;
    *ptr++ = '.';
    memcpy(ptr, display, dlen);     ptr += dlen;
    memcpy(ptr, ".tty", 4);         ptr += 4;
    memcpy(ptr, vtnr, vlen);        ptr += vlen;
    memcpy(ptr, ".sock", 6);
}

/* ── Process Watchdog & Supervisor Logic ─────────────────────────────── */

static void start_watchdog(const AppConfig *app, int slot_idx) {
    pid_t wpid = fork();
    if (wpid < 0) {
        perror("Cerberus: watchdog failed");
        return;
    }

    if (wpid == 0) {
        /*
         * Create a new process group for the watchdog and child app.
         * Ignore SIGINT and SIGTERM so Ctrl+C in terminal doesn't kill watchdog
         * before the parent supervisor can cleanly tear it down.
         */
        setpgid(0, 0);
        signal(SIGINT, SIG_IGN);
        signal(SIGTERM, SIG_IGN);

        sigset_t unblock_set;
        sigemptyset(&unblock_set);
        sigprocmask(SIG_SETMASK, &unblock_set, NULL);

        prctl(PR_SET_PDEATHSIG, SIGKILL);

        while (1) {
            pid_t app_pid = fork();

            if (app_pid == 0) {
                /* Reset default signals for actual application */
                signal(SIGINT, SIG_DFL);
                signal(SIGTERM, SIG_DFL);
                signal(SIGPIPE, SIG_DFL);
                umask(old_umask);
                prctl(PR_SET_PDEATHSIG, SIGTERM);   /* watchdog gone -> app goes too */
                execvp(app->command[0], app->command);
                _exit(1);

            } else if (app_pid > 0) {
                int status = 0;
                waitpid(app_pid, &status, 0);

                if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
                    fprintf(stderr, "Cerberus: %s exited with code %d\n",
                            app->command[0], WEXITSTATUS(status));
                else if (WIFSIGNALED(status))
                    fprintf(stderr, "Cerberus: %s killed by signal %d\n",
                            app->command[0], WTERMSIG(status));

                if (app->restart_delay < 0) {
                    _exit(0);
                } else {
                    struct timespec ts = {app->restart_delay, 0}, rem;
                    while (nanosleep(&ts, &rem) < 0) {
                        if (errno == EINTR) ts = rem;
                        else _exit(0);
                    }
                }

            } else {
                _exit(1);
            }
        }

    } else {
        watchdogs[slot_idx] = wpid;
    }
}

static void spawn_apps(void) {
    for (int i = 0; apps[i].command[0] != NULL; i++) {
        if (w_count >= MAX_WATCHDOGS) {
            fprintf(stderr, "Cerberus: reached MAX_WATCHDOGS (%d), skipping: %s\n",
                    MAX_WATCHDOGS, apps[i].command[0]);
            break;
        }
        start_watchdog(&apps[i], w_count);
        w_count++;
    }
}

static void stop_watchdog(int idx) {
    if (idx < 0 || idx >= w_count || watchdogs[idx] <= 0) return;

    /* Send SIGKILL directly to watchdog and its process group */
    kill(-watchdogs[idx], SIGKILL);
    kill(watchdogs[idx], SIGKILL);
    waitpid(watchdogs[idx], NULL, 0);
    watchdogs[idx] = 0;
}

/* Ask the app to exit (the watchdog ignores TERM, so only the app sees it). */
static void term_watchdog(int idx) {
    if (idx < 0 || idx >= w_count || watchdogs[idx] <= 0) return;
    kill(-watchdogs[idx], SIGTERM);
}

static void grace(void) {
    struct timespec ts = {0, 200000000L};
    nanosleep(&ts, NULL);
}

static void cleanup_apps(void) {
    for (int i = 0; i < w_count; i++)
        term_watchdog(i);
    grace();
    for (int i = 0; i < w_count; i++)
        stop_watchdog(i);
    w_count = 0;
}

static void handle_ipc_cmd(StarterMsg m, int client_fd) {
    if (m.cmd == 'e') {
        running = 0;
        const char *msg = "Exiting Cerberus...\n";
        (void)write(client_fd, msg, strlen(msg));
    } else if (m.cmd == 'r') {
        if (m.target_idx >= 0) {
            if (m.target_idx < w_count) {
                char resp[128];
                snprintf(resp, sizeof(resp), "Reloading app [%d]: %s...\n",
                         m.target_idx, apps[m.target_idx].command[0]);
                (void)write(client_fd, resp, strlen(resp));

                term_watchdog(m.target_idx);
                grace();
                stop_watchdog(m.target_idx);
                start_watchdog(&apps[m.target_idx], m.target_idx);
            } else {
                char resp[128];
                snprintf(resp, sizeof(resp), "Error: Invalid index %d (max: %d)\n",
                         m.target_idx, w_count - 1);
                (void)write(client_fd, resp, strlen(resp));
            }
        } else {
            const char *msg = "Reloading all apps...\n";
            (void)write(client_fd, msg, strlen(msg));
            cleanup_apps();
            spawn_apps();
        }
    } else if (m.cmd == 's') {
        char resp[1024];
        int pos = snprintf(resp, sizeof(resp), "Active Watchdogs: %d/%d\n", w_count, MAX_WATCHDOGS);
        for (int i = 0; i < w_count && pos < (int)sizeof(resp); i++) {
            pos += snprintf(resp + pos, sizeof(resp) - pos, "  [%d] PID: %d -> %s\n",
                            i, watchdogs[i], apps[i].command[0]);
        }
        (void)write(client_fd, resp, strlen(resp));
    }
}

/* ── Main Entry ───────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    const char *display = getenv("DISPLAY");

    /* CLI Client Mode */
    if (argc > 1) {
        get_sock_path(sock_path, sizeof(sock_path), display);
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        size_t slen = strlen(sock_path);
        memcpy(addr.sun_path, sock_path, slen + 1);

        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            fprintf(stderr, "Cerberus: could not connect to supervisor daemon\n");
            if (fd >= 0) close(fd);
            return 1;
        }

        StarterMsg msg = { .target_idx = -1 };

        if (!strcmp(argv[1], "reload") || !strcmp(argv[1], "-r")) {
            msg.cmd = 'r';
            if (argc > 2 && isdigit((unsigned char)argv[2][0])) {
                msg.target_idx = atoi(argv[2]);
            }
        } else if (!strcmp(argv[1], "exit") || !strcmp(argv[1], "-e")) {
            msg.cmd = 'e';
        } else if (!strcmp(argv[1], "status") || !strcmp(argv[1], "-s")) {
            msg.cmd = 's';
        } else {
            fprintf(stderr, "Usage: Cerberus [reload [idx] | exit | status]\n");
            close(fd);
            return 1;
        }

        if (write(fd, &msg, sizeof(msg)) == sizeof(msg)) {
            char buf[1024];
            int n = read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                fputs(buf, stdout);
            }
        }
        close(fd);
        return 0;
    }

    /* Daemon Mode Initialization */
    pid_t ppid = getppid();

    for (int s = 1; s < NSIG; s++)
        signal(s, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);           /* client hung up mid-reply must not kill us */

    struct sigaction sa = { .sa_handler = onsig };  /* no SA_RESTART: pselect must see EINTR */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    sigemptyset(&sigs);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGHUP);
    sigprocmask(SIG_BLOCK, &sigs, NULL);

    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != ppid)
        return 0;

    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

    /* Setup Socket Path */
    get_sock_path(sock_path, sizeof(sock_path), display);

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    size_t slen = strlen(sock_path);
    memcpy(addr.sun_path, sock_path, slen + 1);

    /* Single Instance Check */
    int probe_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe_fd >= 0) {
        if (connect(probe_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            fprintf(stderr, "Cerberus: already running on display %s\n", display ? display : ":0");
            close(probe_fd);
            return 1;
        }
        close(probe_fd);
    }

    unlink(sock_path);
    serv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (serv_fd < 0) return 1;
    fcntl(serv_fd, F_SETFD, FD_CLOEXEC);

    old_umask = umask(0077);
    if (bind(serv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(serv_fd, 5) < 0) {
        close(serv_fd);
        return 1;
    }

    spawn_apps();

    sigset_t empty_mask;
    sigemptyset(&empty_mask);

    /* Event Loop */
    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(serv_fd, &fds);

        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };

        int activity = pselect(serv_fd + 1, &fds, NULL, NULL, &ts, &empty_mask);

        if (activity < 0)
            continue;

        if (activity > 0 && FD_ISSET(serv_fd, &fds)) {
            int client_fd = accept4(serv_fd, NULL, NULL, SOCK_CLOEXEC);
            if (client_fd < 0) continue;

            struct timeval tv = { .tv_sec = 1 };
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            struct ucred cred;
            socklen_t ulen = sizeof(cred);
            if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &ulen) == 0 &&
                cred.uid == getuid()) {
                StarterMsg m = {0};
                if (read(client_fd, &m, sizeof(m)) == sizeof(m)) {
                    handle_ipc_cmd(m, client_fd);
                }
            }

            shutdown(client_fd, SHUT_WR);
            close(client_fd);
        }
    }

    cleanup_apps();
    close(serv_fd);
    unlink(sock_path);
    return 0;
}

