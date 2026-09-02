/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RAILNFS_HELPER_H
#define RAILNFS_HELPER_H

#include <sys/types.h>

#define RAILNFS_LOG "/var/log/railnfs.log"
#define RAILNFS_RUN_DIR "/run/railnfs"

/* A mountpoint's daemon, so whoever unmounts can take down what mount.railnfs
 * put up. The path is the key, with its slashes folded so it can be a filename.
 */
int remember_daemon(const char *Target, pid_t Pid);
pid_t recall_daemon(const char *Target);
int forget_daemon(const char *Target);

/* The export server, which is a mode of this same binary rather than a program
 * of its own - so starting it is a re-exec of /proc/self/exe and never a search
 * of a PATH that mount(8) has already scrubbed.
 */
int railnfsServe(int Argc, char **Argv);
void railnfsUsage(void);

#endif
