/* See LICENSE file for copyright and license details. */
#include <alsa/asoundlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../slstatus.h"
#include "../util.h"

#define CARD "hw:1" /* keep in sync with EVVOL_CARD in events.c */

/*
 * vol_perc(fmt): fmt is the third field of the config.h entry. Like the "nit"
 * of music_status it is a string of letters, drawn left to right in the order
 * you write them, one space between pieces.
 *
 *   lowercase = speaker (Master)      UPPERCASE = microphone (Capture)
 *
 *   i / I   icon (speaker level or mute / mic live or muted)
 *   v / V   volume percent
 *   b / B   bar; digits right after it set the width, b16 = 16 cells
 *   s / S   the word "muted", drawn only while muted
 *   d / D   level in dB, drawn only if the control reports one
 *
 * Unknown characters are skipped. NULL draws DEFAULT_FMT.
 *   "iv"      🔉 56%            "ivb"    🔉 56% ██████░░░░
 *   "ib16"    🔉 ████████░░░░░░░░         "ivbIV"  speaker, bar, then mic
 */
#define DEFAULT_FMT "iv"

/* bar: default width, limits for the width suffix, and the two cell glyphs */
#define BAR_WIDTH 10
#define BAR_MIN   2
#define BAR_MAX   40
#define BAR_FULL  "█"
#define BAR_EMPTY "░"

/* colours (Nord, same palette as music.c) */
#define COL_TEXT  "^c#D8DEE9^" /* percentages */
#define COL_BAR   "^c#88C0D0^" /* filled cells */
#define COL_EMPTY "^c#4C566A^" /* empty cells */
#define COL_MUTED "^c#BF616A^" /* filled cells while muted, the word "muted" */
#define COL_DB    "^c#EBCB8B^" /* dB readout */
#define COL_END   "^d^"

#define ICON_MUTED   "🔇"
#define ICON_LOW     "🔈"
#define ICON_MED     "🔉"
#define ICON_HIGH    "🔊"
#define ICON_MIC_ON  "🎤"
#define ICON_MIC_OFF "🚫"

struct chan {
	int found;  /* control exists and was read */
	int pct;
	int muted;
	int has_db;
	double db;
};

struct ob {
	char s[768];
	size_t n;
};

static void
ob_add(struct ob *o, const char *fmt, ...)
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
ob_sep(struct ob *o)
{
	if (o->n)
		ob_add(o, " ");
}

/* percent over the raw range, averaged over L/R; the mute switch is ALSA's
 * (1 = audible, 0 = muted) */
static int
chan_read(snd_mixer_elem_t *e, int cap, struct chan *c)
{
	snd_mixer_selem_channel_id_t ch;
	long min, max, v, sum = 0, db;
	int i, n = 0, sw = 1;

	memset(c, 0, sizeof(*c));
	c->pct = 100; /* a switch-only control counts as full */

	if (cap ? snd_mixer_selem_has_capture_volume(e)
	        : snd_mixer_selem_has_playback_volume(e)) {
		if (cap ? snd_mixer_selem_get_capture_volume_range(e, &min, &max)
		        : snd_mixer_selem_get_playback_volume_range(e, &min, &max))
			return -1;
		for (i = 0; i <= SND_MIXER_SCHN_LAST; i++) {
			ch = (snd_mixer_selem_channel_id_t)i;
			if (!(cap ? snd_mixer_selem_has_capture_channel(e, ch)
			          : snd_mixer_selem_has_playback_channel(e, ch)))
				continue;
			if (cap ? snd_mixer_selem_get_capture_volume(e, ch, &v)
			        : snd_mixer_selem_get_playback_volume(e, ch, &v))
				continue;
			sum += v;
			n++;
		}
		if (!n)
			return -1;
		v = (sum + n / 2) / n;
		if (max > min)
			c->pct = (int)(((v - min) * 100 + (max - min) / 2) / (max - min));
		if (c->pct < 0)
			c->pct = 0;
		if (c->pct > 100)
			c->pct = 100;

		/* ALSA reports "-inf" (muted gain) as -9999999 */
		if (!(cap ? snd_mixer_selem_get_capture_dB(e, SND_MIXER_SCHN_FRONT_LEFT, &db)
		          : snd_mixer_selem_get_playback_dB(e, SND_MIXER_SCHN_FRONT_LEFT, &db)) &&
		    db > -1000000L) {
			c->has_db = 1;
			c->db = (double)db / 100.0;
		}
	}

	if (cap ? snd_mixer_selem_has_capture_switch(e)
	        : snd_mixer_selem_has_playback_switch(e)) {
		if (!(cap ? snd_mixer_selem_get_capture_switch(e, SND_MIXER_SCHN_FRONT_LEFT, &sw)
		          : snd_mixer_selem_get_playback_switch(e, SND_MIXER_SCHN_FRONT_LEFT, &sw)))
			c->muted = !sw;
	}

	c->found = 1;

	return 0;
}

static const char *
icon_for(const struct chan *c, int mic)
{
	if (mic)
		return c->muted ? ICON_MIC_OFF : ICON_MIC_ON;
	if (c->muted)
		return ICON_MUTED;

	return c->pct > 70 ? ICON_HIGH : c->pct > 30 ? ICON_MED : ICON_LOW;
}

static void
add_bar(struct ob *o, const struct chan *c, int width)
{
	int fill = (c->pct * width + 50) / 100;
	int i;

	ob_sep(o);
	if (fill > 0)
		ob_add(o, "%s", c->muted ? COL_MUTED : COL_BAR);
	for (i = 0; i < fill; i++)
		ob_add(o, BAR_FULL);
	if (fill < width)
		ob_add(o, "%s", COL_EMPTY);
	for (; i < width; i++)
		ob_add(o, BAR_EMPTY);
	ob_add(o, COL_END);
}

const char *
vol_perc(const char *fmt)
{
	static struct ob o;
	snd_mixer_t *h;
	snd_mixer_selem_id_t *sid;
	snd_mixer_elem_t *e;
	struct chan spk, mic;
	const char *p;

	if (snd_mixer_open(&h, 0) < 0)
		return NULL;
	if (snd_mixer_attach(h, CARD) < 0 ||
	    snd_mixer_selem_register(h, NULL, NULL) < 0 ||
	    snd_mixer_load(h) < 0) {
		snd_mixer_close(h);
		return NULL;
	}

	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_name(sid, "Master");
	if (!(e = snd_mixer_find_selem(h, sid)) || chan_read(e, 0, &spk) < 0) {
		snd_mixer_close(h);
		return NULL;
	}

	/* the mic is optional: without one the uppercase letters draw nothing */
	memset(&mic, 0, sizeof(mic));
	snd_mixer_selem_id_set_name(sid, "Capture");
	if ((e = snd_mixer_find_selem(h, sid)))
		chan_read(e, 1, &mic);

	snd_mixer_close(h);

	o.n = 0;
	o.s[0] = '\0';
	for (p = fmt ? fmt : DEFAULT_FMT; *p; p++) {
		int is_mic = (*p >= 'A' && *p <= 'Z');
		char key = is_mic ? (char)(*p + ('a' - 'A')) : *p;
		const struct chan *c = is_mic ? &mic : &spk;

		if (!c->found)
			continue;

		switch (key) {
		case 'i':
			ob_sep(&o);
			ob_add(&o, "%s", icon_for(c, is_mic));
			break;
		case 'v':
			ob_sep(&o);
			ob_add(&o, "%s%d%%%s ", COL_TEXT, c->pct, COL_END);
			break;
		case 'b': {
			int w = BAR_WIDTH, digits = 0, val = 0;

			while (p[1 + digits] >= '0' && p[1 + digits] <= '9' && val < 1000)
				val = val * 10 + (p[1 + digits++] - '0');
			if (digits) {
				w = val < BAR_MIN ? BAR_MIN : val > BAR_MAX ? BAR_MAX : val;
				p += digits;
			}
			add_bar(&o, c, w);
			break;
		}
		case 's':
			if (c->muted) {
				ob_sep(&o);
				ob_add(&o, "%sM%s", COL_MUTED, COL_END);
			}
			break;
		case 'd':
			if (c->has_db) {
				ob_sep(&o);
				ob_add(&o, "%s%.1fdB%s", COL_DB, c->db, COL_END);
			}
			break;
		default:
			break;
		}
	}

	return o.n ? o.s : NULL;
}

