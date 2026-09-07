# Railfs

Railfs mounts a remote directory the way nfs or cifs does, and the client comes
in the same two pieces those do:

- `mount.railfs`, the userspace helper `mount(8)` [finds][helper-path] and
  [execs][helper-exec] out of `/sbin` when it is asked for `-t railfs`. It
  resolves the host, folds `HOST:EXPORT` into the one option string the kernel
  parses, and calls `mount(2)`.
- `railfs.ko`, the kernel module. It registers the filesystem type, builds a
  superblock per mount, and answers the VFS - lookups, readdirs, pages - from
  `raild` on the other end.

The documents here work through that design: where railfs sits in the VFS, what
each operation costs on the wire, and why the module is shaped the way it is.

[helper-path]: https://github.com/util-linux/util-linux/blob/c08bd2aa50aa306c6ad51e50f582acf686283deb/libmount/src/context.c#L2363-L2364
[helper-exec]: https://github.com/util-linux/util-linux/blob/c08bd2aa50aa306c6ad51e50f582acf686283deb/libmount/src/context_mount.c#L521

* [Virtual Filesystem](vfs.md)
* [Page Cache](pagecache.md)
* [Transport](transport.md)
