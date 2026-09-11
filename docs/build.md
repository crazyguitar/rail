# Build

```bash
sudo apt-get install -y liburing-dev libssh-dev libibverbs-dev
make                 # or: cmake -S . -B build && cmake --build build
sudo make install    # rail, raild, railfs, mount.railfs, mount.railnfs into PREFIX
```

Needs a C++23 compiler.

One build produces everything, the kernel module included: it lands at
`build/src/linux/railfs.ko`, and needs the headers for the running kernel
(`linux-headers-$(uname -r)`). A machine without them builds the rest and says
so. Secure Boot needs the module signed - `cmake --build build --target
railfs-mok` makes a key for this machine, `railfs-sign` stamps the module, and
`scripts/enroll-mok.sh` walks through enrolling it.

The module builds on kernels 5.15 through 6.17; `src/linux/railfs-compat.h`
carries the differences. Below 6.12 the page cache cannot be told the largest
folio one fetch can fill, so the module keeps to single-page folios there and
`minfolio=` is ignored. 5.15, 6.1, 6.8, 6.11 and 6.14 are compile-checked;
6.17 is what the suite runs on.
The kernel and mount tests also pass on 5.15.0-1074-oracle with Mellanox OFED.

RDMA selects the fastest active ports first, breaking ties by device name and
port number on both peers. This keeps slower management adapters from taking
the two rails on machines with a separate, faster RDMA fabric.

With an out-of-tree Mellanox OFED driver, the module also needs that driver's
headers and symbol versions. A module built only against the stock kernel
headers can compile but fail to load with `ib_core` symbol-version errors:

```bash
OFED_DIR=/usr/src/ofa_kernel/$(uname -m)/$(uname -r)
make -C /lib/modules/$(uname -r)/build M="$PWD/src/linux" \
  KBUILD_EXTRA_SYMBOLS="$OFED_DIR/Module.symvers" \
  NOSTDINC_FLAGS="-I$OFED_DIR/include -include $OFED_DIR/include/linux/compat-2.6.h" modules
```

Kernels without `KBUILD_EXTMOD_OUTPUT` build the module in
`src/linux/railfs.ko`; set `RAIL_KO` to that absolute path when running the
kernel tests.

`insmod build/src/linux/railfs.ko` loads it from the build tree. Installing it
puts it where `modprobe` looks, which the Kubernetes mount needs:

```bash
sudo make modules-install
sudo modprobe railfs
```

Nothing loads it at boot; `/etc/modules-load.d/railfs.conf` does.

## Packages

Every tag builds two Debian packages for amd64 and arm64 and attaches them to
the release, with a tarball of the same binaries:

```bash
sudo apt install ./rail_0.1.0-1_arm64.deb ./railfs-dkms_0.1.0-1_all.deb
```

`rail` holds the daemon, the command and the mount helpers. `railfs-dkms`
holds the module source and DKMS builds it for the running kernel, and again
after a kernel upgrade; it needs `linux-headers-$(uname -r)`. Locally,
`dpkg-buildpackage -us -uc -b` produces the same packages.

The `rail` package ships a `raild` unit, installed but not enabled. Set the
export in `/etc/default/raild`, then:

```bash
sudo systemctl enable --now raild
```
