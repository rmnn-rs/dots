
/* interval between updates (in ms) */
const unsigned int interval = 1000;

/* text to show if no value can be retrieved */
static const char unknown_str[] = "n/a";

/* maximum output string length */
#define MAXLEN 2048

static struct arg args[] = {

	{ datetime, " %s ^c#4C566A^ ^d^", "CHR %a %-d %b  %-I:%M %p ", 1, {0}, 0 },
	{ battery_state, " %s", "BAT0", 1, {0}, 0 },
	{ battery_perc, " ^c#FFFFFF^%s%% ^d^^c#4C566A^  ^d^ ", "BAT0", 5, {0}, 0 },
	{ wifi_perc, " %s ", "wlan0", 1, {0}, 0 },
	{ wifi_essid, " %s ^c#4C566A^  ^d^", "wlan0", 1, {0}, 0 },
	{ cpu_freq, " ^c#88C0D0^^d^ ^c#ffffff^%sHz ^d^", NULL, 5, {0}, 0 },
	{ temp, "^c#ffffff^ %sC ^d^^c#4C566A^  ^d^", "/sys/class/hwmon/hwmon1/temp1_input", 3, {0}, 0 },
	{ ram_used, "^c#88C0D0^ ^d^ %s ", NULL, 5, {0}, 0 },
	{ ram_perc, "^c#88C0D0^ ^d^ %s%% ^c#4C566A^  ^d^", NULL, 5, {0}, 0 },
	{ get_wall_info, " %s ^c#4C566A^  ^d^", NULL, 3, {0}, 0 },
	{ music_status, "%s   ", "nit", 1, {0}, 0 },


};

