#ifndef WALL_H
#define WALL_H


typedef union {
    int i;
    unsigned int ui;
    float f;
    const void *v;
} Arg;

extern char master_dir[1024];

extern char current_folder[256];
extern int count, cur;

const char *get_db_path(void);
void wall_settings_init(void);

void wall_cycle(const void *arg);
void wall_restore(void);
void wall_random(const void *arg);
void wall_select(const void *arg);
void wall_reload(const void *arg);
void wall_folder_select(const void *arg);
void wall_apply(void);
void wall_setup_renderer(void);
int wall_wl_fd(void);
int wall_wl_dispatch(void);

void wall_prepare(void);

void wall_purity(const void *arg);
void wall_purity_step(const void *arg);
void wall_view(const char *name);
const char *wall_current(void);

void wall_jump(const char *path);
void wall_show(const char *path);

#endif

