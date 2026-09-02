# Mount an export

## Serve a directory

```bash
raild --serve /models --port 18600 --backend rdma
```

## Mount it

`mount.railfuse` is a read-write FUSE mount that speaks the wire protocol
directly, and is roughly 2x quicker than the NFS route. `mount` drives it, or
run it yourself:

```bash
mount -t railfuse <peer>:/ /mnt -o port=18600,rdma
mount.railfuse <peer> /mnt --port 18600 --backend rdma   # or <peer>:/
ls /mnt
fusermount3 -u /mnt
```

`-o` takes the kernel mount's names in bytes - `port`, `conns`, `readahead`,
`fetch`, `actimeo`, `rdma`, `noverify` - and passes the rest to libfuse. The
long flags are rail's, in mebibytes.

`chown` answers `EPERM`; everything else a mount normally needs works.

Every page is hashed on both ends and checked on arrival. `noverify`, or
`--no-checksum`, turns that off for a mount that trusts the fabric's own CRC.
Nothing then catches a corrupt page, so leave it on unless a read is worth more
than the guarantee.

```bash
mount -t railfuse <peer>:/ /mnt -o port=18600,rdma,noverify
```

## Mount it from the kernel

`railfs.ko` is the same protocol without FUSE in the path. It is faster than the
FUSE mount on every axis measured and it is what the numbers below call the
kernel mount.

```bash
sudo insmod build/src/linux/railfs.ko
sudo mount -i -t railfs -o host=<peer>,export=/,port=18600,rdma,uid=$(id -u),gid=$(id -g) none /mnt
sudo umount /mnt && sudo rmmod railfs
```

`none` is there because `mount` wants a source: given only a target it looks the
rest up in `/etc/fstab` and reports the mountpoint missing from it. `export=`
has to name what the daemon serves, and without `uid`/`gid` every inode belongs
to root and nothing you own can be written.

`-i` keeps `mount` from handing off to `/sbin/mount.railfs`. Install that helper
and the spec becomes a `HOST:EXPORT` pair, which is what `mount(8)` and
`/etc/fstab` expect:

```bash
sudo cp build/tools/mount.railfs/mount.railfs /sbin/   # or: make install PREFIX=/usr

sudo mount -t railfs <peer>:/ /mnt -o rdma,uid=$(id -u),gid=$(id -g)
sudo mount -t railfs <peer>:/ /mnt -o rdma,port=18600,conns=64,fetch=524288
```

The helper folds the spec into the option string and puts it last, so an
`-o host=` cannot send the mount somewhere the spec did not name. In `fstab`:

```
<peer>:/ /mnt railfs rdma,conns=32,uid=1000,gid=1000,noauto 0 0
```

The workqueue every fetch and flush runs on belongs to the module rather than
to a mount, so its depth is a module parameter and can be changed on a live
mount:

```bash
sudo insmod build/src/linux/railfs.ko inflight=128
echo 96 | sudo tee /sys/module/railfs/parameters/inflight
```

| option | default | |
| --- | --- | --- |
| `host=` `export=` | none | where the export is; the helper sets both from the spec |
| `port=` | `18515` | what the daemon listens on |
| `rdma` | off | fabric rather than tcp |
| `noverify` | off | trust the fabric's crc, stop hashing pages |
| `uid=` `gid=` | root | who everything belongs to; the wire carries no ownership, so without these nothing else can write |
| `actimeo=` | `30` | seconds a name or a stat may be believed |
| `conns=` | `32` | connections in the pool |
| `fetch=` | `1048576` | bytes a readahead window is cut into |
| `readahead=` | `268435456` | bytes the kernel reads ahead |
| `block=` | `262144` | bytes a short fetch is widened to |
| `flush_span=` | `4` | connections one file's writeback spreads over |
| `flush_limit=` | `16` | flushes in flight across the mount |

The five tuning values are powers of two and are refused rather than clamped, so
`/proc/mounts` reports what the mount is actually running. `fetch=` cannot
exceed the page the session negotiated.

`/sys/kernel/debug/railfs/stats` reports where a request's time went, per phase,
in nanoseconds. Writing to it resets the counters.

## Export over NFS

```bash
sudo cp build/tools/mount.railnfs/{mount,umount}.railnfs /sbin/

sudo mount -t railnfs <peer> /mnt -o backend=rdma
sudo mount -t railnfs <peer>:/ /mnt -o backend=rdma,port=18600,sessions=8
sudo umount /mnt
```

`port=`, `nfsport=`, `backend=`, `sessions=` and `readahead=` are the helper's;
everything else in `-o` goes to `mount.nfs`. In `fstab`:

```
<peer>:/ /mnt railnfs backend=rdma,port=18600,noauto 0 0
```

The server is a mode of the same binary, so by hand it is:

```bash
mount.railnfs --serve <peer> --nfs-port 2049 --port 18600 --backend rdma
sudo mount -t nfs -o vers=3,tcp,port=2049,mountport=2049,nolock,soft 127.0.0.1:/ /mnt
```

`WRITE`, `COMMIT`, `CREATE`, `SETATTR` and `REMOVE` reach the peer; directories
and links still answer `NFS3ERR_ROFS`. Mount `soft` - a hard mount against a
server that stops answering wedges everything that touches the directory.
