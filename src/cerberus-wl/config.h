/* apps run by autostart: argv (NULL terminated), restart delay in seconds.
 * delay -1 = run once, delay >= 0 = restart that many seconds after it exits.
 * The table ends with { {NULL}, 0 }. */
static const AppConfig apps[] = {
    /* command                                               | delay (-1 = once) */
//    { { "/bin/sh", "-c", "slstatus -s | dbus-run-session dwl",   NULL }, -1 },
    { {"waywall", "--daemon",                                    NULL},   2 },
    { {"/bin/sh", "-c", "sls -s > \"$DWL_STATUS_FIFO\"",         NULL},   3 },
    { {"dunst",                                                  NULL},   5 },
    { {NULL},                                                             0 }
};

