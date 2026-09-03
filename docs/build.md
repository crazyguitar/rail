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
