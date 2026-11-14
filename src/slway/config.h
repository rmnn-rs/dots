/* interval between updates (in ms) */
const unsigned int interval = 1000;

/* text to show if no value can be retrieved */
static const char unknown_str[] = "n/a";

/* maximum output string length */
#define MAXLEN 2048

static struct arg args[] = {

	{ datetime,       " %s ^c#4C566A^ ^d^",                     "CHR %a %-d %b  %-I:%M %p ",                  0, {0}, 0 },
	{ battery_state,  " %s",                                     "BAT0",                                       0, {0}, 0 },
	{ battery_perc,   " ^c#FFFFFF^%s%% ^d^^c#4C566A^  ^d^",     "BAT0",                                      10, {0}, 0 },
//	{ vol_perc,       "^c#ffffff^ %s ^d^ ^c#4C566A^ ^d^",        NULL,                                        3, {0}, 0 },
	{ wifi_perc,      " %s ",                                    "wlan0",                                      5, {0}, 0 },
	{ wifi_essid,     " %s ^c#4C566A^  ^d^",                    "wlan0",                                      0, {0}, 0 },
	{ cpu_freq,       " ^c#88C0D0^^d^ ^c#ffffff^%sHz ^d^",       NULL,                                        4, {0}, 0 },
	{ temp,           "^c#ffffff^ %sC ^d^^c#4C566A^  ^d^",      "/sys/class/hwmon/hwmon1/temp1_input",        5, {0}, 0 },
	{ ram_used,       "^c#88C0D0^ ^d^ %s ",                      NULL,                                        5, {0}, 0 },
	{ ram_perc,       "^c#88C0D0^ ^d^ %s%% ^c#4C566A^  ^d^",    NULL,                                        5, {0}, 0 },
	{ get_wall_info,  " %s ^c#4C566A^  ^d^",                     NULL,                                        0, {0}, 0 },
	{ music_status,   "%s",                                      "nit",                                        1, {0}, 0 },


};

/* event sources: when the fd fires, the listed components refresh right away.
 * interval 0 components only ever run through these (or SIGUSR1).
 * sampled values (cpu, temp, ram, wifi signal) have no kernel event, they stay on timers */
static struct evsrc evsrcs[] = {
	/* init            drain            refreshes                                     */
	{ ev_minute_init, ev_minute_drain, { datetime },                    0 }, /* wall-clock minute, needs minute-resolution format */
	{ ev_uevent_init, ev_uevent_drain, { battery_state, battery_perc }, 0 }, /* AC plug / battery state */
	{ ev_link_init,   ev_link_drain,   { wifi_perc, wifi_essid },       0 }, /* carrier, (dis)association */
	{ ev_mpd_init,    ev_mpd_drain,    { music_status },                0 }, /* MPD idle: play/pause/track */
	{ ev_wall_init,   ev_wall_drain,   { get_wall_info },               0 }, /* wallman push: wall changed */
};

