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
