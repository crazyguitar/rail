// SPDX-License-Identifier: GPL-2.0

#include "helper.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int pidfile(const char *Target, char *Out, size_t Room)
{
	char Folded[PATH_MAX];
	char *Real = realpath(Target, NULL);
	size_t I;

	if (snprintf(Folded, sizeof(Folded), "%s", Real ? Real : Target) >= (int)sizeof(Folded)) {
		free(Real);
		return -1;
	}
	free(Real);

	for (I = 0; Folded[I]; I++) {
		if (Folded[I] == '/') {
			Folded[I] = '_';
		}
	}

	return snprintf(Out, Room, "%s/%s.pid", RAILNFS_RUN_DIR, Folded) < (int)Room ? 0 : -1;
}

int remember_daemon(const char *Target, pid_t Pid)
{
	char Path[PATH_MAX];
	FILE *Out;

	if (mkdir(RAILNFS_RUN_DIR, 0755) != 0 && access(RAILNFS_RUN_DIR, W_OK) != 0) {
		return -1;
	}
	if (pidfile(Target, Path, sizeof(Path)) != 0) {
		return -1;
	}

	Out = fopen(Path, "w");
	if (!Out) {
		return -1;
	}

	fprintf(Out, "%ld\n", (long)Pid);
	return fclose(Out) == 0 ? 0 : -1;
}

pid_t recall_daemon(const char *Target)
{
	char Path[PATH_MAX];
	long Pid = 0;
	FILE *In;

	if (pidfile(Target, Path, sizeof(Path)) != 0) {
		return -1;
	}

	In = fopen(Path, "r");
	if (!In) {
		return -1;
	}

	if (fscanf(In, "%ld", &Pid) != 1 || Pid <= 0) {
		Pid = -1;
	}
	fclose(In);
	return (pid_t)Pid;
}

int forget_daemon(const char *Target)
{
	char Path[PATH_MAX];

	if (pidfile(Target, Path, sizeof(Path)) != 0) {
		return -1;
	}
	return unlink(Path);
}
