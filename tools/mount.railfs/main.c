// SPDX-License-Identifier: GPL-2.0
//
// mount(8) execs `/sbin/mount.railfs SPEC DIR [-sfnv] [-o OPTIONS]` when it is
// asked for `-t railfs`, the same contract mount.cifs is built to. The helper
// exists so the spec can name a host and an export while the kernel sees only
// the option string it understands.

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(void)
{
	fprintf(stderr, "usage: mount.railfs HOST:EXPORT MOUNTPOINT [-sfnv] [-o OPTIONS]\n");
}

// mount.cifs takes //host/share; this takes host:export, the spelling rail
// already uses on its command line.
static int split_spec(char *Spec, char **Host, char **Export)
{
	char *Colon = strchr(Spec, ':');

	if (!Colon || Colon == Spec || Colon[1] == '\0')
		return -1;

	*Colon = '\0';
	*Host = Spec;
	*Export = Colon + 1;
	return 0;
}

// The kernel takes a dotted quad and has no resolver, so a name is turned into
// an address here, where getaddrinfo exists.
static int resolve_host(const char *Host, char *Into, size_t Room)
{
	struct addrinfo Hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
	struct addrinfo *Found = NULL;
	struct sockaddr_in *Addr;
	struct in_addr Dotted;
	int Err;
	int Ret = -1;

	if (inet_pton(AF_INET, Host, &Dotted) == 1) {
		if (snprintf(Into, Room, "%s", Host) < (int)Room) {
			Ret = 0;
		}

		goto out;
	}

	Err = getaddrinfo(Host, NULL, &Hints, &Found);
	if (Err != 0 || !Found) {
		fprintf(stderr, "mount.railfs: cannot resolve %s: %s\n", Host, gai_strerror(Err));
		goto out;
	}

	Addr = (struct sockaddr_in *)Found->ai_addr;
	if (inet_ntop(AF_INET, &Addr->sin_addr, Into, (socklen_t)Room)) {
		Ret = 0;
	}

out:
	if (Found) {
		freeaddrinfo(Found);
	}

	return Ret;
}

// mount(8) hands generic options through in the same -o string as the
// filesystem's own. The kernel takes the two apart: flags go in the mount(2)
// argument, and only what is left is a filesystem option. Passing "ro" through
// as one would make the mount fail on an option railfs has never heard of.
// Generic mount options become MS_* flags and everything else is passed to the
// kernel as filesystem options. Each entry says which bits it sets and which it
// clears, because these come in pairs that undo one another: "ro,rw" has to end
// up read-write, and a table that only ever ORed could never take a bit back.
//
// Returns 0, or -1 when an option would be dropped rather than honoured - a
// mount that silently ignores what it was asked for is worse than one that
// refuses.
static int split_flags(const char *Options, char *Rest, size_t Room, unsigned long *Out)
{
	static const struct {
		const char *Name;
		unsigned long Set;
		unsigned long Clear;
	} Generic[] = {
		{ "ro", MS_RDONLY, 0 },
		{ "rw", 0, MS_RDONLY },
		{ "nosuid", MS_NOSUID, 0 },
		{ "suid", 0, MS_NOSUID },
		{ "nodev", MS_NODEV, 0 },
		{ "dev", 0, MS_NODEV },
		{ "noexec", MS_NOEXEC, 0 },
		{ "exec", 0, MS_NOEXEC },
		{ "sync", MS_SYNCHRONOUS, 0 },
		{ "async", 0, MS_SYNCHRONOUS },
		{ "noatime", MS_NOATIME, 0 },
		{ "atime", 0, MS_NOATIME },
		{ "nodiratime", MS_NODIRATIME, 0 },
		{ "diratime", 0, MS_NODIRATIME },
		{ "relatime", MS_RELATIME, 0 },
		{ "norelatime", 0, MS_RELATIME },
	};

	unsigned long Flags = 0;
	char Copy[4096];
	char *Save = NULL;
	char *One;
	size_t At = 0;

	Rest[0] = '\0';
	*Out = 0;

	if (snprintf(Copy, sizeof(Copy), "%s", Options) >= (int)sizeof(Copy)) {
		fprintf(stderr, "mount.railfs: option string too long\n");
		return -1;
	}

	for (One = strtok_r(Copy, ",", &Save); One; One = strtok_r(NULL, ",", &Save)) {
		size_t I;
		int Known = 0;

		for (I = 0; I < sizeof(Generic) / sizeof(Generic[0]); I++) {
			if (strcmp(One, Generic[I].Name) != 0)
				continue;
			Flags = (Flags & ~Generic[I].Clear) | Generic[I].Set;
			Known = 1;
			break;
		}

		if (Known)
			continue;

		if (At + strlen(One) + 2 > Room) {
			fprintf(stderr, "mount.railfs: no room for option %s\n", One);
			return -1;
		}

		At += snprintf(Rest + At, Room - At, "%s%s", At ? "," : "", One);
	}

	*Out = Flags;
	return 0;
}

int main(int argc, char **argv)
{
	const char *Options = "";
	char *Host = NULL;
	char *Export = NULL;
	char Data[4096];
	char Rest[4096];
	char Address[INET_ADDRSTRLEN];
	struct stat Where;
	unsigned long Flags;
	int Fake = 0;
	int Opt;

	if (argc < 3) {
		usage();
		return 1;
	}

	// Split a copy: the spec is still needed whole, as the device string the
	// kernel records and as the name any error has to quote back.
	char Spec[4096];
	const char *Target = argv[2];

	if (snprintf(Spec, sizeof(Spec), "%s", argv[1]) >= (int)sizeof(Spec)) {
		fprintf(stderr, "mount.railfs: spec too long\n");
		return 1;
	}

	// mount(8) passes these; -s and -v are accepted and ignored, -f is a dry
	// run, and -n means do not update /etc/mtab, which this helper never does.
	optind = 3;
	while ((Opt = getopt(argc, argv, "sfnvo:")) != -1) {
		switch (Opt) {
		case 'o':
			Options = optarg;
			break;
		case 'f':
			Fake = 1;
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

	if (split_spec(Spec, &Host, &Export) != 0) {
		fprintf(stderr, "mount.railfs: expected HOST:EXPORT, got %s\n", argv[1]);
		return 1;
	}

	if (split_flags(Options, Rest, sizeof(Rest), &Flags) != 0)
		return 1;

	if (resolve_host(Host, Address, sizeof(Address)) != 0) {
		// A colon left in the export means the spec was split inside an
		// address rather than after the host. Said here and not at the split,
		// where a path that legitimately holds one looks the same.
		if (strchr(Export, ':')) {
			fprintf(stderr, "mount.railfs: %s looks like an IPv6 address, which railfs does not speak\n", argv[1]);
		}

		return 1;
	}

	// mount(2) answers ENOENT for a missing spec and a missing target alike,
	// so the target is checked here to say which one is wrong.
	if (stat(Target, &Where) != 0) {
		fprintf(stderr, "mount.railfs: %s: %s\n", Target, strerror(errno));
		return 1;
	}

	if (!S_ISDIR(Where.st_mode)) {
		fprintf(stderr, "mount.railfs: %s is not a directory\n", Target);
		return 1;
	}

	// The kernel parses one string, so the spec is folded into it rather than
	// given a second channel the module would have to learn about. Host and
	// export come last because the parser keeps the last value it is given,
	// and the spec is what the helper exists to translate: -o host=... must
	// not be able to send the mount somewhere else.
	if (snprintf(Data, sizeof(Data), "%s%shost=%s,export=%s", Rest, *Rest ? "," : "", Address, Export) >= (int)sizeof(Data)) {
		fprintf(stderr, "mount.railfs: options too long\n");
		return 1;
	}

	if (Fake)
		return 0;

	if (mount(argv[1], Target, "railfs", Flags, Data) != 0) {
		fprintf(stderr, "mount.railfs: mounting %s on %s failed: %s\n", argv[1], Target, strerror(errno));

		// The module says which option it refused, and only to the log.
		if (errno == EINVAL) {
			fprintf(stderr, "mount.railfs: the reason is in the kernel log: dmesg | tail\n");
		}

		return 1;
	}
	return 0;
}
