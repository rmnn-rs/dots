/* wallman configuration. Edit, then rebuild: sudo make clean install && make clean */

static const char master_dir_cfg[] = "/home/pc/wl/";

static const char db_name_cfg[] = "wallman.db";

static const char *const sandbox_read[] = {
    "/usr", "/lib", "/lib64", "/lib32", "/etc", "/proc", "/sys", "/dev", "/nix", "/opt",
};

