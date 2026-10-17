#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <time.h>
#include <libgen.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <wayland-client.h>
#include <Imlib2.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wall.h"
#include "walldb.h"

int count = 0;
int cur = 0;
char current_folder[256] = "";
char master_dir[1024] = "";
static char **files = NULL;

/* Internal Renderer State */
typedef struct Output {
    struct Output *next;
    uint32_t name;  /* registry name, to match global_remove */
    struct wl_output *wl_out;
    struct wl_surface *surf;
    struct zwlr_layer_surface_v1 *ls;
    int w, h;   /* from layer-surface configure, falls back to mode */
    int mw, mh; /* current wl_output mode */
} Output;

static struct wl_display *wl_dpy = NULL;
static struct wl_compositor *compositor = NULL;
static struct wl_shm *shm = NULL;
static struct zwlr_layer_shell_v1 *layer_shell = NULL;
static Output *outputs = NULL;
static int ready = 0;   /* setup finished: new outputs map themselves */
static int redraw = 0;  /* an output appeared or was resized */
//static Imlib_Image current_imlib_image = NULL;

static size_t path_join(char *dest, size_t max, const char *p1, const char *p2, const char *p3) {
    size_t l1 = p1 ? strlen(p1) : 0;
    size_t l2 = p2 ? strlen(p2) : 0;
    size_t l3 = p3 ? strlen(p3) : 0;
    size_t total = l1 + (p2 ? 1 : 0) + l2 + (p3 ? 1 : 0) + l3;

    if (total >= max) return 0;

    char *ptr = dest;
    if (l1) { memcpy(ptr, p1, l1); ptr += l1; }
    if (p2) { *ptr++ = '/'; memcpy(ptr, p2, l2); ptr += l2; }
    if (p3) { *ptr++ = '/'; memcpy(ptr, p3, l3); ptr += l3; }
    *ptr = '\0';

    return total;
}

static void out_geometry(void *d, struct wl_output *wo, int32_t x, int32_t y,
                         int32_t pw, int32_t ph, int32_t sub,
                         const char *make, const char *model, int32_t tr) {
    (void)d; (void)wo; (void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)make; (void)model; (void)tr;
}

static void out_mode(void *d, struct wl_output *wo, uint32_t flags,
                     int32_t w, int32_t h, int32_t refresh) {
    (void)wo; (void)refresh;
    Output *o = d;
    if (flags & WL_OUTPUT_MODE_CURRENT) { o->mw = w; o->mh = h; }
}

static const struct wl_output_listener out_listener = {
    .geometry = out_geometry,
    .mode     = out_mode,
};

static void ls_configure(void *d, struct zwlr_layer_surface_v1 *ls,
                         uint32_t serial, uint32_t w, uint32_t h) {
    Output *o = d;
    int nw = w ? (int)w : o->mw;
    int nh = h ? (int)h : o->mh;
    if (nw != o->w || nh != o->h) redraw = 1; /* new output or real resize */
    o->w = nw;
    o->h = nh;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
}

static void ls_closed(void *d, struct zwlr_layer_surface_v1 *ls) {
    Output *o = d;
    zwlr_layer_surface_v1_destroy(ls);
    wl_surface_destroy(o->surf);
    o->ls = NULL;
    o->surf = NULL;
}

static const struct zwlr_layer_surface_v1_listener ls_listener = {
    .configure = ls_configure,
    .closed    = ls_closed,
};

static void output_map(Output *o) {
    o->surf = wl_compositor_create_surface(compositor);
    o->ls = zwlr_layer_shell_v1_get_layer_surface(layer_shell, o->surf, o->wl_out,
                ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "wallman");
    zwlr_layer_surface_v1_set_anchor(o->ls,
                ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(o->ls, -1);
    zwlr_layer_surface_v1_add_listener(o->ls, &ls_listener, o);
    wl_surface_commit(o->surf);
}

static void reg_global(void *d, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t ver) {
    (void)d; (void)ver;
    if (strcmp(iface, wl_compositor_interface.name) == 0)
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 1);
    else if (strcmp(iface, wl_shm_interface.name) == 0)
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0)
        layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
    else if (strcmp(iface, wl_output_interface.name) == 0) {
        Output *o = calloc(1, sizeof(*o));
        if (!o) return;
        o->name = name;
        o->wl_out = wl_registry_bind(reg, name, &wl_output_interface, 1);
        wl_output_add_listener(o->wl_out, &out_listener, o);
        o->next = outputs;
        outputs = o;
        if (ready) output_map(o); /* hot-added output (VT switch, replug) */
    }
}

static void reg_global_remove(void *d, struct wl_registry *reg, uint32_t name) {
    (void)d; (void)reg;
    for (Output **p = &outputs; *p; p = &(*p)->next) {
        Output *o = *p;
        if (o->name != name) continue;
        if (o->ls) zwlr_layer_surface_v1_destroy(o->ls);
        if (o->surf) wl_surface_destroy(o->surf);
        wl_output_destroy(o->wl_out);
        *p = o->next;
        free(o);
        return;
    }
}

static const struct wl_registry_listener reg_listener = {
    .global        = reg_global,
    .global_remove = reg_global_remove,
};

/* Connect to the compositor, bind globals, and map a background layer surface per output */
void wall_setup_renderer(void) {
    if (wl_dpy) return;
    wl_dpy = wl_display_connect(NULL);
    if (!wl_dpy) exit(1);

    struct wl_registry *reg = wl_display_get_registry(wl_dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(wl_dpy); /* globals */
    if (!compositor || !shm || !layer_shell) exit(1);
    wl_display_roundtrip(wl_dpy); /* output modes */

    ready = 1;
    for (Output *o = outputs; o; o = o->next) output_map(o);
    wl_display_roundtrip(wl_dpy); /* configure -> sizes */

//    imlib_set_cache_size(64 * 1024 * 1024);
    imlib_set_cache_size(0);
}

static void buf_release(void *d, struct wl_buffer *buf) {
    (void)d;
    wl_buffer_destroy(buf);
}

static const struct wl_buffer_listener buf_listener = { .release = buf_release };

/* Copy a w*h XRGB frame into a fresh shm buffer and show it; the buffer frees itself on release */
static void output_present(Output *o, const void *pixels) {
    size_t size = (size_t)o->w * (size_t)o->h * 4;
    int fd = memfd_create("wallman", MFD_CLOEXEC);
    if (fd < 0) return;
    if (ftruncate(fd, (off_t)size) != 0) { close(fd); return; }

    void *map = mmap(NULL, size, PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return; }
    memcpy(map, pixels, size);
    munmap(map, size);

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, o->w, o->h, o->w * 4,
                                                      WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    wl_buffer_add_listener(buf, &buf_listener, NULL);
    wl_surface_attach(o->surf, buf, 0, 0);
    wl_surface_damage(o->surf, 0, 0, o->w, o->h);
    wl_surface_commit(o->surf);
}

/*static void prefetch_neighbors(void) {
    if (count < 2) return;
    int neighbors[2] = { (cur - 1 + count) % count, (cur + 1) % count };

    for (int i = 0; i < 2; i++) {
        char path[PATH_MAX];
        if (path_join(path, sizeof(path), master_dir, current_folder, files[neighbors[i]])) {
            Imlib_Image img = imlib_load_image(path);
            if (img) {
                imlib_context_set_image(img);
                imlib_free_image(); // Remains in Imlib2's 32MB cache
            }
        }
    }
}*/



const char *get_save_path(void) {
    static char path[PATH_MAX];
    if (path[0]) return path;
    const char *home = getenv("HOME");
    if (!home) return NULL;

    size_t hlen = strlen(home);
    const char *suffix = "/etc/wallman";
    size_t slen = strlen(suffix);
    if (hlen + slen >= PATH_MAX) return NULL;

    memcpy(path, home, hlen);
    memcpy(path + hlen, suffix, slen + 1); /* +1 copies \0 */
    return path;
}

static void free_files(void) {
    if (!files) return;
    for (int i = 0; i < count; i++) free(files[i]);
    free(files);
    files = NULL;
    count = 0;
}

static void scan_and_fill(void) {
    if (walldb_open(get_save_path()) == 0)
        count = walldb_list(current_folder, &files);
}

static void wall_init(void) {
    if (files) return;
    char want[PATH_MAX] = "";
    const char *save = get_save_path();
    if (master_dir[0] == '\0' && save) {
        FILE *f = fopen(save, "r");
        if (f) {
            if (fgets(master_dir, sizeof(master_dir), f))
                master_dir[strcspn(master_dir, "\n")] = 0;
            if (fgets(current_folder, sizeof(current_folder), f))
                current_folder[strcspn(current_folder, "\n")] = 0;
            if (fgets(want, sizeof(want), f))
                want[strcspn(want, "\n")] = 0;
            fclose(f);
        }
    }

    scan_and_fill();
    if (count == 0 && strcmp(current_folder, "*") != 0) { /* stale or emptied view: show everything */
        memcpy(current_folder, "*", 2);
        scan_and_fill();
    }
    for (int i = 0; i < count; i++)
        if (strcmp(files[i], want) == 0) { cur = i; break; }
}

static char shown[PATH_MAX]; /* what's on screen: the target of CLI edits, even after a reload moves cur */

static void run_setter(const char *filename) {
    size_t sl = strlen(filename);
    if (filename != shown && sl < sizeof(shown)) memcpy(shown, filename, sl + 1);
    if (!wl_dpy) {
        fprintf(stderr, "Error: Wayland display not initialized\n");
        return;
    }
    /* apply pending configure/closed events and free released buffers */
    if (wl_display_roundtrip(wl_dpy) < 0) {
        fprintf(stderr, "Error: Wayland connection lost\n");
        return;
    }
    redraw = 0;

    char fullpath[PATH_MAX];
    if (!path_join(fullpath, sizeof(fullpath), master_dir, filename, NULL))
        return;

    Imlib_Image buffer = imlib_load_image(fullpath);
    if (!buffer) {
        fprintf(stderr, "Error: Imlib2 failed to load %s\n", fullpath);
        return;
    }

    // Get Image dimensions
    imlib_context_set_image(buffer);
    int imgW = imlib_image_get_width();
    int imgH = imlib_image_get_height();
    imlib_context_set_blend(1);

    // One canvas per output, one layer surface per output
    for (Output *o = outputs; o; o = o->next) {
        if (!o->surf || o->w <= 0 || o->h <= 0) continue;

        Imlib_Image rootimg = imlib_create_image(o->w, o->h);
        if (!rootimg) continue;

        imlib_context_set_image(rootimg);
        imlib_context_set_color(0, 0, 0, 255);
        imlib_image_fill_rectangle(0, 0, o->w, o->h);

        double aspect = (double)o->w / imgW;
        if ((int)(imgH * aspect) < o->h)
            aspect = (double)o->h / imgH;

        int scaledW = (int)(imgW * aspect);
        int scaledH = (int)(imgH * aspect);
        int left = (o->w - scaledW) / 2;
        int top  = (o->h - scaledH) / 2;

        imlib_blend_image_onto_image(buffer, 0, 0, 0, imgW, imgH,
                                     left, top, scaledW, scaledH);

        output_present(o, imlib_image_get_data_for_reading_only());
        imlib_free_image_and_decache();
    }
    wl_display_flush(wl_dpy);

    // Cleanup Imlib2 Memory (Client-side)
    imlib_context_set_image(buffer);
    imlib_free_image_and_decache();
}

int wall_wl_fd(void) {
    return wl_dpy ? wl_display_get_fd(wl_dpy) : -1;
}

/* Called when wall_wl_fd() is readable: handle output hotplug/resize. -1 = compositor gone */
int wall_wl_dispatch(void) {
    if (wl_display_dispatch(wl_dpy) < 0) {
        fprintf(stderr, "Error: Wayland connection lost\n");
        return -1;
    }
    if (redraw && shown[0]) run_setter(shown);            /* what is on screen, which set may have changed */
    wl_display_flush(wl_dpy);
    return 0;
}

void wall_reload(const void *arg) {
    (void)arg;

    char *current_name = (files && count > 0) ? strdup(files[cur]) : NULL;
    free_files();
    wall_init();
    if (current_name) {
        for (int i = 0; i < count; i++) {
            if (strcmp(files[i], current_name) == 0) { cur = i; break; }
        }
        free(current_name);
    }
    if (cur >= count) cur = 0;
}

void wall_cycle(const void *arg) {
    wall_init();
    if (count == 0) return;
    if (arg) {
        const Arg *a = (const Arg *)arg;
        cur = (cur + a->i + count) % count;
    }
    run_setter(files[cur]);
}

void wall_restore(void) {
    wall_init();
    if (count > 0) run_setter(files[cur]);
}

void wall_save(const void *arg) {
    (void)arg;

    const char *save = get_save_path();
    if (count == 0 || master_dir[0] == '\0' || !save) return;

    char temp_path[PATH_MAX];
    size_t slen = strlen(save);
    if (slen + 5 >= PATH_MAX) return;
    memcpy(temp_path, save, slen);
    memcpy(temp_path + slen, ".tmp", 5);

    FILE *f = fopen(temp_path, "w");
    if (!f) return;

    if (fprintf(f, "%s\n%s\n%s\n", master_dir, current_folder, files[cur]) < 0) goto cleanup;
    if (fflush(f) != 0) goto cleanup;

    int fd = fileno(f);
    if (fd == -1 || fsync(fd) != 0) goto cleanup;
    if (fclose(f) != 0) { unlink(temp_path); return; }
    f = NULL;

    if (rename(temp_path, save) != 0) { unlink(temp_path); return; }

    char dirbuf[PATH_MAX];
    memcpy(dirbuf, save, slen + 1);
    char *dir = dirname(dirbuf);
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    return;

cleanup:
    if (f) fclose(f);
    unlink(temp_path);
}

void wall_random(const void *arg) {
    (void)arg;

    wall_init();
    if (count <= 1) return;
    cur = rand() % count;
    run_setter(files[cur]);
}

static void view_apply(void) {
    free_files();
    cur = 0;
    wall_init();
    if (count > 0) run_setter(files[cur]);
}

void wall_folder_select(const void *arg) {
    if (!arg || master_dir[0] == '\0') return;
    if (walldb_next_view(current_folder, ((const Arg *)arg)->i,
                         current_folder, sizeof(current_folder)) == 0)
        view_apply();
}

void wall_view(const char *name) {
    size_t n = name ? strlen(name) : 0;
    if (n == 0 || n >= sizeof(current_folder)) return;
    memcpy(current_folder, name, n + 1);
    view_apply();
}

static void purity_apply(void) {
    wall_reload(NULL);                      /* an emptied view falls back to "*" in wall_init */
    if (count > 0) run_setter(files[cur]);  /* the current one may have just been filtered out */
}

void wall_purity(const void *arg) {
    if (walldb_set_purity(((const Arg *)arg)->i) == 0) purity_apply();
}

void wall_purity_step(const void *arg) {
    if (walldb_set_purity(walldb_get_purity() + ((const Arg *)arg)->i) == 0) purity_apply();
}

const char *wall_current(void) {
    return shown[0] ? shown : NULL;
}

/* Make a library file (path relative to the master dir) the current wallpaper. Stays in the
 * current view when it's there, otherwise tries the inbox, then everything. */
void wall_jump(const char *path) {
    if (!path || !path[0]) return;
    char keep[sizeof(current_folder)];
    memcpy(keep, current_folder, sizeof(keep));

    const char *tries[] = { keep, "?", "*" };
    for (int t = 0; t < 3; t++) {
        size_t n = strlen(tries[t]);
        memcpy(current_folder, tries[t], n + 1);
        free_files();
        wall_init();
        for (int i = 0; i < count; i++)
            if (strcmp(files[i], path) == 0) { cur = i; run_setter(files[cur]); return; }
    }
    /* in no view (e.g. rated above the allowed purity): draw it and leave the lists as they were */
    memcpy(current_folder, keep, sizeof(keep));
    free_files();
    wall_init();
    run_setter(path);
}

/* Draw a file under the master dir without touching the lists */
void wall_show(const char *path) {
    if (path && path[0]) run_setter(path);
}

