#define _GNU_SOURCE
#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "walldb.h"
#include "wallwatch.h"

#define QUIET_MS 1000     /* a burst (copying a folder in) counts as one change; wait this long for it to settle */
#define WATCH_MASK (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR)

struct wd { int wd; char *rel; };        /* a watched directory; rel is relative to the root, "" for the root itself */
struct mv { unsigned cookie; char *rel; };
struct rn { char *from, *to; };

static char root[PATH_MAX], dbpath[PATH_MAX];            /* root: the master dir; dbbase: the database's file name, which lives in it */
static char dbbase[256];
static size_t dbbaselen;
static size_t rootlen;
static int ifd = -1;
static int dirty, rewatch_needed, warned_limit, warned_empty;
static struct wd *wds;     static int nwds, wdcap;
static struct mv *moves;   static int nmoves, movecap;       /* MOVED_FROM events still waiting for their MOVED_TO */
static struct rn *renames; static int nrenames, renamecap;

static void *grow(void *p, int n, int *cap, size_t sz) {
    if (n < *cap) return p;
    int nc = *cap ? *cap * 2 : 32;
    void *q = realloc(p, (size_t)nc * sz);
    if (!q) { fputs("wallwatch: out of memory\n", stderr); _exit(1); }
    *cap = nc;
    return q;
}

static char *dup_or_die(const char *s) {
    char *d = strdup(s);
    if (!d) { fputs("wallwatch: out of memory\n", stderr); _exit(1); }
    return d;
}

/* ── watches ───────────────────────────────────────────────────────────── */

static int add_dir(const char *fpath, const struct stat *sb, int type, struct FTW *ftw) {
    (void)sb;
    if (type != FTW_D) return FTW_CONTINUE;
    if (ftw->level > 0 && fpath[ftw->base] == '.') return FTW_SKIP_SUBTREE;      /* hidden, like scan */

    int wd = inotify_add_watch(ifd, fpath, WATCH_MASK);
    if (wd < 0) {
        if (errno == ENOSPC && !warned_limit) {
            warned_limit = 1;
            fputs("wallwatch: inotify watch limit reached, some folders are not watched (raise fs.inotify.max_user_watches)\n", stderr);
        }
        return FTW_CONTINUE;
    }
    wds = grow(wds, nwds, &wdcap, sizeof(*wds));
    wds[nwds].wd = wd;
    wds[nwds].rel = dup_or_die(ftw->level ? fpath + rootlen + (rootlen > 1) : "");
    nwds++;
    return FTW_CONTINUE;
}

/* (re)build every watch from scratch: after a folder appeared, vanished or moved */
static void rewatch(void) {
    if (ifd >= 0) close(ifd);
    for (int i = 0; i < nwds; i++) free(wds[i].rel);
    nwds = 0;
    ifd = inotify_init1(IN_CLOEXEC);
    if (ifd < 0) { perror("wallwatch: inotify_init1"); _exit(1); }
    if (root[0]) nftw(root, add_dir, 16, FTW_ACTIONRETVAL);
}

/* The database sits in the master dir, so its own writes (and SQLite's -journal / -wal / -shm beside it)
 * show up as events there. They are not changes to the library: reacting to them would loop forever,
 * because every scan writes the database. */
static int is_db_file(const char *name) {
    if (strncmp(name, dbbase, dbbaselen) != 0) return 0;
    const char *rest = name + dbbaselen;
    return !*rest || !strcmp(rest, "-journal") || !strcmp(rest, "-wal") || !strcmp(rest, "-shm");
}

static const char *dir_of(int wd) {
    for (int i = 0; i < nwds; i++)
        if (wds[i].wd == wd) return wds[i].rel;
    return NULL;
}

static char *join_rel(const char *dir, const char *name) {
    size_t dl = strlen(dir), nl = strlen(name);
    char *s = malloc(dl + 1 + nl + 1);
    if (!s) { fputs("wallwatch: out of memory\n", stderr); _exit(1); }
    memcpy(s, dir, dl);
    if (dl) s[dl++] = '/';
    memcpy(s + dl, name, nl + 1);
    return s;
}

/* ── events ────────────────────────────────────────────────────────────── */

static void on_event(const struct inotify_event *e) {
    if (e->mask & IN_Q_OVERFLOW) { dirty = rewatch_needed = 1; return; }      /* lost events: look at everything */
    if (e->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) { dirty = rewatch_needed = 1; return; }
    if (e->len == 0 || e->name[0] == '.') return;                             /* hidden: scan skips those too (.wallman-temp) */

    const char *dir = dir_of(e->wd);
    if (dir && !dir[0] && is_db_file(e->name)) return;                        /* our own database, in the root */

    dirty = 1;
    if (e->mask & IN_ISDIR) rewatch_needed = 1;
    if (!dir) return;
    if (e->mask & IN_MOVED_FROM) {
        moves = grow(moves, nmoves, &movecap, sizeof(*moves));
        moves[nmoves].cookie = e->cookie;
        moves[nmoves].rel = join_rel(dir, e->name);
        nmoves++;
    } else if (e->mask & IN_MOVED_TO) {
        for (int i = 0; i < nmoves; i++)
            if (moves[i].cookie == e->cookie) {                               /* both halves seen: a rename, not delete + add */
                renames = grow(renames, nrenames, &renamecap, sizeof(*renames));
                renames[nrenames].from = moves[i].rel;
                renames[nrenames].to = join_rel(dir, e->name);
                nrenames++;
                moves[i] = moves[--nmoves];
                break;
            }
    }
}

static void process_batch(void (*notify)(void)) {
    if (rewatch_needed) { rewatch(); rewatch_needed = 0; }       /* before scanning, so files in a brand-new folder aren't missed */

    if (walldb_open_rw(dbpath) == 0 && root[0]) {
        if (walldb_total() > 0) {
            for (int i = 0; i < nrenames; i++) walldb_rename(renames[i].from, renames[i].to);
            walldb_scan(root, NULL, 0, NULL);
            walldb_prune(root);
        } else if (!warned_empty) {
            warned_empty = 1;
            fputs("wallwatch: library not imported yet, run 'wallman --scan' once (the watcher takes over after that)\n", stderr);
        }
    }

    for (int i = 0; i < nrenames; i++) { free(renames[i].from); free(renames[i].to); }
    for (int i = 0; i < nmoves; i++) free(moves[i].rel);         /* moved out of the library: prune handles those */
    nrenames = nmoves = 0;
    notify();                                                    /* reload even when nothing changed: another daemon may share this DB */
}

static void loop(void (*notify)(void)) {
    char buf[16384] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        struct pollfd p = { .fd = ifd, .events = POLLIN };
        int timeout = -1;
        for (;;) {
            int r = poll(&p, 1, timeout);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            ssize_t n = read(ifd, buf, sizeof(buf));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) _exit(1);
            for (char *q = buf; q < buf + n; ) {
                const struct inotify_event *e = (const struct inotify_event *)q;
                on_event(e);
                q += sizeof(*e) + e->len;
            }
            timeout = dirty ? QUIET_MS : -1;                     /* the quiet timer only runs once something relevant happened */
        }
        if (dirty) { process_batch(notify); dirty = 0; }
    }
}

pid_t wallwatch_start(const char *master, const char *db_path, void (*notify)(void)) {
    size_t ml = master ? strlen(master) : 0, bl = db_path ? strlen(db_path) : 0;
    if (ml == 0 || ml >= sizeof(root) || bl == 0 || bl >= sizeof(dbpath)) return -1;

    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid != 0) return pid;

    /* ── the watcher process ── */
    prctl(PR_SET_PDEATHSIG, SIGTERM);                            /* gone when the daemon is, however it dies */
    if (getppid() != parent) _exit(0);
    signal(SIGTERM, SIG_DFL);                                    /* the daemon's handlers were copied by fork */
    signal(SIGINT, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    syscall(SYS_close_range, 3, ~0U, 0);                         /* the daemon's socket, X connection, database... */
    walldb_forget();                                             /* and its inherited SQLite handle */

    memcpy(dbpath, db_path, bl + 1);
    memcpy(root, master, ml + 1);
    while (ml > 1 && root[ml - 1] == '/') root[--ml] = '\0';
    rootlen = ml;
    const char *base = strrchr(dbpath, '/');
    base = base ? base + 1 : dbpath;
    dbbaselen = strlen(base);
    if (dbbaselen >= sizeof(dbbase)) dbbaselen = sizeof(dbbase) - 1;
    memcpy(dbbase, base, dbbaselen);
    dbbase[dbbaselen] = '\0';

    walldb_open_rw(dbpath);                                      /* an old database gets its positions numbered right away */
    dirty = rewatch_needed = 0;                                  /* nothing to catch up on at start */

    rewatch();
    loop(notify);
    _exit(0);
}

