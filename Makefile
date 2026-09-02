BUILD ?= build
PREFIX ?= /usr/local
TYPE ?= Release
JOBS ?= $(shell nproc 2>/dev/null || echo 4)
FILTER ?= *
PEER ?= $(RAIL_PEER)
KVER ?= $(shell uname -r)
MODDIR ?= /lib/modules/$(KVER)/extra
MODULE ?= $(firstword $(wildcard $(BUILD)/src/linux/railfs.ko src/linux/railfs.ko))

.PHONY: all build install modules-install modules-uninstall test bench format clean

all: build

build: $(BUILD)/CMakeCache.txt
	cmake --build $(BUILD) -j$(JOBS)
	@test -z "$(MODULE)" || test ! -f $(MODDIR)/railfs.ko || cmp -s $(MODULE) $(MODDIR)/railfs.ko || \
	  printf 'warning: %s is older than this build; mount -t railfs loads it, not %s.\n         run: sudo make modules-install\n' '$(MODDIR)/railfs.ko' '$(MODULE)'

$(BUILD)/CMakeCache.txt:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=$(TYPE)

install: build
	cmake --install $(BUILD) --prefix $(PREFIX)
	@test -x /sbin/mount.railfs || \
	  printf 'warning: mount(8) execs /sbin/mount.<type>, and this installed under %s.\n         the HOST:EXPORT spec form needs: sudo make install PREFIX=/usr\n' '$(PREFIX)'

modules-install: build
	@test -n "$(MODULE)" || { echo 'no railfs.ko built; needs linux-headers-$(KVER)'; exit 1; }
	install -D -m 644 $(MODULE) $(MODDIR)/railfs.ko
	depmod -a $(KVER)

modules-uninstall:
	rm -f $(MODDIR)/railfs.ko
	depmod -a $(KVER)

# Through the launcher, never by hand: it clears the mount and the module a
# killed run leaves behind, and names the peer. E2E passes it flags, so
# make test E2E='--local --peer 10.0.0.2' runs without an allocation.
test: build
	scripts/run-e2e.sh --filter '$(FILTER)' $(E2E)

# Through the launcher, for the same reasons as test, plus one: it asks slurm
# for the nodes exclusively, and a benchmark sharing a machine measures that.
bench: build
	scripts/run-bench.sh --filter '$(FILTER)' $(BENCH)

format:
	clang-format -i $$(git ls-files '*.cc' '*.h' | grep -v '^src/linux/')

clean:
	rm -rf $(BUILD)
