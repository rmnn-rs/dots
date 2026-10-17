/* See LICENSE file for copyright and license details. */
#define _GNU_SOURCE /* ppoll; drop this if you pass -D_GNU_SOURCE in config.mk */
#include <errno.h>
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
    const int interval;    /* Interval in seconds */
    char cache[256];        /* Stores the last successful result */
    time_t next_update;    /* Timestamp for the next permitted run */
};

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
    struct timespec current, due, wait;
    sigset_t blk, orig;
    time_t soon;
    size_t i, len;
    int sflag, ret, force;
    char status[MAXLEN];
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

    force = 0;
    do {
        /* monotonic: immune to wall-clock jumps, same clock the ppoll timeout
         * uses. don't use time() here, it's a coarse clock that can lag by a
         * few ms and would make us wake early and spin */
        if (clock_gettime(CLOCK_MONOTONIC, &current) < 0)
            die("clock_gettime:");
        time_t now = current.tv_sec;
        status[0] = '\0';
        len = 0;

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
                args[i].next_update = now + args[i].interval;
            }

            /* Build the status string using the cached value */
            if ((ret = esnprintf(status + len, sizeof(status) - len,
                                 args[i].fmt, args[i].cache)) < 0)
                break;

            len += ret;
        }

        if (sflag) {
            puts(status);
            fflush(stdout);
            if (ferror(stdout))
                die("puts:");
        } else {
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
            if (soon <= now)    /* interval 0 would spin otherwise */
                soon = now + 1;

            if (clock_gettime(CLOCK_MONOTONIC, &current) < 0)
                die("clock_gettime:");
            due.tv_sec = soon;
            due.tv_nsec = 0;
            difftimespec(&wait, &due, &current);

            if (wait.tv_sec >= 0 &&
                ppoll(NULL, 0, &wait, &orig) < 0 &&
                errno != EINTR)
                    die("ppoll:");
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

