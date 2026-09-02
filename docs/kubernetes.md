# Run it on Kubernetes

`charts/raild` runs the daemon as a DaemonSet on the nodes that hold the data.
Hand the image to containerd; `docker build` leaves it where kubelet cannot
read it:

```bash
docker build -f docker/Dockerfile -t raild:dev .
docker save raild:dev | sudo ctr -n k8s.io images import -
```

That import repeats on every serving node, and after every rebuild. Label the
nodes that export, then install:

```bash
kubectl label node NODE rail.io/export=true
helm install raild charts/raild -n rail --create-namespace \
  --set export.hostPath=/models
```

`export.hostPath` is required, so a release that omits it fails rather than
serving the wrong tree. `helm show values` lists the rest.

The pod takes `hostNetwork`, so clients reach the node's fabric address, not a
Service IP:

```bash
kubectl -n rail get pods -o wide
sudo mount -i -t railfs -o host=<peer>,export=/,port=18600,rdma,uid=$(id -u),gid=$(id -g) none /mnt
```

`seccompProfile: Unconfined` is load-bearing: the default profile blocks the
`io_uring` syscalls the data path is built on, and every bulk transfer hangs.
`privileged` is for `/dev/infiniband` and the memory-registration limit.

## Mount it from a pod

`charts/railfs-csi` is a CSI node driver, so the export arrives as an ordinary
PVC. Its init container runs `modprobe railfs`, so install the module first.

```bash
docker build -f docker/Dockerfile.csi -t railfs-csi:dev .
docker save railfs-csi:dev | sudo ctr -n k8s.io images import -

helm install railfs-csi charts/railfs-csi -n rail --create-namespace \
  --set volume.create=true \
  --set volume.attributes.host=<peer> \
  --set volume.attributes.export=/
```

`volume.create` writes the PV and its claim. Only `host` and `export` are
required; the other mount options pass through unchanged.

```bash
kubectl get csinodes -o custom-columns='NODE:.metadata.name,DRIVERS:.spec.drivers[*].name'
```

A pod takes it like any other claim:

```yaml
  volumes:
    - name: models
      persistentVolumeClaim:
        claimName: railfs
```

```bash
kubectl -n rail exec POD -- sh -c 'grep /models /proc/mounts; ls /models'
```

Volumes are static: a PV names its export and the claim binds to it. Nothing
carves a subdirectory per claim.

## Serve a model from the mount

`charts/vllm` runs vLLM under NVIDIA Dynamo with its weights on a railfs mount
instead of local disk. It creates the PV and claim itself, so it needs the CSI
driver above and a Dynamo install to deploy into.

```bash
helm install vllm charts/vllm -n dynamo-system \
  --set railfs.attributes.host=<peer> \
  --set railfs.attributes.export=/ \
  --set model.name=Qwen3.8-27B-FP8 \
  --set model.path=/models/Qwen3.8-27B-FP8

kubectl -n dynamo-system exec deploy/vllm-frontend -- \
  curl -s localhost:8000/v1/models

helm uninstall vllm -n dynamo-system --wait
```
