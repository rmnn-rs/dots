#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <limits.h>
#include <ftw.h>
#include <errno.h>
#include <sys/stat.h>
#include <sqlite3.h>
#include "walldb.h"

static sqlite3 *db;
static int purity = 3;

static const char SCHEMA[] =
    "PRAGMA foreign_keys=ON;"
    "CREATE TABLE IF NOT EXISTS wall("
    "  path     TEXT PRIMARY KEY NOT NULL,"       /* relative to the master dir */
    "  name     TEXT NOT NULL,"
    "  category TEXT NOT NULL DEFAULT '',"
    "  purity   INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS tag("
    "  path TEXT NOT NULL REFERENCES wall(path) ON DELETE CASCADE ON UPDATE CASCADE,"
    "  tag  TEXT NOT NULL,"
    "  PRIMARY KEY(path, tag));"
    "CREATE TABLE IF NOT EXISTS tier(folder TEXT PRIMARY KEY NOT NULL, level INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS wall_by_category ON wall(category);"
    "CREATE INDEX IF NOT EXISTS tag_by_tag ON tag(tag, path);";

/* ?1 = max purity, ?2 = category / tag. 'x/' <= c < 'x0' is the index-friendly "starts with x/" */
#define ORDER " ORDER BY name COLLATE NATSORT, path LIMIT 10000"

static const char Q_ALL[] =
    "SELECT path FROM wall WHERE purity BETWEEN 1 AND ?1" ORDER;
static const char Q_INBOX[] =
    "SELECT path FROM wall WHERE purity <= ?1 AND (purity = 0 OR category = '')" ORDER;
#define IN_CAT "(category = ?2 OR (category >= ?2 || '/' AND category < ?2 || '0'))"
/* a physical folder (path prefix); "." means the whole library */
#define IN_FOLDER "(?2 = '.' OR (path >= ?2 || '/' AND path < ?2 || '0'))"
#define IN_TAG "path IN (SELECT path FROM tag WHERE tag = ?2 OR (tag >= ?2 || '/' AND tag < ?2 || '0'))"

static const char Q_CAT[] = "SELECT path FROM wall WHERE purity BETWEEN 1 AND ?1 AND " IN_CAT ORDER;
static const char Q_TAG[] = "SELECT path FROM wall WHERE purity BETWEEN 1 AND ?1 AND " IN_TAG ORDER;
static const char Q_VIEWS[] =
    "SELECT DISTINCT category FROM wall WHERE category <> '' AND purity BETWEEN 1 AND ?1 "
    "ORDER BY category COLLATE NOCASE";

/* ── helpers ───────────────────────────────────────────────────────────── */

/* The old natural_sort from wall.c, as a collation: a2 < a10, case-insensitive */
static int natural(void *u, int n1, const void *p1, int n2, const void *p2) {
    (void)u;
    const unsigned char *a = p1, *ae = a + n1, *b = p2, *be = b + n2;
    while (a < ae && b < be) {
        if (isdigit(*a) && isdigit(*b)) {
            unsigned long x = 0, y = 0;
            while (a < ae && isdigit(*a)) x = x * 10 + (unsigned long)(*a++ - '0');
            while (b < be && isdigit(*b)) y = y * 10 + (unsigned long)(*b++ - '0');
            if (x != y) return x < y ? -1 : 1;
        } else {
            int ca = tolower(*a), cb = tolower(*b);
            if (ca != cb) return ca - cb;
            a++; b++;
        }
    }
    return (a < ae) - (b < be);
}

struct vec { char **v; int n, cap; };

static int push(struct vec *a, const char *s) {
    if (!s) return 0;
    if (a->n >= a->cap) {
        int nc = a->cap ? a->cap * 2 : 16;
        char **t = realloc(a->v, sizeof(char *) * (size_t)nc);
        if (!t) return -1;
        a->v = t;
        a->cap = nc;
    }
    char *d = strdup(s);
    if (!d) return -1;
    a->v[a->n++] = d;
    return 0;
}

static void vec_free(struct vec *a) {
    for (int i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
}

static void run(const char *sql, const char *arg, struct vec *a) {
    sqlite3_stmt *st;
    if (!db || sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(st, 1, purity);
    if (arg) sqlite3_bind_text(st, 2, arg, -1, SQLITE_STATIC);
    while (sqlite3_step(st) == SQLITE_ROW)
        if (push(a, (const char *)sqlite3_column_text(st, 0)) < 0) break;
    sqlite3_finalize(st);
}

/* One number out of a query: ?1 is the allowed purity, ?2 an optional text argument */
static int scalar(const char *sql, const char *arg) {
    sqlite3_stmt *st;
    int n = -1;
    if (db && sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, purity);
        if (arg) sqlite3_bind_text(st, 2, arg, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

static int open_db(const char *base, int flags) {
    if (db) return 0;
    char path[PATH_MAX];
    size_t n = base ? strlen(base) : 0;
    if (n == 0 || n + 4 > sizeof(path)) return -1;
    memcpy(path, base, n);
    memcpy(path + n, ".db", 4);

    if (sqlite3_open_v2(path, &db, flags, NULL) != SQLITE_OK) {
        fprintf(stderr, "walldb: %s: %s\n", path, sqlite3_errmsg(db));
        sqlite3_close(db);
        db = NULL;
        return -1;
    }
    sqlite3_create_collation(db, "NATSORT", SQLITE_UTF8, NULL, natural);
    /* no temp files: the daemon's sandbox can't create them */
    sqlite3_exec(db, "PRAGMA temp_store=MEMORY", NULL, NULL, NULL);
    return 0;
}

/* ── daemon side ───────────────────────────────────────────────────────── */

int walldb_open(const char *base) {
    /* No busy timeout on purpose: SQLite would sleep, and the daemon's seccomp
     * filter doesn't allow that. A reload that races a writer just comes back empty. */
    return open_db(base, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX);
}

int walldb_set_purity(int p) {
    if (p < 1 || p > 3) return -1;
    purity = p;
    return 0;
}

int walldb_get_purity(void) { return purity; }

int walldb_list(const char *view, char ***out) {
    struct vec a = {0};
    if (!view || !view[0] || !strcmp(view, "*")) run(Q_ALL,   NULL,     &a);
    else if (!strcmp(view, "?"))                 run(Q_INBOX, NULL,     &a);
    else if (view[0] == '@')                     run(Q_TAG,   view + 1, &a);
    else                                         run(Q_CAT,   view,     &a);
    *out = a.v;
    return a.n;
}

int walldb_next_view(const char *cur, int dir, char *out, size_t outsz) {
    struct vec a = {0};
    /* only views with something to show: an empty one would just fall back to "*" and look like nothing happened */
    if (scalar("SELECT EXISTS(SELECT 1 FROM wall WHERE purity BETWEEN 1 AND ?1)", NULL) > 0) push(&a, "*");
    if (scalar("SELECT EXISTS(SELECT 1 FROM wall WHERE purity <= ?1 AND (purity = 0 OR category = ''))", NULL) > 0) push(&a, "?");
    run(Q_VIEWS, NULL, &a);

    int i = -1, rc = -1;
    for (int k = 0; k < a.n; k++)
        if (cur && !strcmp(a.v[k], cur)) i = k;
    if (i < 0) i = dir > 0 ? -1 : 0;                        /* the current view isn't in the cycle (any more): start from an end */
    if (a.n > 0) {
        const char *next = a.v[((i + dir) % a.n + a.n) % a.n];
        size_t len = strlen(next);
        if (len < outsz) { memcpy(out, next, len + 1); rc = 0; }
    }
    vec_free(&a);
    return rc;
}

/* ── CLI side ──────────────────────────────────────────────────────────── */

int walldb_open_rw(const char *base) {
    if (open_db(base, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) < 0) return -1;
    sqlite3_busy_timeout(db, 5000);
    if (sqlite3_exec(db, SCHEMA, NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "walldb: schema: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

/* Runs a write with up to two text binds. Returns rows changed, or -1. */
static int write_sql(const char *sql, const char *a, const char *b) {
    sqlite3_stmt *st;
    int rc = -1;
    if (db && sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, a, -1, SQLITE_STATIC);
        if (b) sqlite3_bind_text(st, 2, b, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_DONE) rc = sqlite3_changes(db);
        sqlite3_finalize(st);
    }
    return rc;
}

static int txn(const char *sql) {
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

int walldb_level(const char *s) {
    static const char *const names[] = { "unrated", "sfw", "sketchy", "nsfw" };
    for (int i = 0; i < 4; i++)
        if (!strcasecmp(s, names[i])) return i;
    return (s[0] >= '0' && s[0] <= '3' && !s[1]) ? s[0] - '0' : -1;   /* s[1] only read once s[0] is a digit */
}

/* "*", "?" and leading "@" are view syntax, so they can't start a category or tag */
static int bad_name(const char *s) {
    return !s[0] || s[0] == '*' || s[0] == '?' || s[0] == '@';
}

int walldb_rate(const char *path, int p) {
    if (p < 0 || p > 3) return -1;
    char v[2] = { (char)('0' + p), '\0' };
    return write_sql("UPDATE wall SET purity = CAST(?1 AS INTEGER) WHERE path = ?2", v, path) > 0 ? 0 : -1;
}

int walldb_rate_view(const char *view, int p) {
    if (p < 0 || p > 3 || !view[0] || !strcmp(view, "*") || !strcmp(view, "?")) return -1;
    char v[2] = { (char)('0' + p), '\0' };
    if (view[0] == '@')
        return write_sql("UPDATE wall SET purity = CAST(?1 AS INTEGER) WHERE " IN_TAG, v, view + 1);
    return write_sql("UPDATE wall SET purity = CAST(?1 AS INTEGER) WHERE " IN_CAT, v, view);
}

/* "Anime/" -> "Anime", "-" -> "" (no category); rejects names that collide with view syntax */
static int clean_category(const char *cat, char *buf, size_t sz) {
    size_t n = strcmp(cat, "-") ? strlen(cat) : 0;
    while (n > 0 && cat[n - 1] == '/') n--;
    if (n >= sz) return -1;
    memcpy(buf, cat, n);
    buf[n] = '\0';
    return (n && bad_name(buf)) ? -1 : 0;
}

int walldb_set_category(const char *path, const char *cat) {
    char buf[256];
    if (clean_category(cat, buf, sizeof(buf)) < 0) return -1;
    return write_sql("UPDATE wall SET category = ?1 WHERE path = ?2", buf, path) > 0 ? 0 : -1;
}

int walldb_set_category_folder(const char *folder, const char *cat, int level, int *rated) {
    char buf[256], f[PATH_MAX];
    size_t n = strlen(folder);
    while (n > 1 && folder[n - 1] == '/') n--;
    if (clean_category(cat, buf, sizeof(buf)) < 0 || n == 0 || n >= sizeof(f) || level < 0 || level > 3) return -1;
    memcpy(f, folder, n);
    f[n] = '\0';
    if (rated) *rated = 0;

    int rows = -1, r = 0, ok = txn("BEGIN") == 0;
    if (ok) ok = (rows = write_sql("UPDATE wall SET category = ?1 WHERE " IN_FOLDER, buf, f)) >= 0;
    if (ok && level > 0) {                                  /* only the unrated ones: never touches an existing rating */
        char v[2] = { (char)('0' + level), '\0' };
        ok = (r = write_sql("UPDATE wall SET purity = CAST(?1 AS INTEGER) WHERE purity = 0 AND " IN_FOLDER, v, f)) >= 0;
    }
    if (ok) ok = txn("COMMIT") == 0;
    if (!ok) {
        txn("ROLLBACK");
        fputs("walldb: update failed, nothing was changed\n", stderr);
        return -1;
    }
    if (rated) *rated = r;
    return rows;
}

int walldb_set_name(const char *path, const char *name) {
    if (!name[0]) return -1;
    return write_sql("UPDATE wall SET name = ?1 WHERE path = ?2", name, path) > 0 ? 0 : -1;
}

int walldb_tag(const char *path, const char *tag, int add) {
    if (add) {
        if (bad_name(tag)) return -1;
        return write_sql("INSERT OR IGNORE INTO tag(path, tag) VALUES(?1, ?2)", path, tag) >= 0 ? 0 : -1;
    }
    return write_sql("DELETE FROM tag WHERE path = ?1 AND tag = ?2", path, tag) >= 0 ? 0 : -1;
}

int walldb_info(const char *path) {
    static const char *const label[] = { "unrated", "SFW", "Sketchy", "NSFW" };
    sqlite3_stmt *st;
    int rc = -1;

    if (db && sqlite3_prepare_v2(db, "SELECT name, category, purity FROM wall WHERE path = ?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            int p = sqlite3_column_int(st, 2);
            printf("name:     %s\npath:     %s\ncategory: %s\npurity:   %d (%s)\n",
                   sqlite3_column_text(st, 0), path, sqlite3_column_text(st, 1),
                   p, label[p >= 0 && p <= 3 ? p : 0]);
            rc = 0;
        }
        sqlite3_finalize(st);
    }
    if (rc < 0) return -1;

    printf("tags:    ");
    if (sqlite3_prepare_v2(db, "SELECT tag FROM tag WHERE path = ?1 ORDER BY tag", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
        while (sqlite3_step(st) == SQLITE_ROW)
            printf(" %s", sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
    }
    printf("\n");
    return 0;
}

int walldb_total(void) {
    return scalar("SELECT count(*) FROM wall", NULL);
}

void walldb_forget(void) {
    db = NULL;
}

/* A file or a whole folder moved: update the rows in place so tags and ratings stay (the tag rows follow via ON UPDATE CASCADE) */
int walldb_rename(const char *from, const char *to) {
    return write_sql("UPDATE wall SET path = ?2 || substr(path, length(?1) + 1) "
                     "WHERE path = ?1 OR (path >= ?1 || '/' AND path < ?1 || '0')", from, to);
}

int walldb_unrated(const char *cat) {
    char buf[256];
    if (!cat) return scalar("SELECT count(*) FROM wall WHERE purity = 0", NULL);
    if (clean_category(cat, buf, sizeof(buf)) < 0 || !buf[0]) return -1;
    return scalar("SELECT count(*) FROM wall WHERE purity = 0 AND " IN_CAT, buf);
}

int walldb_exists(const char *path) {
    return scalar("SELECT count(*) FROM wall WHERE path = ?2", path) > 0;
}

int walldb_clear_category(const char *cat) {
    char buf[256];
    if (clean_category(cat, buf, sizeof(buf)) < 0 || !buf[0]) return -1;
    return write_sql("UPDATE wall SET category = ?1 WHERE " IN_CAT, "", buf);
}

/* ── listings for scripts: tab-separated, to stdout ────────────────────── */

#define LIST_SELECT \
    "SELECT path, purity, category, name, " \
    "(SELECT group_concat(tag, ',') FROM tag WHERE tag.path = wall.path) FROM wall"
#define LIST_ORDER " ORDER BY category COLLATE NOCASE, name COLLATE NATSORT, path"

static int print_rows(const char *sql, const char *arg) {
    sqlite3_stmt *st;
    int n = -1;
    if (db && sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (arg) sqlite3_bind_text(st, 2, arg, -1, SQLITE_STATIC);
        int cols = sqlite3_column_count(st);
        for (n = 0; sqlite3_step(st) == SQLITE_ROW; n++)
            for (int i = 0; i < cols; i++) {
                const unsigned char *t = sqlite3_column_text(st, i);
                printf("%s%s%s", i ? "\t" : "", t ? (const char *)t : "", i == cols - 1 ? "\n" : "");
            }
        sqlite3_finalize(st);
    }
    return n;
}

int walldb_print_cats(void) {
    return print_rows("SELECT category, count(*), sum(purity = 0) FROM wall WHERE category <> '' "
                      "GROUP BY category ORDER BY category COLLATE NOCASE", NULL);
}

int walldb_print_walls(const char *view) {
    if (!view[0] || !strcmp(view, "*")) return print_rows(LIST_SELECT LIST_ORDER, NULL);
    if (!strcmp(view, "?"))             return print_rows(LIST_SELECT " WHERE (purity = 0 OR category = '')" LIST_ORDER, NULL);
    if (view[0] == '@')                 return print_rows(LIST_SELECT " WHERE " IN_TAG LIST_ORDER, view + 1);
    return print_rows(LIST_SELECT " WHERE " IN_CAT LIST_ORDER, view);
}

int walldb_print_tiers(void) {
    return print_rows("SELECT folder, level FROM tier ORDER BY folder COLLATE NOCASE", NULL);
}

/* Drops rows whose file no longer exists (their tags go with them). Refuses when the
 * master dir isn't readable or when *every* file is missing: that is an unmounted drive. */
int walldb_prune(const char *root) {
    struct stat st;
    char full[PATH_MAX];
    size_t rl = root ? strlen(root) : 0;
    if (!db || rl == 0 || stat(root, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "walldb: %s is not accessible, nothing pruned\n", root);
        return -1;
    }

    struct vec gone = {0};
    int total = 0, oom = 0;
    sqlite3_stmt *sel;
    if (sqlite3_prepare_v2(db, "SELECT path FROM wall", -1, &sel, NULL) != SQLITE_OK) return -1;
    while (sqlite3_step(sel) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(sel, 0);
        size_t pl = strlen(p);
        total++;
        if (rl + 1 + pl >= sizeof(full)) continue;
        memcpy(full, root, rl);
        full[rl] = '/';
        memcpy(full + rl + 1, p, pl + 1);
        if (stat(full, &st) < 0 && (errno == ENOENT || errno == ENOTDIR) && push(&gone, p) < 0) {
            oom = 1;                                        /* other stat errors (EACCES, EIO) don't count as gone */
            break;
        }
    }
    sqlite3_finalize(sel);
    if (oom) { vec_free(&gone); return -1; }

    if (total > 0 && gone.n == total) {
        fputs("walldb: every file is missing (unmounted drive?), nothing pruned\n", stderr);
        vec_free(&gone);
        return -1;
    }
    int ok = txn("BEGIN") == 0;
    for (int i = 0; ok && i < gone.n; i++)
        ok = write_sql("DELETE FROM wall WHERE path = ?1", gone.v[i], NULL) >= 0;
    if (ok) ok = txn("COMMIT") == 0;
    if (!ok) {
        txn("ROLLBACK");
        fputs("walldb: prune failed, nothing was removed\n", stderr);
    }

    int n = ok ? gone.n : -1;
    vec_free(&gone);
    return n;
}

/* ── scan: register files that aren't in the database yet ──────────────── */

static sqlite3_stmt *ins;
static size_t scan_rootlen;
static int scan_added, scan_skipped, scan_err;

/* the remembered "Folder=N" mappings, loaded for the walk */
#define MAX_TIERS 64
static struct { char folder[256]; size_t len; int level; } scan_tier[MAX_TIERS];
static int scan_ntiers;

static int is_image(const char *name) {
    const char *ext = strrchr(name, '.');
    return ext && (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".png") || !strcasecmp(ext, ".jpeg") ||
                   !strcasecmp(ext, ".webp") || !strcasecmp(ext, ".ff"));
}

#define TIER_REMOVE (-2)

/* "Folder=N": N (0..3 or a level name), TIER_REMOVE for "Folder=-", -1 when malformed; *len = length of Folder */
static int tier_arg(const char *t, size_t *len) {
    *len = 0;
    const char *eq = strchr(t, '=');
    if (!eq || eq == t || (size_t)(eq - t) >= sizeof(scan_tier[0].folder)) return -1;
    *len = (size_t)(eq - t);
    return strcmp(eq + 1, "-") ? walldb_level(eq + 1) : TIER_REMOVE;
}

/* store (or forget) the tiers given on the command line */
static int save_tiers(const char *const *tiers, int ntiers) {
    for (int i = 0; i < ntiers; i++) {
        char f[256], v[2];
        size_t nl;
        int lvl = tier_arg(tiers[i], &nl);
        if (lvl == -1 || nl == 0) return -1;                    /* walldb_scan checked already; never trust that here */
        memcpy(f, tiers[i], nl);
        while (nl > 1 && f[nl - 1] == '/') nl--;
        f[nl] = '\0';
        v[0] = (char)('0' + (lvl > 0 ? lvl : 0));
        v[1] = '\0';
        int rc = lvl == TIER_REMOVE ? write_sql("DELETE FROM tier WHERE folder = ?1", f, NULL)
                                    : write_sql("INSERT OR REPLACE INTO tier(folder, level) VALUES(?1, CAST(?2 AS INTEGER))", f, v);
        if (rc < 0) return -1;
    }
    return 0;
}

static int load_tiers(void) {
    sqlite3_stmt *st;
    scan_ntiers = 0;
    if (sqlite3_prepare_v2(db, "SELECT folder, level FROM tier", -1, &st, NULL) != SQLITE_OK) return -1;
    while (scan_ntiers < MAX_TIERS && sqlite3_step(st) == SQLITE_ROW) {
        const char *f = (const char *)sqlite3_column_text(st, 0);
        size_t n = f ? strlen(f) : 0;
        if (n == 0 || n >= sizeof(scan_tier[0].folder)) continue;
        memcpy(scan_tier[scan_ntiers].folder, f, n + 1);
        scan_tier[scan_ntiers].len = n;
        scan_tier[scan_ntiers].level = sqlite3_column_int(st, 1);
        scan_ntiers++;
    }
    sqlite3_finalize(st);
    return 0;
}

static int visit(const char *fpath, const struct stat *sb, int type, struct FTW *ftw) {
    (void)sb;
    const char *file = fpath + ftw->base;
    if (file[0] == '.' && ftw->level > 0) return type == FTW_D ? FTW_SKIP_SUBTREE : FTW_CONTINUE;
    if (type != FTW_F) return FTW_CONTINUE;
    if (!is_image(file)) { scan_skipped++; return FTW_CONTINUE; }

    /* directory part below the root becomes the category: "Anime/Eva" */
    const char *rel = fpath + scan_rootlen + (scan_rootlen > 1);   /* root "/" already ends in a slash */
    size_t dlen = (size_t)(file - rel);
    if (dlen) dlen--;                                           /* drop the trailing '/' */
    char cat[256];
    if (dlen >= sizeof(cat)) return FTW_CONTINUE;
    memcpy(cat, rel, dlen);
    cat[dlen] = '\0';

    /* a remembered "Folder=N" turns a tier folder into a purity instead of a category */
    int p = 0;
    const char *c = cat;
    for (int i = 0; i < scan_ntiers; i++) {
        size_t nl = scan_tier[i].len;
        if (dlen >= nl && !memcmp(cat, scan_tier[i].folder, nl) && (cat[nl] == '\0' || cat[nl] == '/')) {
            p = scan_tier[i].level;
            c = cat + nl + (cat[nl] ? 1 : 0);
            break;
        }
    }

    char name[256];
    size_t nlen = strlen(file);
    const char *dot = strrchr(file, '.');
    if (dot && dot != file) nlen = (size_t)(dot - file);
    if (nlen >= sizeof(name)) return FTW_CONTINUE;
    memcpy(name, file, nlen);
    name[nlen] = '\0';

    sqlite3_reset(ins);
    /* the statement outlives this call, so SQLite must copy these (they live on our stack / nftw's) */
    sqlite3_bind_text(ins, 1, rel,  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 3, c,    -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (ins, 4, p);
    if (sqlite3_step(ins) == SQLITE_DONE) scan_added += sqlite3_changes(db);
    else scan_err = 1;
    return FTW_CONTINUE;
}

int walldb_scan(const char *root, const char *const *tiers, int ntiers, int *skipped) {
    char r[PATH_MAX];
    size_t n = strlen(root);
    while (n > 1 && root[n - 1] == '/') n--;
    if (!db || n == 0 || n >= sizeof(r)) return -1;
    for (int i = 0; i < ntiers; i++) {
        size_t nl;
        if (tier_arg(tiers[i], &nl) == -1) {
            fprintf(stderr, "walldb: bad tier '%s' (want Folder=0..3, Folder=SFW|Sketchy|NSFW, or Folder=- to forget it)\n", tiers[i]);
            return -1;
        }
    }
    memcpy(r, root, n);
    r[n] = '\0';

    scan_rootlen = n;
    scan_added = scan_skipped = scan_err = 0;
    ins = NULL;

    int rc = (txn("BEGIN") == 0 && save_tiers(tiers, ntiers) == 0 && load_tiers() == 0) ? 0 : -1;
    if (rc == 0) {
        rc = sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO wall(path, name, category, purity) VALUES(?1, ?2, ?3, ?4)",
                                -1, &ins, NULL) == SQLITE_OK ? nftw(r, visit, 16, FTW_ACTIONRETVAL) : -1;
        sqlite3_finalize(ins);
    }
    if (rc < 0 || scan_err || txn("COMMIT") < 0) {
        txn("ROLLBACK");
        fputs("walldb: scan failed, nothing was added\n", stderr);
        return -1;
    }
    if (skipped) *skipped = scan_skipped;
    return scan_added;
}

