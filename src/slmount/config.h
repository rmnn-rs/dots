#define MAX_DEVS 32
#define MAPDIR   "/dev/mapper/"                                  /* LUKS maps are MAPDIR"slm-<name>" */
#define MFLAGS   (MS_NOSUID | MS_NODEV)                          /* mount flags for everything */

/* whole disks to hide ('a' shows them): kernel name, or better the stable
 * ID_SERIAL_SHORT / ID_PATH since sda/sdc reshuffle between boots:
 *   udevadm info -q property /dev/sda | grep -E 'ID_SERIAL_SHORT|ID_PATH' */
static const char *hide[]  = { "TPBF2303310010306819", NULL };
static const char *ownfs[] = { "vfat", "exfat", "ntfs", NULL }; /* no unix perms: mount as our uid/gid */

/* keys: a char or an ncurses KEY_*, the action, and its entry in the help
 * lines (NULL = alias, not listed). The prompt boxes keep their own fixed
 * editing keys (Enter, Esc, Backspace, ^U). */
static const Key keys[] = {
    /* key        function      help */
    { 'j',        seldown,      "J K Nav" },
    { KEY_DOWN,   seldown,      NULL },
    { 'k',        selup,        NULL },
    { KEY_UP,     selup,        NULL },
    { '\n',       mountsel,     "Enter mount/unlock" },
    { 'l',        mountsel,     NULL },
    { 'c',        mountcustom,  "c Custom Path" },
    { 'i',        do_image,     "i Disk Image" },
    { 'r',        togglero,     "r Read-only" },
    { 'u',        umountsel,    "u Umount/Lock" },
    { 'h',        umountsel,    NULL },
    { 'U',        umountall,    "U All" },
    { 'e',        ejectsel,     "e Eject" },
    { 'a',        toggleall,    "a Internal drives" },
    { 'q',        quit,         "q Quit" },
    { 'Q',        quit,         NULL },
};

