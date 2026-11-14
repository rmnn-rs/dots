#ifndef WALLWATCH_H
#define WALLWATCH_H

#include <sys/types.h>

/*
 * Keeps the database in step with the master dir while the daemon runs: new files are scanned in
 * (with the remembered tier mappings), deleted ones pruned, moved ones renamed so their tags and
 * ratings stay. After each burst of changes it calls notify() so the daemon reloads.
 *
 * master is the master dir (config.h), db_path the database inside it; the watcher ignores the
 * database's own files there.
 *
 * Call this BEFORE the sandbox is applied. The watcher is a separate, deliberately unsandboxed
 * process (it only ever handles file names, never image data) so the daemon itself stays read-only
 * with its syscall list unchanged. It exits when the daemon does. Returns its pid, or -1.
 */
pid_t wallwatch_start(const char *master, const char *db_path, void (*notify)(void));

#endif

