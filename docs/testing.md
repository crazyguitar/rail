# Test

```bash
salloc -N2 scripts/run-e2e.sh                  # every suite, about twenty minutes
salloc -N2 make test FILTER='*/rdma'           # one transport
scripts/run-e2e.sh --local --peer 10.0.0.2     # without slurm
scripts/run-e2e.sh --skip-mount                # skip the suites that mount
```

Always through the launcher: it clears the mount and module a killed run leaves
behind, which otherwise fails the next run for an unrelated reason.

## What a run needs

Two machines. The suite runs on one and asks the other to serve, running the
peer's `raild` by the path it found its own binary at, so both need the same
tree at the same path. A peer that is behind fails as a refused connection.

| | |
| --- | --- |
| `RAIL_PEER` | the far host, as both ssh and the data channel reach it; required, no default |
| `RAIL_DIR` | working directory on both sides, default `/tmp/rail-e2e` |
| `RAIL_KERNEL_TESTS` | ask for the privileged suites; without it they are left out |

## Root

Mounting, loading a module and reading the kernel log need it. Asking for those
suites without it stops the run rather than skipping — a skipped suite reads
exactly like one that passed:

```
rail-e2e: RAIL_KERNEL_TESTS is set, but this is not running as root.
```

Grant `sudo` for the one binary, not for everything:

```bash
sudo tee /etc/sudoers.d/rail-e2e >/dev/null <<'RULE'
%rail-test ALL=(root) NOPASSWD: SETENV: /home/you/rail/build/tests/e2e/rail-e2e
%rail-test ALL=(root) NOPASSWD: /usr/bin/umount -l /tmp/rail-e2e/kernel-mnt, /usr/sbin/rmmod railfs, /usr/bin/rm -rf /tmp/rail-e2e
RULE
sudo chmod 0440 /etc/sudoers.d/rail-e2e
sudo visudo -c
```

`SETENV:` carries `RAIL_PEER` through; the second line is the launcher's
cleanup. Keep the path exact, and run `visudo -c` before logging out.

## In Kubernetes

Same binary, same suite; only the launcher differs.

```bash
# Kubernetes reads containerd's k8s.io namespace, not docker's own store.
docker build -f docker/Dockerfile.test -t rail-test:dev .
docker save rail-test:dev | sudo ctr -n k8s.io images import -

kubectl create namespace rail-test
kubectl -n rail-test create secret generic rail-e2e-ssh \
  --from-file=id_ed25519=$HOME/.ssh/id_ed25519 \
  --from-file=known_hosts=$HOME/.ssh/known_hosts

helm install rail-e2e charts/test-e2e -n rail-test \
  --set peer=10.0.0.2 --set ssh.user=you --set node=<node>

kubectl -n rail-test logs -f job/rail-e2e
```

| value | |
| --- | --- |
| `peer` | required: the address that serves |
| `ssh.user` | required: the container is root, the peer is not |
| `node` | pin it, or the scheduler can land the pod on the peer and the suite drives itself |
| `filter` | which cases, `*` by default |
| `kernel` | mount and load the module, `true` by default |

`filter` picks the backend, the same string `make test FILTER=` takes:

| | |
| --- | --- |
| the kernel mount | `Kernel.*:MountHelper.*:ExportRoot.*` |
| the fuse mount | `Fuse.*:Backends/Mount.*` |
| the nfs gateway | `Backends/Nfs.*` |
| one transport | append `/rdma` or `/tcp` |

`kernel=false` drops the privilege, the module and the debugfs mount, which is
how the protocol cases prove they never quietly wanted root.

A Job is immutable, so a second run is `helm uninstall rail-e2e -n rail-test`
and install again.

## The CSI driver

Its tests mount for real, so they take the peer from the environment and skip
without it. Five of the nine need neither peer nor root:

```bash
cd csi && sudo RAILFS_CSI_HOST=<peer> RAILFS_CSI_EXPORT=models go test ./...
```

`RAILFS_CSI_EXPORT` is relative to what the daemon serves; `RAILFS_CSI_PORT` and
`RAILFS_CSI_TCP` override the rest. Where there is no go:

```bash
docker run --rm -v $PWD:/w -w /w -e CGO_ENABLED=0 golang:1.23 go test -c -o /w/csi.test ./...
sudo RAILFS_CSI_HOST=<peer> RAILFS_CSI_EXPORT=models ./csi.test
```
