#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <alsa/asoundlib.h>
#ifdef HAVE_NOTIFY
#include <libnotify/notify.h>
#endif

#define VERSION          "2.0"
#ifndef DEFAULT_CARD
#define DEFAULT_CARD     "hw:1"            /* T440p Analog PCH */
#endif
#define MAX_VOLUME       100               /* speaker cap in percent; lower it to protect your ears */
#define NOTIFY_TIMEOUT   1500              /* notification timeout, ms */
#define DEFAULT_FORMAT   "%i %v%%"
#define MAX_ACTIONS      64
#define BAR_WIDTH        10
#define DB_MUTE_FLOOR    (-1000000L)       /* ALSA reports "-inf" as -9999999 */

#define ICON_MUTED   "🔇"
#define ICON_LOW     "🔈"
#define ICON_MED     "🔉"
#define ICON_HIGH    "🔊"
#define ICON_MIC_ON  "🎤"
#define ICON_MIC_OFF "🚫"

enum target   { T_SPK, T_MIC, T_COUNT };
enum mute_req { MR_TOGGLE, MR_MUTE, MR_UNMUTE };
enum act_type { A_SET, A_INC, A_DEC, A_MUTE, A_MUTE_ALL, A_TARGET };
enum out_mode { O_NONE, O_FORMAT, O_NUMBER, O_JSON };

/* long-only options */
enum {
    OPT_MIXER = 256, OPT_MICMIXER, OPT_MIC, OPT_SPK, OPT_JSON
};

typedef struct {
    char card[64];
    char mixer[64];       /* "" = auto-detect */
    char mic_mixer[64];   /* "" = auto-detect */
    char format[128];
} Opts;

typedef struct {
    enum act_type type;
    int target;           /* T_SPK / T_MIC, or -1 = whatever is current */
    int val;              /* percent */
    int rel;              /* A_SET: value is relative (+N / -N) */
    int mode;             /* enum mute_req */
} Action;

typedef struct { Action v[MAX_ACTIONS]; int n; } Plan;

typedef struct {
    snd_mixer_elem_t *elem;
    const char       *name;
    int               capture;
} Ctl;

typedef struct {
    int    pct;
    int    muted;
    int    has_db;
    double db;
} State;

typedef struct {
    snd_mixer_t *h;
    Ctl          ctl[T_COUNT];
    int          ready[T_COUNT];
} Mixer;

static const char *prog = "vol";

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static void warnf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void usage_error(const char *fmt, ...)
    __attribute__((format(printf, 1, 2), noreturn));

static void warnf(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", prog);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void usage_error(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", prog);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\nTry '%s --help'.\n", prog);
    exit(2);
}

static void copy_str(char *dst, size_t n, const char *src)
{
    size_t len = strlen(src);
    if (len >= n) len = n - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static char *trim(char *s)
{
    char *e;
    while (isspace((unsigned char)*s)) s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static int parse_bool(const char *s)
{
    static const char *const yes[] = { "1", "yes", "true", "on", NULL };
    static const char *const no[]  = { "0", "no", "false", "off", NULL };
    for (int i = 0; yes[i]; i++) if (!strcasecmp(s, yes[i])) return 1;
    for (int i = 0; no[i]; i++)  if (!strcasecmp(s, no[i]))  return 0;
    return -1;
}

/* "50", "50%", "+5", "-5%"  ->  *val, *rel (1 if it carried a sign) */
static int parse_pct(const char *s, int *val, int *rel)
{
    const char *p = s;
    char *end; long v; while (isspace((unsigned char)*p)) p++;
    errno = 0;
    v = strtol(p, &end, 10);
    if (end == p || errno || v < -1000 || v > 1000) return -1;
    if (*end == '%') end++;
    if (*end != '\0') return -1;
    *rel = (*p == '+' || *p == '-');
    *val = (int)v;
    return 0;
}

static int parse_mute(const char *arg)
{
    int b;
    if (!arg || !strcasecmp(arg, "toggle")) return MR_TOGGLE;
    b = parse_bool(arg);
    if (b < 0) usage_error("invalid mute mode '%s' (use toggle, on or off)", arg);
    return b ? MR_MUTE : MR_UNMUTE;
}

/* ------------------------------------------------------------------ */
/* settings: defaults, config file, environment                        */
/* ------------------------------------------------------------------ */

static void opts_default(Opts *o)
{
    memset(o, 0, sizeof *o);
    copy_str(o->card, sizeof o->card, DEFAULT_CARD);
    copy_str(o->format, sizeof o->format, DEFAULT_FORMAT);
}

/* Shared by the config file and the command line.
 * Returns 0 on success, -1 for a bad value, -2 for an unknown key. */
static int set_opt(Opts *o, const char *key, const char *val)
{
    if (!strcmp(key, "card")) {
        int digits = 1;
        if (!*val) return -1;
        for (const char *p = val; *p; p++)
            if (!isdigit((unsigned char)*p)) digits = 0;
        if (digits) {
            char tmp[64];
            snprintf(tmp, sizeof tmp, "hw:%.40s", val);
            copy_str(o->card, sizeof o->card, tmp);
        } else {
            copy_str(o->card, sizeof o->card, val);
        }
    } else if (!strcmp(key, "mixer")) {
        copy_str(o->mixer, sizeof o->mixer, val);
    } else if (!strcmp(key, "mic_mixer")) {
        copy_str(o->mic_mixer, sizeof o->mic_mixer, val);
    } else if (!strcmp(key, "format")) {
        copy_str(o->format, sizeof o->format, val);
    } else {
        return -2;
    }
    return 0;
}

static void cli_opt(Opts *o, const char *key, const char *val)
{
    if (set_opt(o, key, val) < 0)
        usage_error("invalid value '%s' for %s", val, key);
}

static void load_config(Opts *o)
{
    char path[512], line[256];
    const char *env = getenv("VOL_CONFIG");
    int explicit_path = env && *env;
    int ln = 0;
    FILE *f;

    if (explicit_path) {
        copy_str(path, sizeof path, env);
    } else {
        const char *xdg  = getenv("XDG_CONFIG_HOME");
        const char *home = getenv("HOME");
        if (xdg && *xdg)        snprintf(path, sizeof path, "%.400s/vol/config", xdg);
        else if (home && *home) snprintf(path, sizeof path, "%.400s/.config/vol/config", home);
        else return;
    }

    f = fopen(path, "r");
    if (!f) {
        if (explicit_path) warnf("cannot read %s: %s", path, strerror(errno));
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq, *key, *val;
        ln++;
        if (!*s || *s == '#') continue;
        eq = strchr(s, '=');
        if (!eq) { warnf("%s:%d: expected 'key = value'", path, ln); continue; }
        *eq = '\0';
        key = trim(s);
        val = trim(eq + 1);
        switch (set_opt(o, key, val)) {
        case -1: warnf("%s:%d: invalid value '%s' for %s", path, ln, val, key); break;
        case -2: warnf("%s:%d: unknown setting '%s'", path, ln, key); break;
        default: break;
        }
    }
    fclose(f);
}

static void load_env(Opts *o)
{
    const char *v;
    if ((v = getenv("VOL_CARD")) && *v && set_opt(o, "card", v) < 0)
        warnf("ignoring invalid VOL_CARD='%s'", v);
    if ((v = getenv("VOL_MIXER")) && *v)
        set_opt(o, "mixer", v);
}

/* ------------------------------------------------------------------ */
/* thin ALSA wrappers: one Ctl is either a playback or capture element */
/* ------------------------------------------------------------------ */

static int c_has_vol(const Ctl *c)
{
    return c->capture ? snd_mixer_selem_has_capture_volume(c->elem)
                      : snd_mixer_selem_has_playback_volume(c->elem);
}

static int c_has_sw(const Ctl *c)
{
    return c->capture ? snd_mixer_selem_has_capture_switch(c->elem)
                      : snd_mixer_selem_has_playback_switch(c->elem);
}

static int c_has_chan(const Ctl *c, int ch)
{
    snd_mixer_selem_channel_id_t id = (snd_mixer_selem_channel_id_t)ch;
    return c->capture ? snd_mixer_selem_has_capture_channel(c->elem, id)
                      : snd_mixer_selem_has_playback_channel(c->elem, id);
}

static int c_range(const Ctl *c, long *min, long *max)
{
    return c->capture ? snd_mixer_selem_get_capture_volume_range(c->elem, min, max)
                      : snd_mixer_selem_get_playback_volume_range(c->elem, min, max);
}

/* average over all channels so a slightly unbalanced L/R still reads sanely */
static int c_get_raw(const Ctl *c, long *out)
{
    long sum = 0;
    int n = 0;

    for (int ch = 0; ch <= SND_MIXER_SCHN_LAST; ch++) {
        long v;
        int r;
        if (!c_has_chan(c, ch)) continue;
        r = c->capture
            ? snd_mixer_selem_get_capture_volume(c->elem, (snd_mixer_selem_channel_id_t)ch, &v)
            : snd_mixer_selem_get_playback_volume(c->elem, (snd_mixer_selem_channel_id_t)ch, &v);
        if (r < 0) continue;
        sum += v;
        n++;
    }
    if (!n) return -1;
    *out = (sum + n / 2) / n;
    return 0;
}

static int c_set_raw(const Ctl *c, long raw)
{
    return c->capture ? snd_mixer_selem_set_capture_volume_all(c->elem, raw)
                      : snd_mixer_selem_set_playback_volume_all(c->elem, raw);
}

static int c_get_db(const Ctl *c, long *db)
{
    return c->capture
        ? snd_mixer_selem_get_capture_dB(c->elem, SND_MIXER_SCHN_FRONT_LEFT, db)
        : snd_mixer_selem_get_playback_dB(c->elem, SND_MIXER_SCHN_FRONT_LEFT, db);
}

/* sw == 1 means audible, sw == 0 means muted (ALSA's convention) */
static int c_get_sw(const Ctl *c, int *sw)
{
    return c->capture
        ? snd_mixer_selem_get_capture_switch(c->elem, SND_MIXER_SCHN_FRONT_LEFT, sw)
        : snd_mixer_selem_get_playback_switch(c->elem, SND_MIXER_SCHN_FRONT_LEFT, sw);
}

static int c_set_sw(const Ctl *c, int sw)
{
    return c->capture ? snd_mixer_selem_set_capture_switch_all(c->elem, sw)
                      : snd_mixer_selem_set_playback_switch_all(c->elem, sw);
}

/* ------------------------------------------------------------------ */
/* percent <-> raw conversion (respects a non-zero minimum)            */
/* ------------------------------------------------------------------ */

static int raw_to_pct(long raw, long min, long max)
{
    long range = max - min, p;
    if (range <= 0) return 100;
    p = ((raw - min) * 100 + range / 2) / range;
    return (int)(p < 0 ? 0 : p > 100 ? 100 : p);
}

static long pct_to_raw(int pct, long min, long max)
{
    long range = max - min;
    long raw = min + ((long)pct * range + 50) / 100;
    return raw < min ? min : raw > max ? max : raw;
}

static int get_state(const Ctl *c, State *s)
{
    long min, max, raw, db;

    memset(s, 0, sizeof *s);
    s->pct = 100;                               /* switch-only controls */
    if (c_has_vol(c)) {
        if (c_range(c, &min, &max) < 0 || c_get_raw(c, &raw) < 0) {
            warnf("cannot read '%s'", c->name);
            return -1;
        }
        s->pct = raw_to_pct(raw, min, max);
        if (c_get_db(c, &db) == 0 && db > DB_MUTE_FLOOR) {
            s->has_db = 1;
            s->db = (double)db / 100.0;
        }
    }
    if (c_has_sw(c)) {
        int sw = 1;
        if (c_get_sw(c, &sw) == 0) s->muted = !sw;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* opening the mixer and locating controls                             */
/* ------------------------------------------------------------------ */

static snd_mixer_t *mixer_open(const char *card, int quiet)
{
    snd_mixer_t *h = NULL;
    int err;

    if ((err = snd_mixer_open(&h, 0)) < 0) {
        if (!quiet) warnf("cannot open mixer: %s", snd_strerror(err));
        return NULL;
    }
    if ((err = snd_mixer_attach(h, card)) < 0 ||
        (err = snd_mixer_selem_register(h, NULL, NULL)) < 0 ||
        (err = snd_mixer_load(h)) < 0) {
        if (!quiet) warnf("card '%s': %s (try --list)", card, snd_strerror(err));
        snd_mixer_close(h);
        return NULL;
    }
    return h;
}

/* accepts "Name" or "Name,N" for the N-th element of that name */
static snd_mixer_elem_t *find_elem(snd_mixer_t *h, const char *name)
{
    snd_mixer_selem_id_t *sid;
    char buf[64], *comma;
    unsigned idx = 0;

    copy_str(buf, sizeof buf, name);
    comma = strrchr(buf, ',');
    if (comma) {
        char *end;
        unsigned long v = strtoul(comma + 1, &end, 10);
        if (end != comma + 1 && *end == '\0') {
            idx = (unsigned)v;
            *comma = '\0';
        }
    }
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, idx);
    snd_mixer_selem_id_set_name(sid, buf);
    return snd_mixer_find_selem(h, sid);
}

static const char *const SPK_FALLBACK[] = { "Master", "PCM", "Speaker", "Headphone", NULL };
static const char *const MIC_FALLBACK[] = { "Capture", "Mic", "Internal Mic", NULL };

/* Resolve (and cache) the control for a target; warns and returns NULL if
 * nothing suitable exists. */
static Ctl *mixer_ctl(Mixer *m, int t, const Opts *o)
{
    Ctl *c = &m->ctl[t];
    const char *wanted = (t == T_MIC) ? o->mic_mixer : o->mixer;
    const char *const *fb = (t == T_MIC) ? MIC_FALLBACK : SPK_FALLBACK;

    if (m->ready[t]) return c;

    c->capture = (t == T_MIC);
    c->elem = NULL;
    if (*wanted) {
        c->elem = find_elem(m->h, wanted);
        if (!c->elem) {
            warnf("no control named '%s' on %s (try --list)", wanted, o->card);
            return NULL;
        }
    } else {
        for (; *fb && !c->elem; fb++) {
            snd_mixer_elem_t *e = find_elem(m->h, *fb);
            Ctl probe = { e, *fb, c->capture };
            if (e && (c_has_vol(&probe) || c_has_sw(&probe))) c->elem = e;
        }
        if (!c->elem) {
            warnf("no %s control found on %s (try --list)",
                  t == T_MIC ? "capture" : "playback", o->card);
            return NULL;
        }
    }
    c->name = snd_mixer_selem_get_name(c->elem);
    m->ready[t] = 1;
    return c;
}

/* ------------------------------------------------------------------ */
/* operations                                                          */
/* ------------------------------------------------------------------ */

static int apply_volume(Ctl *c, const Action *a)
{
    long min, max, cur, raw;
    int cur_pct, target, cap;
    int dir = 0;                       /* signed change requested, in percent */

    if (!c_has_vol(c)) {
        warnf("'%s' has no volume control", c->name);
        return -1;
    }
    if (c_range(c, &min, &max) < 0 || c_get_raw(c, &cur) < 0) {
        warnf("cannot read '%s'", c->name);
        return -1;
    }
    cur_pct = raw_to_pct(cur, min, max);

    switch (a->type) {
    case A_INC: dir =  a->val; break;
    case A_DEC: dir = -a->val; break;
    case A_SET: if (a->rel) dir = a->val; break;
    default: break;
    }
    target = (a->type == A_SET && !a->rel) ? a->val : cur_pct + dir;

    cap = c->capture ? 100 : MAX_VOLUME;
    if (target > cap) target = cap;
    if (target < 0)   target = 0;

    raw = pct_to_raw(target, min, max);
    /* Coarse mixers (few raw steps) can round a small step back onto the
     * current value and leave the key "stuck"; nudge one raw step instead. */
    if (dir > 0 && target > cur_pct && raw <= cur && cur < pct_to_raw(cap, min, max))
        raw = cur + 1;
    else if (dir < 0 && target < cur_pct && raw >= cur && cur > min)
        raw = cur - 1;

    if (c_set_raw(c, raw) < 0) {
        warnf("cannot set '%s'", c->name);
        return -1;
    }
    /* Changing speaker volume unmutes it (as before); never touch the mic switch. */
    if (!c->capture && c_has_sw(c)) c_set_sw(c, 1);
    return 0;
}

static int apply_mute(Ctl *c, int mode)
{
    int sw = 1, want;

    if (!c_has_sw(c)) {
        warnf("'%s' has no mute switch", c->name);
        return -1;
    }
    c_get_sw(c, &sw);
    want = (mode == MR_TOGGLE) ? !sw : (mode == MR_UNMUTE);
    if (c_set_sw(c, want) < 0) {
        warnf("cannot change mute on '%s'", c->name);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* output: status line, number, JSON                                   */
/* ------------------------------------------------------------------ */

static const char *icon_for(int t, const State *s)
{
    if (t == T_MIC) return s->muted ? ICON_MIC_OFF : ICON_MIC_ON;
    if (s->muted)   return ICON_MUTED;
    return s->pct > 70 ? ICON_HIGH : s->pct > 30 ? ICON_MED : ICON_LOW;
}

static void print_format(FILE *out, const char *fmt, int t, const Ctl *c, const State *s)
{
    for (const char *p = fmt; *p; p++) {
        if (*p != '%' || p[1] == '\0') { fputc(*p, out); continue; }
        switch (*++p) {
        case 'i': fputs(icon_for(t, s), out); break;
        case 'v': fprintf(out, "%d", s->pct); break;
        case 's': fputs(s->muted ? "muted" : "on", out); break;
        case 'n': fputs(c->name, out); break;
        case 'd':
            if (s->has_db) fprintf(out, "%.1f", s->db);
            else           fputc('?', out);
            break;
        case 'b': {
            int fill = (s->pct * BAR_WIDTH + 50) / 100;
            for (int i = 0; i < BAR_WIDTH; i++) fputs(i < fill ? "█" : "░", out);
            break;
        }
        case '%': fputc('%', out); break;
        default:  fputc('%', out); fputc(*p, out); break;
        }
    }
    fputc('\n', out);
}

static void json_str(FILE *out, const char *s)
{
    fputc('"', out);
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20) continue;
        if (*s == '"' || *s == '\\') fputc('\\', out);
        fputc(*s, out);
    }
    fputc('"', out);
}

static void print_state(FILE *out, enum out_mode om, const Opts *o, int t,
                        const Ctl *c, const State *s)
{
    switch (om) {
    case O_NUMBER:
        fprintf(out, "%d\n", s->pct);
        break;
    case O_JSON:
        fputs("{\"target\":", out);
        json_str(out, t == T_MIC ? "mic" : "speaker");
        fputs(",\"control\":", out);
        json_str(out, c->name);
        fprintf(out, ",\"volume\":%d,\"muted\":%s,\"icon\":", s->pct,
                s->muted ? "true" : "false");
        json_str(out, icon_for(t, s));
        if (s->has_db) fprintf(out, ",\"db\":%.2f", s->db);
        fputs("}\n", out);
        break;
    default:
        print_format(out, o->format, t, c, s);
        break;
    }
    fflush(out);
}

/* ------------------------------------------------------------------ */
/* desktop notifications                                               */
/* ------------------------------------------------------------------ */

#ifdef HAVE_NOTIFY
static int notify_start(void) { return notify_init("vol-control"); }
static void notify_stop(void) { notify_uninit(); }

static void notify_state(int t, const State *s)
{
    const char *title, *icon, *tag;
    char body[48];
    NotifyNotification *n;
    GError *err = NULL;

    if (t == T_MIC) {
        title = "Microphone";
        tag   = "vol-mic";
        icon  = s->muted    ? "microphone-sensitivity-muted"
              : s->pct > 66 ? "microphone-sensitivity-high"
              : s->pct > 33 ? "microphone-sensitivity-medium"
                            : "microphone-sensitivity-low";
    } else {
        title = "Audio";
        tag   = "vol";
        icon  = (s->muted || s->pct == 0) ? "audio-volume-muted"
              : s->pct < 30 ? "audio-volume-low"
              : s->pct < 70 ? "audio-volume-medium"
                            : "audio-volume-high";
    }
    if (s->muted) snprintf(body, sizeof body, "Muted");
    else          snprintf(body, sizeof body, "%s: %d%%",
                           t == T_MIC ? "Level" : "Volume", s->pct);

    n = notify_notification_new(title, body, icon);
    if (!n) return;
    notify_notification_set_urgency(n, NOTIFY_URGENCY_LOW);
    notify_notification_set_timeout(n, NOTIFY_TIMEOUT);
    notify_notification_set_hint(n, "value", g_variant_new_int32(s->muted ? 0 : s->pct));
    /* the same tag makes the daemon replace the previous popup instead of stacking */
    notify_notification_set_hint(n, "x-canonical-private-synchronous", g_variant_new_string(tag));
    notify_notification_set_hint(n, "x-dunst-stack-tag", g_variant_new_string(tag));
    if (!notify_notification_show(n, &err)) g_clear_error(&err);
    g_object_unref(G_OBJECT(n));
}
#else
static int notify_start(void) { return 0; }
static void notify_stop(void) { }
static void notify_state(int t, const State *s)
{
    (void)t; (void)s;
}
#endif

/* ------------------------------------------------------------------ */
/* --list                                                              */
/* ------------------------------------------------------------------ */

static int list_devices(const char *selected)
{
    int card = -1, found = 0;

    while (snd_card_next(&card) >= 0 && card >= 0) {
        char hw[32], *cname = NULL;
        snd_mixer_t *h;

        snprintf(hw, sizeof hw, "hw:%d", card);
        if (snd_card_get_name(card, &cname) < 0) cname = NULL;
        printf("%c %s  %s\n", strcmp(hw, selected) == 0 ? '*' : ' ', hw,
               cname ? cname : "(unknown)");
        free(cname);
        found++;

        h = mixer_open(hw, 1);
        if (!h) continue;
        for (snd_mixer_elem_t *e = snd_mixer_first_elem(h); e; e = snd_mixer_elem_next(e)) {
            char label[80];
            unsigned idx;
            if (!snd_mixer_selem_is_active(e)) continue;
            idx = snd_mixer_selem_get_index(e);
            if (idx) snprintf(label, sizeof label, "%.60s,%u", snd_mixer_selem_get_name(e), idx);
            else     snprintf(label, sizeof label, "%.70s", snd_mixer_selem_get_name(e));
            printf("      %-26s %s%s%s%s\n", label,
                   snd_mixer_selem_has_playback_volume(e) ? "pvol " : "",
                   snd_mixer_selem_has_playback_switch(e) ? "psw "  : "",
                   snd_mixer_selem_has_capture_volume(e)  ? "cvol " : "",
                   snd_mixer_selem_has_capture_switch(e)  ? "csw "  : "");
        }
        snd_mixer_close(h);
    }
    if (!found) {
        warnf("no sound cards found");
        return 1;
    }
    puts("\n  * = selected card;  pvol/psw = playback volume/switch,"
         "  cvol/csw = capture volume/switch");
    return 0;
}

/* ------------------------------------------------------------------ */
/* command line                                                        */
/* ------------------------------------------------------------------ */

static void usage(FILE *out)
{
    fprintf(out, "Usage: %s [OPTIONS]\n"
                 "Operations run in the order given. With no options, prints the status.\n\n",
            prog);
    fputs(
"Volume\n"
"  -s, --set N         Set volume to N%  (+N / -N change it relatively)\n"
"  -i, --inc N         Raise volume by N%\n"
"  -d, --dec N         Lower volume by N%\n"
"\n"
"Mute        MODE is toggle (default), on (= muted) or off (= audible), e.g. --mute=off\n"
"  -m, --mute[=MODE]      Mute the current target (the speaker unless --mic was given)\n"
"  -c, --micmute[=MODE]   Mute the microphone\n"
"  -M, --MUTE[=MODE]      Speaker and microphone together; a toggle mutes both if\n"
"                         either is live, and unmutes both only if both are muted\n"
"\n"
"Target\n"
"      --mic           Options after this act on the microphone\n"
"      --speaker       Options after this act on the speaker (the default)\n"
"\n"
"Output      (printed once, after all operations; the last one given wins)\n"
"  -b, --bar           Status line, default format \"" DEFAULT_FORMAT "\"\n"
"  -g, --get           Just the volume number\n"
"      --json          One JSON object\n"
"  -f, --format FMT    Status line format (implies -b):\n"
"                        %i icon   %v volume   %s on/muted   %b text bar\n"
"                        %d dB     %n control name   %% a literal %\n"
"\n"
"Device\n"
"  -D, --card CARD     ALSA card: N, hw:N or a device name (default " DEFAULT_CARD ")\n"
"      --mixer NAME    Speaker control (default: first of Master, PCM, Speaker, Headphone)\n"
"      --mic-mixer N   Microphone control (default: first of Capture, Mic, Internal Mic)\n"
"  -l, --list          List sound cards and their mixer controls\n"
"\n"
"Other\n"
"  -q, --quiet         No desktop notification\n"
"  -h, --help          This text\n"
"  -V, --version       Version\n"
"\n"
"Config file ($VOL_CONFIG or ~/.config/vol/config), one 'key = value' per line:\n"
"  card, mixer, mic_mixer, format\n"
"Environment: VOL_CARD, VOL_MIXER, VOL_CONFIG.  Flags override both.\n"
"Exit status: 0 ok, 1 ALSA/runtime error, 2 bad usage.\n", out);
}

int main(int argc, char **argv)
{
    static const struct option long_opts[] = {
        {"bar",       no_argument,       0, 'b'},
        {"get",       no_argument,       0, 'g'},
        {"set",       required_argument, 0, 's'},
        {"inc",       required_argument, 0, 'i'},
        {"dec",       required_argument, 0, 'd'},
        {"mute",      optional_argument, 0, 'm'},
        {"micmute",   optional_argument, 0, 'c'},
        {"MUTE",      optional_argument, 0, 'M'},
        {"mic",       no_argument,       0, OPT_MIC},
        {"speaker",   no_argument,       0, OPT_SPK},
        {"json",      no_argument,       0, OPT_JSON},
        {"format",    required_argument, 0, 'f'},
        {"card",      required_argument, 0, 'D'},
        {"mixer",     required_argument, 0, OPT_MIXER},
        {"mic-mixer", required_argument, 0, OPT_MICMIXER},
        {"list",      no_argument,       0, 'l'},
        {"quiet",     no_argument,       0, 'q'},
        {"help",      no_argument,       0, 'h'},
        {"version",   no_argument,       0, 'V'},
        {0, 0, 0, 0}
    };

    Opts o;
    Plan plan;
    Mixer m;
    enum out_mode om = O_NONE;
    int quiet = 0, do_list = 0;
    int touched[T_COUNT] = { 0, 0 };
    int cur = T_SPK, rc = 0, c;
    const char *slash = strrchr(argv[0], '/');

    prog = slash ? slash + 1 : argv[0];
    memset(&plan, 0, sizeof plan);
    memset(&m, 0, sizeof m);

    opts_default(&o);
    load_config(&o);
    load_env(&o);

    while ((c = getopt_long(argc, argv, "bgs:i:d:m::c::M::f:D:lqhV", long_opts, NULL)) != -1) {
        int v, rel;
        switch (c) {
        case 's':
            if (parse_pct(optarg, &v, &rel) < 0)
                usage_error("invalid volume '%s' (use N, +N or -N, optionally with %%)", optarg);
            if (plan.n >= MAX_ACTIONS) usage_error("too many operations");
            plan.v[plan.n++] = (Action){ .type = A_SET, .target = -1, .val = v, .rel = rel };
            break;
        case 'i':
        case 'd':
            if (parse_pct(optarg, &v, &rel) < 0 || v < 0)
                usage_error("invalid amount '%s'", optarg);
            if (plan.n >= MAX_ACTIONS) usage_error("too many operations");
            plan.v[plan.n++] = (Action){ .type = (c == 'i') ? A_INC : A_DEC, .target = -1, .val = v };
            break;
        case 'm':
        case 'c':
            if (plan.n >= MAX_ACTIONS) usage_error("too many operations");
            plan.v[plan.n++] = (Action){ .type = A_MUTE, .target = (c == 'c') ? T_MIC : -1,
                                         .mode = parse_mute(optarg) };
            break;
        case 'M':
            if (plan.n >= MAX_ACTIONS) usage_error("too many operations");
            plan.v[plan.n++] = (Action){ .type = A_MUTE_ALL, .target = -1, .mode = parse_mute(optarg) };
            break;
        case OPT_MIC:
        case OPT_SPK:
            if (plan.n >= MAX_ACTIONS) usage_error("too many operations");
            plan.v[plan.n++] = (Action){ .type = A_TARGET, .target = (c == OPT_MIC) ? T_MIC : T_SPK };
            break;
        case 'b': om = O_FORMAT; break;
        case 'g': om = O_NUMBER; break;
        case OPT_JSON: om = O_JSON; break;
        case 'f':
            cli_opt(&o, "format", optarg);
            if (om == O_NONE) om = O_FORMAT;
            break;
        case 'l': do_list = 1; break;
        case 'D': cli_opt(&o, "card", optarg); break;
        case OPT_MIXER: cli_opt(&o, "mixer", optarg); break;
        case OPT_MICMIXER: cli_opt(&o, "mic_mixer", optarg); break;
        case 'q': quiet = 1; break;
        case 'h': usage(stdout); return 0;
        case 'V': printf("%s %s\n", prog, VERSION); return 0;
        default:
            fprintf(stderr, "Try '%s --help'.\n", prog);
            return 2;
        }
    }
    if (optind < argc) usage_error("unexpected argument '%s'", argv[optind]);

    if (do_list) return list_devices(o.card);
    if (!plan.n && om == O_NONE) om = O_FORMAT;   /* bare invocation = status */

    m.h = mixer_open(o.card, 0);
    if (!m.h) return 1;

    for (int i = 0; i < plan.n; i++) {
        const Action *a = &plan.v[i];
        int t = (a->target < 0) ? cur : a->target;
        Ctl *ctl;

        switch (a->type) {
        case A_TARGET:
            cur = a->target;
            break;
        case A_SET:
        case A_INC:
        case A_DEC:
        case A_MUTE:
            if (!(ctl = mixer_ctl(&m, t, &o))) { rc = 1; break; }
            if ((a->type == A_MUTE ? apply_mute(ctl, a->mode) : apply_volume(ctl, a)) < 0)
                rc = 1;
            else
                touched[t] = 1;
            break;
        case A_MUTE_ALL: {
            Ctl *sp = mixer_ctl(&m, T_SPK, &o);
            Ctl *mc = mixer_ctl(&m, T_MIC, &o);
            int mode = a->mode;
            State st;

            if (!sp || !mc) rc = 1;                /* mixer_ctl already said why */
            if (mode == MR_TOGGLE) {
                int live = 0;
                if (sp && get_state(sp, &st) == 0 && !st.muted) live = 1;
                if (mc && get_state(mc, &st) == 0 && !st.muted) live = 1;
                mode = live ? MR_MUTE : MR_UNMUTE;
            }
            if (sp) { if (apply_mute(sp, mode) < 0) rc = 1; else touched[T_SPK] = 1; }
            if (mc) { if (apply_mute(mc, mode) < 0) rc = 1; else touched[T_MIC] = 1; }
            break;
        }
        }
        snd_mixer_handle_events(m.h);              /* pick up our own writes */
    }

    if (om != O_NONE) {
        Ctl *ctl = mixer_ctl(&m, cur, &o);
        State s;
        if (ctl && get_state(ctl, &s) == 0) print_state(stdout, om, &o, cur, ctl, &s);
        else rc = 1;
    }
    if (!quiet && (touched[T_SPK] || touched[T_MIC]) && notify_start()) {
        for (int t = 0; t < T_COUNT; t++) {
            Ctl *ctl;
            State s;
            if (touched[t] && (ctl = mixer_ctl(&m, t, &o)) && get_state(ctl, &s) == 0)
                notify_state(t, &s);
        }
        notify_stop();
    }

    snd_mixer_close(m.h);
    return rc;
}

