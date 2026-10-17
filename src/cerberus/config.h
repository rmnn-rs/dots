/* apps run by autostart: argv (NULL terminated), restart delay in seconds.
 * delay -1 = run once, delay >= 0 = restart that many seconds after it exits.
 * The table ends with { {NULL}, 0 }. */
static const AppConfig apps[] = {
    /* command                                               | delay (-1 = once) */
    { {"/usr/bin/xset", "b", "off", NULL},                      -1 },
    { {"xset", "r", "rate", "200", "50", NULL},                 -1 },
    { {"picom", NULL},                                           5 },
//  { {"bar", NULL},                                             2 },
    { {"slstatus", NULL},                                        2 },
    { {"dunst", NULL},                                           5 },
    { {"unclutter", "--timeout", "5", NULL},                     3 },
    { {"wallman", "-D", NULL},                                   2 },
    { {"bat-notify", NULL},                                      3 },
    { {"dbus-update-activation-environment", "--all", NULL},    -1 },
    { {NULL},                                                    0 }
};

