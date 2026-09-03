#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'TEXT'
usage: run-e2e.sh [--local] [--skip-mount] [--filter GLOB] [--partition NAME] [--peer ADDRESS] [--repo PATH]

  --local          run here instead of through slurm; needs --peer
  --skip-mount     leave out the suites that mount and load the module
  --filter GLOB    gtest filter, default '*'
  --partition      slurm partition, default the cluster default
  --peer ADDRESS   the serving node's address, default the second in the allocation
  --repo PATH      the tree to run, default the directory holding this script

The mounting suites need root on the node that runs them.
TEXT
  exit 2
}

Local=0
SkipMount=0
Filter='*'
Partition=""
Peer=""
Repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

parse_arguments() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --local) Local=1; shift ;;
      --skip-mount) SkipMount=1; shift ;;
      --filter) Filter="${2:?}"; shift 2 ;;
      --partition) Partition="${2:?}"; shift 2 ;;
      --peer) Peer="${2:?}"; shift 2 ;;
      --repo) Repo="${2:?}"; shift 2 ;;
      -h|--help) usage ;;
      *) printf 'unknown argument: %s\n' "$1" >&2; usage ;;
    esac
  done
}

check_prerequisites() {
  if [ "$Local" = "0" ]; then
    command -v srun >/dev/null || { echo 'no srun on PATH; is this a slurm login node? --local runs without one' >&2; exit 1; }
  elif [ -z "$Peer" ]; then
    echo '--local has no allocation to name the peer; pass --peer ADDRESS' >&2
    exit 1
  fi
  test -x "$Repo/build/tests/e2e/rail-e2e" || { printf 'no suite at %s/build; build it first\n' "$Repo" >&2; exit 1; }
}

# Rank 0 runs the suite; rank 1 only holds the second node until rank 0 is done.
# Slurm's node names resolve nowhere else, so the peer is its NodeAddr.
OnNode=$(cat <<'INNER'
set -e
Nodes=$(scontrol show hostnames "$SLURM_JOB_NODELIST")
Second=$(echo "$Nodes" | sed -n 2p)
Serving=$(scontrol show node "$Second" | grep -oE 'NodeAddr=[^ ]+' | cut -d= -f2)
[ -z "$Serving" ] && Serving="$Second"
[ -n "$PEER_OVERRIDE" ] && Serving="$PEER_OVERRIDE"

Marker="/tmp/rail-e2e-running-$SLURM_JOB_ID"
if [ "$SLURM_PROCID" != "0" ]; then
  while [ -e "$Marker" ]; do sleep 5; done
  exit 0
fi
touch "$Marker"
trap 'rm -f "$Marker"' EXIT

export RAIL_PEER="$Serving"
printf 'suite on %s, serving from %s\n' "$(hostname)" "$Serving"
cd "$REPO"

if [ "$SKIP_MOUNT" = "1" ]; then
  exec ./build/tests/e2e/rail-e2e --gtest_filter="$FILTER"
fi

export RAIL_KERNEL_TESTS=1
sudo -n umount -l /tmp/rail-e2e/kernel-mnt 2>/dev/null || true
sudo -n rmmod railfs 2>/dev/null || true
sudo -n rm -rf /tmp/rail-e2e 2>/dev/null || true
exec sudo -E ./build/tests/e2e/rail-e2e --gtest_filter="$FILTER"
INNER
)

run_here() {
  exec env SLURM_PROCID=0 SLURM_JOB_ID="local-$$" REPO="$Repo" FILTER="$Filter" \
    SKIP_MOUNT="$SkipMount" PEER_OVERRIDE="$Peer" bash -c "$OnNode"
}

run_through_slurm() {
  local args=(--nodes=2 --ntasks=2 --ntasks-per-node=1)
  [ -n "$Partition" ] && args+=(--partition="$Partition")
  exec srun "${args[@]}" \
    --export=ALL,REPO="$Repo",FILTER="$Filter",SKIP_MOUNT="$SkipMount",PEER_OVERRIDE="$Peer" \
    bash -c "$OnNode"
}

parse_arguments "$@"
check_prerequisites
if [ "$Local" = "1" ]; then run_here; fi
run_through_slurm
