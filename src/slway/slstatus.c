/* See LICENSE file for copyright and license details. */
#define _GNU_SOURCE /* ppoll; drop this if you pass -D_GNU_SOURCE in config.mk */
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <X11/Xlib.h>

#include "arg.h"
#include "slstatus.h"
#include "util.h"

struct arg {
    const char *(*func)(const char *);
    const char *fmt;
    const char *args;
    const int interval;    /* Interval in seconds, 0 = only on SIGUSR1 */
    char cache[256];        /* Stores the last successful result */
    time_t next_update;    /* Timestamp for the next permitted run */
};

/* a pollable event source (netlink socket, timerfd, MPD idle connection, ...) */
struct evsrc {
    int (*init)(void);      /* returns a pollable fd, or -1 */
    int (*drain)(int fd);   /* eat what is pending: >0 refresh funcs, 0 ignore, <0 source died
                             * (funcs refresh too; drain has already closed the fd) */
    const char *(*funcs[4])(const char *);  /* components to refresh on an event */
    int fd;
};

#define NEVER ((time_t)LONG_MAX)    /* next_update of an interval 0 component */
#define EVRETRY 5                   /* seconds between re-init attempts of a dead source */

char buf[1024];
static volatile sig_atomic_t done, refresh;
static Display *dpy;

#include "config.h"

static void
terminate(const int signo)
{
    if (signo == SIGUSR1)
        refresh = 1;
    else
        done = 1;
}

/* make every component an event source feeds due right now */
static void
fire(const struct evsrc *e)
{
    size_t i, j;

    for (i = 0; i < LEN(args); i++)
        for (j = 0; j < LEN(e->funcs) && e->funcs[j]; j++)
            if (args[i].func == e->funcs[j])
                args[i].next_update = 0;
}

static void
difftimespec(struct timespec *res, struct timespec *a, struct timespec *b)
{
    res->tv_sec = a->tv_sec - b->tv_sec - (a->tv_nsec < b->tv_nsec);
    res->tv_nsec = a->tv_nsec - b->tv_nsec +
                   (a->tv_nsec < b->tv_nsec) * 1E9;
}

static void
usage(void)
{
    die("usage: %s [-v] [-s] [-1]", argv0);
}

int
main(int argc, char *argv[])
{
    struct sigaction act;
    struct timespec current, due, wait, *timeout;
    struct pollfd pfd[LEN(evsrcs)];
    sigset_t blk, orig;
    time_t soon;
    size_t i, len;
    int sflag, ret, force, first, changed, n;
    char status[MAXLEN], last[MAXLEN];
    const char *res;

    sflag = 0;
    ARGBEGIN {
    case 'v':
        die("slstatus-"VERSION);
	__attribute__((fallthrough));
    case '1':
        done = 1;
        /* FALLTHROUGH */
    case 's':
        sflag = 1;
        break;
    default:
        usage();
    } ARGEND

    if (argc)
        usage();

    memset(&act, 0, sizeof(act));
    act.sa_handler = terminate;
    sigaction(SIGINT,  &act, NULL);
    sigaction(SIGTERM, &act, NULL);
    act.sa_flags |= SA_RESTART;
    sigaction(SIGUSR1, &act, NULL);

    /* signals that wake the sleep; blocked only around the flag check + ppoll */
    sigemptyset(&blk);
    sigaddset(&blk, SIGINT);
    sigaddset(&blk, SIGTERM);
    sigaddset(&blk, SIGUSR1);

    if (!sflag && !(dpy = XOpenDisplay(NULL)))
        die("XOpenDisplay: Failed to open display");

    /* Initialize next_update for all args to 0 so they run immediately on start */
    for (i = 0; i < LEN(args); i++) {
        args[i].next_update = 0;
        memset(args[i].cache, 0, sizeof(args[i].cache));
    }
    for (i = 0; i < LEN(evsrcs); i++)
        evsrcs[i].fd = -1;

    force = 0;
    first = 1;
    do {
        /* monotonic: immune to wall-clock jumps, same clock the ppoll timeout
         * uses. don't use time() here, it's a coarse clock that can lag by a
         * few ms and would make us wake early and spin */
        if (clock_gettime(CLOCK_MONOTONIC, &current) < 0)
            die("clock_gettime:");
        time_t now = current.tv_sec;
        status[0] = '\0';
        len = 0;

        /* (re)open dead sources; whatever they feed may have changed meanwhile */
        for (i = 0; i < LEN(evsrcs) && !done; i++)
            if (evsrcs[i].fd < 0 && (evsrcs[i].fd = evsrcs[i].init()) >= 0)
                fire(&evsrcs[i]);

        for (i = 0; i < LEN(args); i++) {
            /* Only call the function if the interval has passed (or SIGUSR1 forced a refresh) */
            if (force || now >= args[i].next_update) {
                res = args[i].func(args[i].args);
                if (!res) {
                    res = unknown_str;
                }

//                strncpy(args[i].cache, res, sizeof(args[i].cache) - 1);
	    if (res) {
                snprintf(args[i].cache, sizeof(args[i].cache), "%s", res);
            }
                args[i].cache[sizeof(args[i].cache) - 1] = '\0';
                args[i].next_update = args[i].interval ?
                                      now + args[i].interval : NEVER;
            }

            /* Build the status string using the cached value */
            if ((ret = esnprintf(status + len, sizeof(status) - len,
                                 args[i].fmt, args[i].cache)) < 0)
                break;

            len += ret;
        }

        /* dwm re-parses and repaints the bar on every root name change,
         * so don't write a string that is identical to the last one */
        changed = first || strcmp(status, last);
        if (changed) {
            memcpy(last, status, strlen(status) + 1);
            first = 0;
        }

        if (changed && sflag) {
            puts(status);
            fflush(stdout);
            if (ferror(stdout))
                die("puts:");
        } else if (changed) {
            if (XStoreName(dpy, DefaultRootWindow(dpy), status) < 0)
                die("XStoreName: Allocation failed");
            XFlush(dpy);
        }

        /* Sleep until the nearest component is due, or a signal arrives.
         * Signals stay blocked while the flags are checked and ppoll unblocks
         * them atomically, so a SIGUSR1 can't slip in between check and sleep.
         * The mask is restored afterwards so popen()'d children don't inherit it. */
        sigprocmask(SIG_BLOCK, &blk, &orig);
        if (!done && !refresh) {
            soon = args[0].next_update;
            for (i = 1; i < LEN(args); i++)
                if (args[i].next_update < soon)
                    soon = args[i].next_update;
            for (i = 0; i < LEN(evsrcs); i++) {
                pfd[i].fd = evsrcs[i].fd;   /* fd < 0 is ignored by ppoll */
                pfd[i].events = POLLIN;
                pfd[i].revents = 0;
                /* a dead source gets retried, don't sleep forever on it */
                if (evsrcs[i].fd < 0 && now + EVRETRY < soon)
                    soon = now + EVRETRY;
            }

            timeout = NULL;     /* all event-only: sleep until a signal or an fd */
            if (soon != NEVER) {
                if (clock_gettime(CLOCK_MONOTONIC, &current) < 0)
                    die("clock_gettime:");
                due.tv_sec = soon;
                due.tv_nsec = 0;
                difftimespec(&wait, &due, &current);
                timeout = &wait;
            }

            n = 0;
            if ((!timeout || timeout->tv_sec >= 0) &&
                (n = ppoll(pfd, LEN(pfd), timeout, &orig)) < 0) {
                if (errno != EINTR)
                    die("ppoll:");
                n = 0;
            }

            for (i = 0; n > 0 && i < LEN(evsrcs); i++) {
                if (!pfd[i].revents)
                    continue;
                /* also on POLLHUP/POLLERR: the read fails and the source reports itself dead */
                ret = evsrcs[i].drain(evsrcs[i].fd);
                if (ret)    /* an event, or the source died: either way what it feeds may be stale */
                    fire(&evsrcs[i]);
                if (ret < 0)
                    evsrcs[i].fd = -1;
            }
        }
        force = refresh;
        refresh = 0;
        sigprocmask(SIG_SETMASK, &orig, NULL);
    } while (!done);

    if (!sflag) {
        XStoreName(dpy, DefaultRootWindow(dpy), NULL);
        if (XCloseDisplay(dpy) < 0)
            die("XCloseDisplay: Failed to close display");
    }

    return 0;
}

