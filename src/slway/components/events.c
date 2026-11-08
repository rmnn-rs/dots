/* See LICENSE file for copyright and license details. */
#define _GNU_SOURCE /* memmem */
#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <mpd/client.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "../slstatus.h"
#include "../util.h"

/*
 * Event sources for the ppoll loop in slstatus.c (see struct evsrc there).
 *
 * ev_*_init():  returns a pollable fd, or -1 (the main loop retries).
 * ev_*_drain(): eats everything pending on the fd. Returns >0 if something
 *               relevant happened, 0 if not, <0 if the source died, in which
 *               case it has already closed the fd itself.
 */

/* ---- netlink ---------------------------------------------------------- */

static int
nl_init(int proto, unsigned int groups)
{
	struct sockaddr_nl sa;
	int fd;

	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	sa.nl_groups = groups;

	if ((fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC,
	                 proto)) < 0) {
		warn("socket:");
		return -1;
	}
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		warn("bind:");
		close(fd);
		return -1;
	}

	return fd;
}

/* read every queued datagram, return 1 if match() liked at least one */
static int
nl_drain(int fd, int (*match)(const char *, size_t))
{
	char msg[8192];
	ssize_t n;
	int hit = 0;

	for (;;) {
		if ((n = recv(fd, msg, sizeof(msg) - 1, 0)) < 0) {
			if (errno == EINTR)
				continue;
			if (errno == ENOBUFS) { /* queue overran, we missed some */
				hit = 1;
				continue;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return hit;
			warn("recv:");
			close(fd);
			return -1;
		}
		msg[n] = '\0';
		if (match(msg, (size_t)n))
			hit = 1;
	}
}

static int
is_power_supply(const char *msg, size_t len)
{
	/* uevent = "action@devpath\0KEY=val\0KEY=val\0..." */
	static const char key[] = "\0SUBSYSTEM=power_supply\0";

	return memmem(msg, len + 1, key, sizeof(key) - 1) != NULL;
}

static int
any(const char *msg, size_t len)
{
	(void)msg;
	(void)len;
	return 1;
}

/* kernel uevents (group 1), only power_supply ones count: AC/battery state */
int
ev_uevent_init(void)
{
	return nl_init(NETLINK_KOBJECT_UEVENT, 1);
}

int
ev_uevent_drain(int fd)
{
	return nl_drain(fd, is_power_supply);
}

/* rtnetlink link changes: carrier up/down, (dis)association, interface add/del */
int
ev_link_init(void)
{
	return nl_init(NETLINK_ROUTE, RTMGRP_LINK);
}

int
ev_link_drain(int fd)
{
	return nl_drain(fd, any);
}

/* ---- wall-clock minute tick ------------------------------------------- */

/* time() reads a coarse clock that trails the real one by up to a tick, so a
 * timer firing exactly on the boundary still sees the old minute. 25 ms covers
 * HZ >= 100 and is invisible on a bar */
#define MINUTE_SLACK_NS 25000000L

static int
minute_arm(int fd)
{
	struct itimerspec its;
	struct timespec now;

	memset(&its, 0, sizeof(its));
	if (clock_gettime(CLOCK_REALTIME, &now) < 0)
		return -1;
	/* next full minute, absolute: also fires after suspend, and
	 * CANCEL_ON_SET tells us when the clock was stepped under us */
	its.it_value.tv_sec = now.tv_sec - now.tv_sec % 60 + 60;
	its.it_value.tv_nsec = MINUTE_SLACK_NS;

	return timerfd_settime(fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET,
	                       &its, NULL);
}

int
ev_minute_init(void)
{
	int fd;

	if ((fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC)) < 0) {
		warn("timerfd_create:");
		return -1;
	}
	if (minute_arm(fd) < 0) {
		warn("timerfd_settime:");
		close(fd);
		return -1;
	}

	return fd;
}

int
ev_minute_drain(int fd)
{
	uint64_t expirations;

	/* ECANCELED: clock was set (resume, ntp step), redraw anyway */
	if (read(fd, &expirations, sizeof(expirations)) < 0 &&
	    errno != EAGAIN && errno != ECANCELED) {
		warn("read:");
		close(fd);
		return -1;
	}
	if (minute_arm(fd) < 0) {
		warn("timerfd_settime:");
		close(fd);
		return -1;
	}

	return 1;
}

/* ---- MPD idle --------------------------------------------------------- */

#define EVMPD_MASK (MPD_IDLE_PLAYER | MPD_IDLE_QUEUE)

static struct mpd_connection *evmpd;

static int
evmpd_fail(void)
{
	if (evmpd)
		mpd_connection_free(evmpd); /* closes the fd */
	evmpd = NULL;

	return -1;
}

/* silent on failure: MPD not running is normal, the main loop retries */
int
ev_mpd_init(void)
{
	if (!(evmpd = mpd_connection_new(NULL, 0, 500)) ||
	    mpd_connection_get_error(evmpd) != MPD_ERROR_SUCCESS ||
	    !mpd_send_idle_mask(evmpd, EVMPD_MASK))
		return evmpd_fail();

	return mpd_connection_get_fd(evmpd);
}

int
ev_mpd_drain(int fd)
{
	(void)fd;

	/* what changed doesn't matter, music_status re-reads everything */
	mpd_recv_idle(evmpd, false);
	if (mpd_connection_get_error(evmpd) != MPD_ERROR_SUCCESS ||
	    !mpd_send_idle_mask(evmpd, EVMPD_MASK))
		return evmpd_fail();

	return 1;
}

