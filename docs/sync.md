# Push files

```bash
rail -r /data peer:/backup --backend rdma
rail -aP --stats /data peer:/backup --backend rdma   # progress and a summary
rail --help                                          # every flag
```

The binary is both ends: it runs `ssh HOST rail --server --receive PATH`, so
name the peer's copy with `--rail-path` unless it is on `PATH`.
