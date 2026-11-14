#ifndef WALLDB_H
#define WALLDB_H

#include <stddef.h>

/*
 * Wallpaper library database (<base>.db). Rows are keyed by path relative to
 * the master dir and carry a display name, one category, a purity (0 unrated,
 * 1 SFW, 2 Sketchy, 3 NSFW) and any number of tags. A category is just an exact
 * name: "Anime/Eva" is a category of its own, not a child of "Anime".
 *
 * Views: "*" everything rated, "?" inbox (unrated or no category),
 * "Category" (exactly that category), "@tag" (includes "tag/..." children).
 */

/* ── Daemon side (read-only, no writes, no sleeping) ───────────────────── */

int  walldb_open(const char *base);              /* 0 on success, safe to repeat */
int  walldb_set_purity(int p);                   /* max purity shown 1..3, starts at 1; -1 if out of range */
int  walldb_get_purity(void);
int  walldb_category(const char *path, char *out, size_t outsz); /* the wall's category ("" none); -1 when unknown */
int  walldb_list(const char *view, char ***out); /* paths in view, <= 10000; caller frees; returns count */
int  walldb_next_view(const char *cur, int dir, char *out, size_t outsz); /* "*", "?", then categories; wraps */
void walldb_close(void);
int walldb_get_meta(const char *key, char *out, size_t outsz);
int walldb_set_meta(const char *key, const char *value);

/* ── CLI side (read-write, creates the schema on first use) ────────────── */

int  walldb_open_rw(const char *base);
int  walldb_level(const char *s);                           /* "0".."3" or unrated/SFW/Sketchy/NSFW (any case) -> 0..3, else -1 */
int  walldb_scan(const char *root, const char *const *tiers, int ntiers, int *skipped);  /* new files in root; returns count added, -1 on error; *skipped = non-image files ignored.
                                                       "Folder=N" tiers are remembered (and applied by every later scan); "Folder=-" forgets one */
int  walldb_info(const char *path);                         /* prints the row to stdout */
int  walldb_rate(const char *path, int purity);             /* 0..3 */
int  walldb_rate_view(const char *view, int purity);        /* everything in a Category or @tag; returns rows changed */
int  walldb_move(const char *path, const char *spec);        /* move a wallpaper inside its category: "12", "+3", "-2", "first", "last".
                                                               Returns the new position, -1 on a bad request, -2 when it has no category */
int  walldb_set_category(const char *path, const char *cat);/* "" or "-" clears */
int  walldb_set_category_folder(const char *folder, const char *cat, int level, int *rated);
                                                            /* every file under a physical folder ("." = all); rows changed.
                                                               level 1..3 also rates the *unrated* ones (*rated = how many) */
int  walldb_clear_category(const char *cat);                  /* remove a category from every wallpaper that has it; rows changed */
int  walldb_unrated(const char *cat);                         /* unrated rows in a category (NULL = whole library) */
int  walldb_exists(const char *path);                        /* is this relative path in the database? */
int  walldb_print_cats(void);                               /* "category<TAB>total<TAB>unrated<TAB>hidden" per category, in cycle order; returns rows, -1 on error */
int  walldb_cat_move(const char *cat, const char *spec);    /* move a category in the cycle of views: "12", "+3", "-2", "first", "last".
                                                               Returns the new position, -1 on a bad request, -2 when no wall has it */
int  walldb_cat_hide(const char *cat, int hide);            /* 1: its walls leave "*", tags, random and the slideshow, and it leaves the cycle
                                                               (its own view still opens by name); 0: back. -2 when no wall has it */
int  walldb_print_walls(const char *view);                  /* "path<TAB>purity<TAB>category<TAB>name<TAB>tags" for a view ("*" = all, ignores the allowed purity) */
int  walldb_total(void);                                    /* rows in the library */
int  walldb_rename(const char *from, const char *to);       /* a file or folder moved: its rows follow (tags and ratings too) */
void walldb_forget(void);                                   /* after fork(): drop the inherited handle without touching it */
int  walldb_print_tiers(void);                              /* "folder<TAB>level" for the remembered scan mappings */
int  walldb_prune(const char *root);                        /* drop rows whose file is gone; returns count, -1 if it refused */
int  walldb_set_name(const char *path, const char *name);
int  walldb_tag(const char *path, const char *tag, int add);/* add=0 removes */

#endif

