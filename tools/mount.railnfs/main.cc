// SPDX-License-Identifier: GPL-2.0
//
// mount(8) execs `/sbin/mount.railnfs SPEC DIR [-sfnv] [-o OPTIONS]` when it is
// asked for `-t railnfs`. Unlike mount.railfs there is no kernel module here: the
// helper starts an railnfs on a loopback port, waits for it to answer, and hands
// the mount to mount.nfs. umount.railnfs takes the daemon back down.

#include "helper.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RAILNFS_START_TRIES 200
#define RAILNFS_START_PAUSE_MS 50
#define RAILNFS_WATCH_PAUSE_MS 2000

static void usage(void)
{
	railnfsUsage();
}

// The daemon's own options are taken out of the -o string; what is left is for
// mount.nfs, so a caller can still say noatime or rsize without this knowing
// what either means.
struct daemon_options {
	const char *Port;
	const char *NfsPort;
	const char *Backend;
	const char *Sessions;
	const char *Threads;
	int NoVerify;
	const char *Readahead;
};

static int take(const char *One, const char *Name, const char **Out)
{
	const size_t Len = strlen(Name);

	if (strncmp(One, Name, Len) != 0 || One[Len] != '=')
		return 0;

	*Out = One + Len + 1;
	return 1;
}

static int split_options(const char *Options, char *Rest, size_t Room, struct daemon_options *Mine)
{
	char Copy[4096];
	char *Save = NULL;
	char *One;
	size_t At = 0;

	Rest[0] = '\0';

	if (snprintf(Copy, sizeof(Copy), "%s", Options) >= (int)sizeof(Copy)) {
		fprintf(stderr, "mount.railnfs: option string too long\n");
		return -1;
	}

	for (One = strtok_r(Copy, ",", &Save); One; One = strtok_r(NULL, ",", &Save)) {
		if (take(One, "port", &Mine->Port) || take(One, "nfsport", &Mine->NfsPort) ||
		    take(One, "backend", &Mine->Backend) || take(One, "sessions", &Mine->Sessions) || take(One, "threads", &Mine->Threads) ||
		    take(One, "readahead", &Mine->Readahead))
			continue;

		if (strcmp(One, "noverify") == 0) {
			Mine->NoVerify = 1;
			continue;
		}

		if (At + strlen(One) + 2 > Room) {
			fprintf(stderr, "mount.railnfs: no room for option %s\n", One);
			return -1;
		}

		At += (size_t)snprintf(Rest + At, Room - At, "%s%s", At ? "," : "", One);
	}

	return 0;
}

// A port the kernel picked and then let go of. Two mounts on one machine each
// need their own daemon, so the default cannot be 2049.
static int free_port(char *Out, size_t Room)
{
	struct sockaddr_in Addr;
	socklen_t Len = sizeof(Addr);
	int Fd = socket(AF_INET, SOCK_STREAM, 0);

	if (Fd < 0)
		return -1;

	memset(&Addr, 0, sizeof(Addr));
	Addr.sin_family = AF_INET;
	Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	Addr.sin_port = 0;

	if (bind(Fd, (struct sockaddr *)&Addr, sizeof(Addr)) != 0 || getsockname(Fd, (struct sockaddr *)&Addr, &Len) != 0) {
		close(Fd);
		return -1;
	}

	snprintf(Out, Room, "%u", (unsigned)ntohs(Addr.sin_port));
	close(Fd);
	return 0;
}

static int answering(const char *Port)
{
	struct sockaddr_in Addr;
	int Fd = socket(AF_INET, SOCK_STREAM, 0);
	int Ok;

	if (Fd < 0)
		return 0;

	memset(&Addr, 0, sizeof(Addr));
	Addr.sin_family = AF_INET;
	Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	Addr.sin_port = htons((uint16_t)strtoul(Port, NULL, 10));

	Ok = connect(Fd, (struct sockaddr *)&Addr, sizeof(Addr)) == 0;
	close(Fd);
	return Ok;
}

static void nap(long Ms)
{
	struct timespec Wait = { Ms / 1000, (Ms % 1000) * 1000000 };

	nanosleep(&Wait, NULL);
}

// setsid so the daemon is not in the mount's process group, and stdio to the
// log because anything it writes to an inherited terminal outlives the mount.
static pid_t start_daemon(const char *Spec, const struct daemon_options *Mine, const char *NfsPort)
{
	const char *Argv[20];
	size_t At = 0;
	pid_t Child = fork();
	int Log;

	if (Child != 0)
		return Child;

	if (setsid() == (pid_t)-1)
		_exit(1);

	Log = open(RAILNFS_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (Log >= 0) {
		dup2(Log, STDOUT_FILENO);
		dup2(Log, STDERR_FILENO);
		if (Log > STDERR_FILENO)
			close(Log);
	}

	Argv[At++] = "mount.railnfs";
	Argv[At++] = "--serve";
	Argv[At++] = Spec;
	Argv[At++] = "--nfs-port";
	Argv[At++] = NfsPort;
	if (Mine->Port) {
		Argv[At++] = "--port";
		Argv[At++] = Mine->Port;
	}
	if (Mine->Backend) {
		Argv[At++] = "--backend";
		Argv[At++] = Mine->Backend;
	}
	if (Mine->Sessions) {
		Argv[At++] = "--sessions";
		Argv[At++] = Mine->Sessions;
	}
	if (Mine->Threads) {
		Argv[At++] = "--threads";
		Argv[At++] = Mine->Threads;
	}
	if (Mine->NoVerify) {
		Argv[At++] = "--no-checksum";
	}
	if (Mine->Readahead) {
		Argv[At++] = "--readahead";
		Argv[At++] = Mine->Readahead;
	}
	Argv[At] = NULL;

	execv("/proc/self/exe", (char *const *)Argv);
	_exit(127);
}

static const char *find_tool(const char *Name, const char *const *Wherever, size_t Count)
{
	static char Path[PATH_MAX];
	size_t I;

	for (I = 0; I < Count; I++) {
		snprintf(Path, sizeof(Path), "%s%s", Wherever[I], Name);
		if (access(Path, X_OK) == 0) {
			return Path;
		}
	}
	return NULL;
}

// The kernel records this mount as type nfs, so umount(8) dispatches to
// umount.nfs and never to umount.railnfs. Nothing else would end the daemon, so a
// watcher outlives the helper and takes it down when the mount goes away.
static int mounted(const char *Target)
{
	struct stat Here;
	struct stat Up;
	char Parent[PATH_MAX];

	if (snprintf(Parent, sizeof(Parent), "%s/..", Target) >= (int)sizeof(Parent)) {
		return 0;
	}
	if (stat(Target, &Here) != 0 || stat(Parent, &Up) != 0) {
		return 0;
	}
	return Here.st_dev != Up.st_dev;
}

static void watch_mount(const char *Target, pid_t Daemon)
{
	if (fork() != 0) {
		return;
	}

	if (setsid() == (pid_t)-1) {
		_exit(1);
	}

	for (;;) {
		nap(RAILNFS_WATCH_PAUSE_MS);
		if (kill(Daemon, 0) != 0) {
			break;
		}
		if (!mounted(Target)) {
			kill(Daemon, SIGTERM);
			break;
		}
	}

	forget_daemon(Target);
	_exit(0);
}

static int run_mount_nfs(const char *Target, const char *NfsPort, const char *Rest)
{
	static const char *const Wherever[] = { "/sbin/", "/usr/sbin/", "/bin/", "/usr/bin/" };
	const char *Binary = find_tool("mount.nfs", Wherever, sizeof(Wherever) / sizeof(Wherever[0]));
	char Options[4096];
	pid_t Child;
	int Status = 0;

	if (!Binary) {
		fprintf(stderr, "mount.railnfs: no mount.nfs found; install nfs-common\n");
		return -1;
	}

	if (snprintf(Options, sizeof(Options), "vers=3,proto=tcp,port=%s,mountport=%s,mountvers=3,nolock,soft,nconnect=8%s%s", NfsPort, NfsPort,
		     *Rest ? "," : "", Rest) >= (int)sizeof(Options)) {
		fprintf(stderr, "mount.railnfs: options too long\n");
		return -1;
	}

	Child = fork();
	if (Child < 0)
		return -1;

	if (Child == 0) {
		execl(Binary, "mount.nfs", "127.0.0.1:/", Target, "-o", Options, (char *)NULL);
		_exit(127);
	}

	if (waitpid(Child, &Status, 0) < 0)
		return -1;

	return WIFEXITED(Status) && WEXITSTATUS(Status) == 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
	struct daemon_options Mine = {};
	const char *Options = "";
	char Chosen[16];
	char Rest[4096];
	const char *Target;
	const char *Spec;
	const char *NfsPort;
	pid_t Daemon;
	int Fake = 0;
	int Opt;

	if (argc >= 2 && (strcmp(argv[1], "--serve") == 0 || strcmp(argv[1], "probe") == 0 || strcmp(argv[1], "--version") == 0)) {
		return railnfsServe(argc - (strcmp(argv[1], "--serve") == 0 ? 1 : 0), argv + (strcmp(argv[1], "--serve") == 0 ? 1 : 0));
	}

	if (argc < 3) {
		usage();
		return 1;
	}

	Spec = argv[1];
	Target = argv[2];

	optind = 3;
	while ((Opt = getopt(argc, argv, "sfnvo:")) != -1) {
		switch (Opt) {
		case 'f':
			Fake = 1;
			break;
		case 'o':
			Options = optarg;
			break;
		case 's':
		case 'n':
		case 'v':
			break;
		default:
			usage();
			return 1;
		}
	}

	if (split_options(Options, Rest, sizeof(Rest), &Mine) != 0)
		return 1;

	if (Mine.NfsPort) {
		NfsPort = Mine.NfsPort;
	} else {
		if (free_port(Chosen, sizeof(Chosen)) != 0) {
			fprintf(stderr, "mount.railnfs: could not find a free port\n");
			return 1;
		}
		NfsPort = Chosen;
	}

	if (Fake)
		return 0;

	Daemon = start_daemon(Spec, &Mine, NfsPort);
	if (Daemon < 0) {
		fprintf(stderr, "mount.railnfs: could not start railnfs\n");
		return 1;
	}

	for (int Try = 0; Try < RAILNFS_START_TRIES && !answering(NfsPort); Try++) {
		nap(RAILNFS_START_PAUSE_MS);
	}

	if (!answering(NfsPort)) {
		kill(Daemon, SIGTERM);
		fprintf(stderr, "mount.railnfs: railnfs did not answer on port %s; see %s\n", NfsPort, RAILNFS_LOG);
		return 1;
	}

	// The pid is written before the mount so a mount that fails still leaves
	// something for umount.railnfs, and cleaned up here when it does fail.
	if (remember_daemon(Target, Daemon) != 0) {
		kill(Daemon, SIGTERM);
		fprintf(stderr, "mount.railnfs: could not record the daemon's pid\n");
		return 1;
	}

	if (run_mount_nfs(Target, NfsPort, Rest) != 0) {
		kill(Daemon, SIGTERM);
		forget_daemon(Target);
		fprintf(stderr, "mount.railnfs: mounting %s on %s failed\n", Spec, Target);
		return 1;
	}

	watch_mount(Target, Daemon);
	return 0;
}
