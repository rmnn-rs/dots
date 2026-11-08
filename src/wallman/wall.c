#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <fcntl.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xrandr.h>
#include <Imlib2.h>
#include "wall.h"
#include "walldb.h"

int count = 0;
int cur = 0;
char current_folder[256] = "";
char master_dir[1024] = "";
static char **files = NULL;

/* Internal Renderer State */
static Display *display = NULL;
static int screen;
static int rr_event;    /* RandR's first event number, 0 when the server has no RandR */
static Pixmap last_pixmap = None;

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

/* Initialize X11 and Imlib once */
void wall_setup_renderer(void) {
    if (display) return;
    display = XOpenDisplay(NULL);
    if (!display) { fputs("Error: cannot open the X display\n", stderr); exit(1); }
    screen = DefaultScreen(display);

    int rr_error;   /* tell us when monitors change, see wall_x_dispatch() */
    if (XRRQueryExtension(display, &rr_event, &rr_error))
        XRRSelectInput(display, RootWindow(display, screen), RRScreenChangeNotifyMask);

    imlib_context_set_display(display);
    imlib_context_set_visual(DefaultVisual(display, screen));
    imlib_context_set_colormap(DefaultColormap(display, screen));
    imlib_set_cache_size(0);
}

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

/* walls in the view, -1 when the database could not be read (busy writer, missing file) */
static int scan_and_fill(void) {
    int n = walldb_open(get_save_path()) == 0 ? walldb_list(current_folder, &files) : -1;
    count = n > 0 ? n : 0;
    return n;
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

    if (scan_and_fill() == 0 && strcmp(current_folder, "*") != 0) { /* stale or emptied view: show everything */
        memcpy(current_folder, "*", 2);
        scan_and_fill();
    }
    for (int i = 0; i < count; i++)
        if (strcmp(files[i], want) == 0) { cur = i; break; }
}

static char shown[PATH_MAX]; /* what's on screen: the target of CLI edits, even after a reload moves cur */

static void run_setter(const char *filename) {
    if (!display) {
        fprintf(stderr, "Error: X11 Display not initialized\n");
        return;
    }

    char fullpath[PATH_MAX];
    if (!path_join(fullpath, sizeof(fullpath), master_dir, filename, NULL))
        return;

    Imlib_Image buffer = imlib_load_image(fullpath);
    if (!buffer) {
        fprintf(stderr, "Error: Imlib2 failed to load %s\n", fullpath);
        return;
    }
    if (filename != shown) memcpy(shown, filename, strlen(filename) + 1);   /* fits: path_join checked master_dir/filename */

    // Get Image and Screen dimensions. DisplayWidth/Height are fixed when we connect and go stale as
    // soon as xrandr resizes the screen (monitors beyond the old size got nothing), so ask the root now.
    imlib_context_set_image(buffer);
    int imgW = imlib_image_get_width();
    int imgH = imlib_image_get_height();
    Window root = RootWindow(display, screen);
    XWindowAttributes wa = {0};
    XGetWindowAttributes(display, root, &wa);
    int sw = wa.width;
    int sh = wa.height;

    // Create the canvas for the root window
    Imlib_Image rootimg = imlib_create_image(sw, sh);
    if (!rootimg) {
        imlib_context_set_image(buffer);
        imlib_free_image_and_decache();
        return;
    }

    imlib_context_set_image(rootimg);
    imlib_context_set_color(0, 0, 0, 255);
    imlib_image_fill_rectangle(0, 0, sw, sh);
    imlib_context_set_dither(1);
    imlib_context_set_blend(1);

    // Handle Multi-Monitor via Xinerama
    int noutputs;
    XineramaScreenInfo *outputs = XineramaQueryScreens(display, &noutputs);
    XineramaScreenInfo fake = { .x_org = 0, .y_org = 0, .width = sw, .height = sh };
    if (!outputs) { outputs = &fake; noutputs = 1; }

    for (int i = 0; i < noutputs; i++) {
        double aspect = (double)outputs[i].width / imgW;
        if ((int)(imgH * aspect) < outputs[i].height)
            aspect = (double)outputs[i].height / imgH;

        int scaledW = (int)(imgW * aspect);
        int scaledH = (int)(imgH * aspect);
        int left = (outputs[i].width  - scaledW) / 2;
        int top  = (outputs[i].height - scaledH) / 2;

        imlib_context_set_image(rootimg);
        /* the cover-scaled image is bigger than its monitor: keep the overflow off the neighbours */
        imlib_context_set_cliprect(outputs[i].x_org, outputs[i].y_org, outputs[i].width, outputs[i].height);
        imlib_blend_image_onto_image(buffer, 0, 0, 0, imgW, imgH,
                                     outputs[i].x_org + left,
                                     outputs[i].y_org + top,
                                     scaledW, scaledH);
    }
    imlib_context_set_cliprect(0, 0, 0, 0);

    // Create the X11 Pixmap
    Pixmap new_pixmap = XCreatePixmap(display, root, sw, sh, DefaultDepth(display, screen));
    imlib_context_set_image(rootimg);
    imlib_context_set_drawable(new_pixmap);
    imlib_render_image_on_drawable(0, 0);

    XGrabServer(display);

    // Set the background
    XSetWindowBackgroundPixmap(display, root, new_pixmap);
    XClearWindow(display, root);

    // Set properties so compositors/terminals see the background
    Atom a_root  = XInternAtom(display, "_XROOTPMAP_ID",    False);
    Atom a_eroot = XInternAtom(display, "ESETROOT_PMAP_ID", False);
    XChangeProperty(display, root, a_root,  XA_PIXMAP, 32, PropModeReplace,
                    (unsigned char *)&new_pixmap, 1);
    XChangeProperty(display, root, a_eroot, XA_PIXMAP, 32, PropModeReplace,
                    (unsigned char *)&new_pixmap, 1);

    if (last_pixmap != None) {
        XFreePixmap(display, last_pixmap);
    }
    last_pixmap = new_pixmap;

    XUngrabServer(display);
    XFlush(display);

    // Cleanup Imlib2 Memory (Client-side)
    imlib_context_set_image(rootimg);
    imlib_free_image_and_decache();
    imlib_context_set_image(buffer);
    imlib_free_image_and_decache();

    if (outputs != &fake)
        XFree(outputs);
}

int wall_x_fd(void) {
    return display ? ConnectionNumber(display) : -1;
}

/* Call before waiting on wall_x_fd() (Xlib may already hold events it read during a round trip) and
 * whenever it is readable. A monitor that was plugged, unplugged or rearranged (RandR) redraws the
 * wall on screen for the new layout. */
void wall_x_dispatch(void) {
    int changed = 0;
    while (display && XPending(display)) {
        XEvent ev;
        XNextEvent(display, &ev);
        if (rr_event && ev.type == rr_event + RRScreenChangeNotify) changed = 1;
    }
    if (changed && shown[0]) run_setter(shown);
}

void wall_reload(const void *arg) {
    (void)arg;

    char *current_name = (files && count > 0) ? strdup(files[cur]) : NULL;
    int gone = current_name != NULL;        /* until we find the wall on screen again */
    free_files();
    wall_init();
    if (current_name) {
        for (int i = 0; i < count; i++) {
            if (strcmp(files[i], current_name) == 0) { cur = i; gone = 0; break; }
        }
        free(current_name);
    }
    if (cur >= count) cur = 0;
    if (gone && count > 0) run_setter(files[cur]);   /* it left the view (rated, recategorized, deleted): show what took its place */
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
    cur = (cur + 1 + rand() % (count - 1)) % count;     /* never the one already showing */
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
    wall_reload(NULL);                      /* redraws if the current one was just filtered out; an emptied view falls back to "*" in wall_init */
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

