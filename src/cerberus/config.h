
/* apps run by autostart: argv (NULL terminated), restart delay in seconds. */
static const AppConfig apps[] = {
    { .command = {"/usr/bin/xset", "b", "off", NULL},                      .restart_delay = -1 },
    { .command = {"xset", "r", "rate", "200", "50", NULL},                 .restart_delay = -1 },
    { .command = {"picom", NULL},                                           .restart_delay = 5 },
    { .command = {"slstatus", NULL},                                        .restart_delay = 2 },
    { .command = {"dunst", NULL},                                           .restart_delay = 5 },
    { .command = {"unclutter", "--timeout", "5", NULL},                     .restart_delay = 3 },
    { .command = {"wallman", "-D", NULL},                                   .restart_delay = 2 },
    { .command = {"bat-notify", NULL},                                      .restart_delay = 3 },
    { .command = {"dbus-update-activation-environment", "--all", NULL},    .restart_delay = -1 },
    { .command = {NULL},                                                    .restart_delay = 0 }
};

