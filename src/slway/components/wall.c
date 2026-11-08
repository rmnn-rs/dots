#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "../slstatus.h"
#include "../util.h"

/*
 * Wall component fed by wallman: ev_wall_init() subscribes (cmd 'w') and wallman pushes
 * "view | n/total\n" every time it changes. get_wall_info() only formats the last line, so
 * it belongs with interval 0 and the evsrcs entry in config.h.
 */

typedef struct {
    char cmd;
    int  interval;
    int  target;
    char arg[256];
} WallMsg;

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

static char wall_line[224];     /* last line wallman pushed, "" = unknown; sized so the formatted
                                 * result fits the 256 byte cache slstatus keeps per component */
static char rx[512];            /* bytes received, not yet a full line */
static size_t rxlen;

static char *
put(char *p, const char *s)
{
    size_t n = strlen(s);

    memcpy(p, s, n);
    return p + n;
}

/* <XDG_RUNTIME_DIR>/wl-<uid>.<display>.sock, the way wallman builds it */
static int
wall_sock_path(char *dest, size_t len)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *display = getenv("WAYLAND_DISPLAY");
    const char *s;
    char uid_str[12], fallback[32];
    char *p;

    fast_utoa((int)getuid(), uid_str);
    if (!display || strncmp(display, "wayland-", 8) || !display[8])
        display = "wayland-0";

    for (s = display + 8; *s; s++) {
        if (*s < '0' || *s > '9') {
            display = "wayland-0";
            break;
        }
    }

    if (!runtime || runtime[0] != '/') {
        p = put(fallback, "/run/user/");
        p = put(p, uid_str);
        *p = '\0';
        runtime = fallback;
    }
    if (strlen(runtime) + 4 + strlen(uid_str) + 1 + strlen(display) + 6 > len)
        return -1;

    p = put(dest, runtime);
    p = put(p, "/wl-");
    p = put(p, uid_str);
    p = put(p, ".");
    p = put(p, display);
    memcpy(p, ".sock", 6);      /* includes '\0' */
    return 0;
}

static int
wall_connect(void)
{
    struct sockaddr_un addr;
    char path[sizeof(addr.sun_path)];
    int fd;

    if (wall_sock_path(path, sizeof(path)) < 0)
        return -1;
    if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* read what wallman pushed, keep the last full line. >0 new line, 0 nothing, <0 wallman went away */
int
ev_wall_drain(int fd)
{
    char *nl;
    ssize_t n;
    size_t l;
    int hit = 0;

    for (;;) {
        n = recv(fd, rx + rxlen, sizeof(rx) - rxlen, MSG_DONTWAIT);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return hit;
        if (n <= 0) {
            close(fd);
            wall_line[0] = '\0';
            rxlen = 0;
            return -1;
        }
        rxlen += (size_t)n;

        while ((nl = memchr(rx, '\n', rxlen))) {
            l = (size_t)(nl - rx);
            if (l >= sizeof(wall_line))
                l = sizeof(wall_line) - 1;
            memcpy(wall_line, rx, l);
            wall_line[l] = '\0';
            rxlen -= (size_t)(nl - rx) + 1;
            memmove(rx, nl + 1, rxlen);
            hit = 1;
        }
        if (rxlen == sizeof(rx))    /* no newline in a whole buffer: garbage, resync */
            rxlen = 0;
    }
}

int
ev_wall_init(void)
{
    WallMsg msg = { .cmd = 'w', .interval = -1 };
    struct pollfd p;
    int fd;

    if ((fd = wall_connect()) < 0)
        return -1;
    if (send(fd, &msg, sizeof(msg), MSG_NOSIGNAL) != (ssize_t)sizeof(msg)) {
        close(fd);
        return -1;
    }

    /* wallman answers with the current state right away: wait for it so the first render isn't blank */
    rxlen = 0;
    p.fd = fd;
    p.events = POLLIN;
    if (poll(&p, 1, 200) > 0 && ev_wall_drain(fd) < 0)
        return -1;
    return fd;
}

const char *
get_wall_info(const char *unused)
{
    static char formatted[512];
    char raw[sizeof(wall_line)];
    char *index;

    if (!wall_line[0])
        return "^c#4C566A^---^d^";

    memcpy(raw, wall_line, sizeof(raw));
    index = strstr(raw, " | ");

    if (index) {
        *index = '\0';
        index += 3;
        snprintf(formatted, sizeof(formatted), "^c#88C0D0^🖼 %s ^c#CCCCCC^[%s]^d^", raw, index);
    } else {
        snprintf(formatted, sizeof(formatted), "^c#88C0D0^🖼 ^c#FFFFFF^%s^d^", raw);
    }

    return formatted;
}

