/* See LICENSE file for copyright and license details. */
#include <stdio.h>
#include <string.h>

#include "../slstatus.h"
#include "../util.h"

#if defined(__linux__)
/*
 * https://www.kernel.org/doc/html/latest/power/power_supply_class.html
 */
	#include <limits.h>
	#include <stdint.h>
	#include <stdarg.h>
	#include <unistd.h>

	#define POWER_SUPPLY_CAPACITY "/sys/class/power_supply/%s/capacity"
	#define POWER_SUPPLY_STATUS   "/sys/class/power_supply/%s/status"
	#define POWER_SUPPLY_CHARGE   "/sys/class/power_supply/%s/charge_now"
	#define POWER_SUPPLY_ENERGY   "/sys/class/power_supply/%s/energy_now"
	#define POWER_SUPPLY_CURRENT  "/sys/class/power_supply/%s/current_now"
	#define POWER_SUPPLY_POWER    "/sys/class/power_supply/%s/power_now"
	#define POWER_SUPPLY_VOLTAGE  "/sys/class/power_supply/%s/voltage_now"

	static const char *
	pick(const char *bat, const char *f1, const char *f2, char *path,
	     size_t length)
	{
		if (esnprintf(path, length, f1, bat) > 0 &&
		    access(path, R_OK) == 0)
			return f1;

		if (esnprintf(path, length, f2, bat) > 0 &&
		    access(path, R_OK) == 0)
			return f2;

		return NULL;
	}

	const char *
	battery_perc(const char *bat)
	{
		int cap_perc;
		char path[PATH_MAX];

		if (esnprintf(path, sizeof(path), POWER_SUPPLY_CAPACITY, bat) < 0)
			return NULL;
		if (pscanf(path, "%d", &cap_perc) != 1)
			return NULL;

		return bprintf("%d", cap_perc);
	}

	const char *
	battery_state(const char *bat)
	{
		static struct {
			char *state;
			char *symbol;
		} map[] = {
//			{ "Charging",    "🔌" },
//			{ "Discharging", "🔋" },
//			{ "Full",        "⚡" },
//			{ "Not charging", " " },
			//{ "Not charging", "🛑" },

	    { "Charging",     "^c#A3BE8C^^d^" },
            { "Discharging",  "^c#EBCB8B^^d^" },
            { "Full",         "^c#88C0D0^^d^" },
            { "Not charging", "^c#BF616A^ ^d^" },

		};
		size_t i;
		char path[PATH_MAX], state[13];

		if (esnprintf(path, sizeof(path), POWER_SUPPLY_STATUS, bat) < 0)
			return NULL;
		if (pscanf(path, "%12[a-zA-Z ]", state) != 1)
			return NULL;

		for (i = 0; i < LEN(map); i++)
			if (!strcmp(map[i].state, state))
				break;

//		return (i == LEN(map)) ? "?" : map[i].symbol;
		return (i == LEN(map)) ? "^c#BF616A^?^d^" : map[i].symbol;
	}

	const char *
	battery_remaining(const char *bat)
	{
		uintmax_t charge_now, current_now, m, h;
		double timeleft;
		char path[PATH_MAX], state[13];

		if (esnprintf(path, sizeof(path), POWER_SUPPLY_STATUS, bat) < 0)
			return NULL;
		if (pscanf(path, "%12[a-zA-Z ]", state) != 1)
			return NULL;

		if (!pick(bat, POWER_SUPPLY_CHARGE, POWER_SUPPLY_ENERGY, path,
		          sizeof(path)) ||
		    pscanf(path, "%ju", &charge_now) < 0)
			return NULL;

		if (!strcmp(state, "Discharging")) {
			if (!pick(bat, POWER_SUPPLY_CURRENT, POWER_SUPPLY_POWER, path,
			          sizeof(path)) ||
			    pscanf(path, "%ju", &current_now) < 0)
				return NULL;

			if (current_now == 0)
				return NULL;

			timeleft = (double)charge_now / (double)current_now;
			h = timeleft;
			m = (timeleft - (double)h) * 60;

			return bprintf("%juh %jum", h, m);
		}

		return "";
	}

	/*
	 * battery_status(arg): arg is "NAME:letters", e.g. "BAT0:sbp". The letters
	 * work like the "nit" of music_status: they are drawn left to right in the
	 * order you write them, one space apart. Leave out the name to get BAT0,
	 * leave out the ":letters" to get BAT_DEFAULT_FMT.
	 *
	 *   s   state symbol, exactly what battery_state() returns
	 *   p   percent
	 *   b   bar, coloured like the state symbol; digits right after it set
	 *       the width (b12 = 12 cells)
	 *   r   time left, only while discharging
	 *   w   power draw in watts, when the battery reports it
	 *
	 * The bar takes its colour from the map in battery_state(), so change the
	 * colours there and both follow. Unknown characters are skipped.
	 *   "BAT0:sbp"  <symbol> ██████░░░░ 63%      "BAT0:sp"  <symbol> 63%
	 */
	#define BAT_DEFAULT_FMT "sp"

	/* bar: default width, limits for the width suffix, and the two cell glyphs */
	#define BAT_BAR_WIDTH   10
	#define BAT_BAR_MIN     2
	#define BAT_BAR_MAX     40
	#define BAT_BAR_FULL    "█"
	#define BAT_BAR_EMPTY   "░"

	#define BAT_COL_TEXT    "^c#FFFFFF^" /* percent, time, watts */
	#define BAT_COL_EMPTY   "^c#4C566A^" /* empty cells */
	#define BAT_COL_UNKNOWN "^c#BF616A^" /* bar when the state can't be read */
	#define BAT_COL_END     "^d^"

	struct bat_ob {
		char s[768];
		size_t n;
	};

	static void
	bat_add(struct bat_ob *o, const char *fmt, ...)
	{
		va_list ap;
		size_t room;
		int r;

		if (o->n >= sizeof(o->s) - 1)
			return;
		room = sizeof(o->s) - o->n;
		va_start(ap, fmt);
		r = vsnprintf(o->s + o->n, room, fmt, ap);
		va_end(ap);
		if (r > 0)
			o->n += (size_t)r >= room ? room - 1 : (size_t)r;
	}

	static void
	bat_sep(struct bat_ob *o)
	{
		if (o->n)
			bat_add(o, " ");
	}

	/* one numeric sysfs file; quiet if it simply isn't there */
	static int
	bat_read(const char *tmpl, const char *bat, uintmax_t *v)
	{
		char path[PATH_MAX];

		if (esnprintf(path, sizeof(path), tmpl, bat) < 0 ||
		    access(path, R_OK) != 0)
			return -1;

		return pscanf(path, "%ju", v) == 1 ? 0 : -1;
	}

	/* power_now (uW), or current_now (uA) * voltage_now (uV) */
	static int
	bat_watts(const char *bat, double *w)
	{
		uintmax_t p, c, v;

		if (!bat_read(POWER_SUPPLY_POWER, bat, &p)) {
			*w = (double)p / 1e6;
			return 0;
		}
		if (!bat_read(POWER_SUPPLY_CURRENT, bat, &c) &&
		    !bat_read(POWER_SUPPLY_VOLTAGE, bat, &v)) {
			*w = (double)c * (double)v / 1e12;
			return 0;
		}

		return -1;
	}

	/* "^c#RRGGBB^<symbol>^d^" -> "^c#RRGGBB^": whatever colour the state map
	 * uses for this state is the colour of the bar */
	static void
	bat_colour(const char *sym, char *out, size_t len)
	{
		if (sym && !strncmp(sym, "^c#", 3) && strlen(sym) >= 10 &&
		    sym[9] == '^' && len > 10) {
			memcpy(out, sym, 10);
			out[10] = '\0';
		} else {
			snprintf(out, len, "%s", BAT_COL_UNKNOWN);
		}
	}

	static void
	bat_bar(struct bat_ob *o, const char *col, int perc, int width)
	{
		int fill, i;

		if (perc < 0)
			perc = 0;
		if (perc > 100)
			perc = 100;
		fill = (perc * width + 50) / 100;

		bat_sep(o);
		if (fill > 0)
			bat_add(o, "%s", col);
		for (i = 0; i < fill; i++)
			bat_add(o, BAT_BAR_FULL);
		if (fill < width)
			bat_add(o, "%s", BAT_COL_EMPTY);
		for (; i < width; i++)
			bat_add(o, BAT_BAR_EMPTY);
		bat_add(o, BAT_COL_END);
	}

	const char *
	battery_status(const char *arg)
	{
		static struct bat_ob o;
		char bat[32], col[16];
		const char *fmt = BAT_DEFAULT_FMT, *colon, *sym, *p;
		uintmax_t v;
		double watts;
		size_t n;
		int perc = -1;

		colon = arg ? strchr(arg, ':') : NULL;
		n = !arg ? 0 : colon ? (size_t)(colon - arg) : strlen(arg);
		if (n >= sizeof(bat))
			return NULL;
		if (n) {
			memcpy(bat, arg, n);
			bat[n] = '\0';
		} else {
			snprintf(bat, sizeof(bat), "BAT0");
		}
		if (colon && colon[1])
			fmt = colon + 1;

		if (!bat_read(POWER_SUPPLY_CAPACITY, bat, &v))
			perc = (int)v;
		sym = battery_state(bat);
		bat_colour(sym, col, sizeof(col));

		o.n = 0;
		o.s[0] = '\0';
		for (p = fmt; *p; p++) {
			switch (*p) {
			case 's':
				if (sym) {
					bat_sep(&o);
					bat_add(&o, "%s", sym);
				}
				break;
			case 'p':
				if (perc >= 0) {
					bat_sep(&o);
					bat_add(&o, "%s%d%%%s", BAT_COL_TEXT, perc, BAT_COL_END);
				}
				break;
			case 'b': {
				int w = BAT_BAR_WIDTH, digits = 0, val = 0;

				while (p[1 + digits] >= '0' && p[1 + digits] <= '9' && val < 1000)
					val = val * 10 + (p[1 + digits++] - '0');
				if (digits) {
					w = val < BAT_BAR_MIN ? BAT_BAR_MIN :
					    val > BAT_BAR_MAX ? BAT_BAR_MAX : val;
					p += digits;
				}
				if (perc >= 0)
					bat_bar(&o, col, perc, w);
				break;
			}
			case 'r': {
				/* battery_remaining() answers through the shared buf, so
				 * copy it out right away */
				const char *r = battery_remaining(bat);

				if (r && *r) {
					bat_sep(&o);
					bat_add(&o, "%s%s%s", BAT_COL_TEXT, r, BAT_COL_END);
				}
				break;
			}
			case 'w':
				if (!bat_watts(bat, &watts) && watts >= 0.05) {
					bat_sep(&o);
					bat_add(&o, "%s%.1fW%s", BAT_COL_TEXT, watts, BAT_COL_END);
				}
				break;
			default:
				break;
			}
		}

		return o.n ? o.s : NULL;
	}
#elif defined(__OpenBSD__)
	#include <fcntl.h>
	#include <machine/apmvar.h>
	#include <sys/ioctl.h>
	#include <unistd.h>

	static int
	load_apm_power_info(struct apm_power_info *apm_info)
	{
		int fd;

		fd = open("/dev/apm", O_RDONLY);
		if (fd < 0) {
			warn("open '/dev/apm':");
			return 0;
		}

		memset(apm_info, 0, sizeof(struct apm_power_info));
		if (ioctl(fd, APM_IOC_GETPOWER, apm_info) < 0) {
			warn("ioctl 'APM_IOC_GETPOWER':");
			close(fd);
			return 0;
		}
		return close(fd), 1;
	}

	const char *
	battery_perc(const char *unused)
	{
		struct apm_power_info apm_info;

		if (load_apm_power_info(&apm_info))
			return bprintf("%d", apm_info.battery_life);

		return NULL;
	}

	const char *
	battery_state(const char *unused)
	{
		struct {
			unsigned int state;
			char *symbol;
		} map[] = {
			{ APM_AC_ON,      "+" },
			{ APM_AC_OFF,     "-" },
		};
		struct apm_power_info apm_info;
		size_t i;

		if (load_apm_power_info(&apm_info)) {
			for (i = 0; i < LEN(map); i++)
				if (map[i].state == apm_info.ac_state)
					break;

			return (i == LEN(map)) ? "?" : map[i].symbol;
		}

		return NULL;
	}

	const char *
	battery_remaining(const char *unused)
	{
		struct apm_power_info apm_info;
		unsigned int h, m;

		if (load_apm_power_info(&apm_info)) {
			if (apm_info.ac_state != APM_AC_ON) {
				h = apm_info.minutes_left / 60;
				m = apm_info.minutes_left % 60;
				return bprintf("%uh %02um", h, m);
			} else {
				return "";
			}
		}

		return NULL;
	}
#elif defined(__FreeBSD__)
	#include <sys/sysctl.h>

	#define BATTERY_LIFE  "hw.acpi.battery.life"
	#define BATTERY_STATE "hw.acpi.battery.state"
	#define BATTERY_TIME  "hw.acpi.battery.time"

	const char *
	battery_perc(const char *unused)
	{
		int cap_perc;
		size_t len;

		len = sizeof(cap_perc);
		if (sysctlbyname(BATTERY_LIFE, &cap_perc, &len, NULL, 0) < 0 || !len)
			return NULL;

		return bprintf("%d", cap_perc);
	}

	const char *
	battery_state(const char *unused)
	{
		int state;
		size_t len;

		len = sizeof(state);
		if (sysctlbyname(BATTERY_STATE, &state, &len, NULL, 0) < 0 || !len)
			return NULL;

		switch (state) {
		case 0: /* FALLTHROUGH */
		case 2:
			return "+";
		case 1:
			return "-";
		default:
			return "?";
		}
	}

	const char *
	battery_remaining(const char *unused)
	{
		int rem;
		size_t len;

		len = sizeof(rem);
		if (sysctlbyname(BATTERY_TIME, &rem, &len, NULL, 0) < 0 || !len
		    || rem < 0)
			return NULL;

		return bprintf("%uh %02um", rem / 60, rem % 60);
	}
#endif

