# rail

A filesystem over RDMA. It implements the VFS interface on top of the fabric,
so a remote directory is read and written as a local path — by the kernel, by
FUSE, over NFS, or as a Kubernetes volume. It also carries files directly, with
rsync's delta algorithm over an `ssh` control channel.

Backends are `tcp` and `rdma`. `rdma` is one-sided: the receiver publishes
where a page should land and the sender writes straight into it.

For what it measures, see [docs/benchmark.md](docs/benchmark.md).
