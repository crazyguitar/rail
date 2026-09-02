# Put the control channel on the fabric

Every read costs one control round trip, so the mount is only as quick as the
interface that address belongs to. RoCE ports often carry no IP at all, which
quietly leaves control on wifi - worth 5.8x here.

```bash
ip -brief addr show          # a 200 GbE port UP with no address?

sudo nmcli con add type ethernet ifname enP2p1s0f1np1 con-name roce \
  ipv4.method manual ipv4.addresses <host>/24 ipv6.method disabled
sudo nmcli con up roce

ping <peer>               # want sub-millisecond, not 5 ms
```

Use a profile: `ip addr add` does not survive NetworkManager. The new address
appends GID entries and leaves index 0 alone, which is the entry the fabric
backend uses; `show_gids` confirms it.
