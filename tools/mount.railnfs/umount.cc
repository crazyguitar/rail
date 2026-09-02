// SPDX-License-Identifier: GPL-2.0
//
// umount(8) execs `/sbin/umount.railnfs DIR [-flnrv]`. Unmounting is only half
// of it: the railnfs mount.railnfs started has nothing else to end it.

#include "helper.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define RAILNFS_STOP_TRIES 100
#define RAILNFS_STOP_PAUSE_US 50000

// mount(8) and umount(8) scrub a helper's environment, so PATH is no help.
static const char *find_umount(void)
{
	static const char *const Wherever[] = { "/bin/umount", "/usr/bin/umount", "/sbin/umount", "/usr/sbin/umount" };
	size_t I;

	for (I = 0; I < sizeof(Wherever) / sizeof(Wherever[0]); I++) {
		if (access(Wherever[I], X_OK) == 0) {
			return Wherever[I];
		}
	}
	return NULL;
}

static int run_umount(const char *Target, int Lazy, int Force)
{
	const char *Binary = find_umount();
	pid_t Child;
	int Status = 0;

	if (!Binary) {
		return -1;
	}

	Child = fork();
	if (Child < 0) {
		return -1;
	}

	if (Child == 0) {
		if (Lazy) {
			execl(Binary, "umount", "-l", "-i", Target, (char *)NULL);
		} else if (Force) {
			execl(Binary, "umount", "-f", "-i", Target, (char *)NULL);
		} else {
			execl(Binary, "umount", "-i", Target, (char *)NULL);
		}
		_exit(127);
	}

	if (waitpid(Child, &Status, 0) < 0) {
		return -1;
	}
	return WIFEXITED(Status) && WEXITSTATUS(Status) == 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
	const char *Target;
	pid_t Daemon;
	int Lazy = 0;
	int Force = 0;
	int Opt;

	if (argc < 2) {
		fprintf(stderr, "usage: umount.railnfs MOUNTPOINT [-flnrv]\n");
		return 1;
	}

	Target = argv[1];

	optind = 2;
	while ((Opt = getopt(argc, argv, "flnrv")) != -1) {
		switch (Opt) {
		case 'l':
			Lazy = 1;
			break;
		case 'f':
			Force = 1;
			break;
		default:
			break;
		}
	}

	if (run_umount(Target, Lazy, Force) != 0) {
		fprintf(stderr, "umount.railnfs: unmounting %s failed\n", Target);
		return 1;
	}

	// The daemon goes only after the mount is gone. Killing it first leaves
	// the kernel client talking to nothing, which with a soft mount is an
	// EIO for whoever is still in the directory.
	Daemon = recall_daemon(Target);
	if (Daemon <= 0) {
		return 0;
	}

	kill(Daemon, SIGTERM);
	for (int Try = 0; Try < RAILNFS_STOP_TRIES && kill(Daemon, 0) == 0; Try++) {
		usleep(RAILNFS_STOP_PAUSE_US);
	}
	if (kill(Daemon, 0) == 0) {
		kill(Daemon, SIGKILL);
	}

	forget_daemon(Target);
	return 0;
}
