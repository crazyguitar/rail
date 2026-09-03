package main

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"sort"
	"strings"

	"github.com/container-storage-interface/spec/lib/go/csi"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
)

type node struct {
	csi.UnimplementedNodeServer
	id string
}

var valuedKeys = map[string]bool{
	"port":        true,
	"conns":       true,
	"fetch":       true,
	"readahead":   true,
	"block":       true,
	"actimeo":     true,
	"uid":         true,
	"gid":         true,
	"flush_span":  true,
	"flush_limit": true,
}

var flagKeys = map[string]bool{
	"rdma":     true,
	"noverify": true,
}

func mountOptions(attrs map[string]string, capability *csi.VolumeCapability, readOnly bool) (string, error) {
	if err := requireHostAndExport(attrs); err != nil {
		return "", err
	}
	if err := refuseCommas(attrs); err != nil {
		return "", err
	}

	opts := []string{"host=" + attrs["host"], "export=" + attrs["export"]}
	opts = append(opts, tuningOptions(attrs)...)
	opts = append(opts, capability.GetMount().GetMountFlags()...)
	if readOnly {
		opts = append(opts, "ro")
	}
	return strings.Join(opts, ","), nil
}

func requireHostAndExport(attrs map[string]string) error {
	if attrs["host"] == "" || attrs["export"] == "" {
		return fmt.Errorf("volume needs both host and export attributes")
	}
	return nil
}

func refuseCommas(attrs map[string]string) error {
	for key, value := range attrs {
		if carried(key) && strings.Contains(value, ",") {
			return fmt.Errorf("%s contains a comma, which would split the mount options", key)
		}
	}
	return nil
}

func carried(key string) bool {
	return key == "host" || key == "export" || valuedKeys[key] || flagKeys[key]
}

func tuningOptions(attrs map[string]string) []string {
	var opts []string
	for _, key := range sortedKeys(attrs) {
		switch {
		case valuedKeys[key]:
			opts = append(opts, key+"="+attrs[key])
		case flagKeys[key] && attrs[key] != "false":
			opts = append(opts, key)
		}
	}
	return opts
}

func sortedKeys(attrs map[string]string) []string {
	keys := make([]string, 0, len(attrs))
	for key := range attrs {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	return keys
}

// --mountpoint and not --target: the two are mutually exclusive, and only the
// first answers for the path itself rather than the filesystem holding it.
func mountedOptions(target string) (string, bool) {
	out, err := exec.Command("findmnt", "-rno", "OPTIONS", "--mountpoint", target).Output()
	if err != nil {
		return "", false
	}
	return strings.TrimSpace(string(out)), true
}

// Whole options rather than substrings: one host can be a prefix of another,
// and either could be the last option with no comma after it.
func serves(options string, attrs map[string]string) bool {
	want := map[string]bool{"host=" + attrs["host"]: false, "export=" + attrs["export"]: false}

	for _, one := range strings.Split(options, ",") {
		if _, ok := want[one]; ok {
			want[one] = true
		}
	}

	for _, found := range want {
		if !found {
			return false
		}
	}

	return true
}

func (n *node) NodeGetInfo(context.Context, *csi.NodeGetInfoRequest) (*csi.NodeGetInfoResponse, error) {
	return &csi.NodeGetInfoResponse{NodeId: n.id}, nil
}

func (*node) NodeGetCapabilities(context.Context, *csi.NodeGetCapabilitiesRequest) (*csi.NodeGetCapabilitiesResponse, error) {
	return &csi.NodeGetCapabilitiesResponse{}, nil
}

func (*node) NodePublishVolume(ctx context.Context, req *csi.NodePublishVolumeRequest) (*csi.NodePublishVolumeResponse, error) {
	target, err := targetPathOf(req.GetTargetPath())
	if err != nil {
		return nil, err
	}
	if req.GetVolumeCapability().GetBlock() != nil {
		return nil, status.Error(codes.InvalidArgument, "railfs serves a filesystem, not a block device")
	}

	opts, err := mountOptions(req.GetVolumeContext(), req.GetVolumeCapability(), req.GetReadonly())
	if err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}

	published, err := publishedAlready(target, req.GetVolumeContext())
	if err != nil || published {
		return &csi.NodePublishVolumeResponse{}, err
	}

	if err := mountRailfs(ctx, opts, target); err != nil {
		return nil, err
	}
	return &csi.NodePublishVolumeResponse{}, nil
}

func (*node) NodeUnpublishVolume(ctx context.Context, req *csi.NodeUnpublishVolumeRequest) (*csi.NodeUnpublishVolumeResponse, error) {
	target, err := targetPathOf(req.GetTargetPath())
	if err != nil {
		return nil, err
	}

	if _, mounted := mountedOptions(target); mounted {
		if err := unmount(ctx, target); err != nil {
			return nil, err
		}
	}

	if err := os.Remove(target); err != nil && !os.IsNotExist(err) {
		return nil, status.Errorf(codes.Internal, "remove %s: %v", target, err)
	}
	return &csi.NodeUnpublishVolumeResponse{}, nil
}

func targetPathOf(target string) (string, error) {
	if target == "" {
		return "", status.Error(codes.InvalidArgument, "no target path")
	}
	return target, nil
}

func publishedAlready(target string, attrs map[string]string) (bool, error) {
	options, mounted := mountedOptions(target)
	if !mounted {
		return false, nil
	}
	if !serves(options, attrs) {
		return false, status.Errorf(codes.FailedPrecondition, "%s already holds another mount: %s", target, options)
	}
	return true, nil
}

func mountRailfs(ctx context.Context, opts, target string) error {
	if err := os.MkdirAll(target, 0o750); err != nil {
		return status.Errorf(codes.Internal, "mkdir %s: %v", target, err)
	}
	out, err := exec.CommandContext(ctx, "mount", "-i", "-t", "railfs", "-o", opts, "none", target).CombinedOutput()
	if err != nil {
		return status.Errorf(codes.Internal, "mount -o %s %s: %v: %s", opts, target, err, strings.TrimSpace(string(out)))
	}
	return nil
}

func unmount(ctx context.Context, target string) error {
	out, err := exec.CommandContext(ctx, "umount", target).CombinedOutput()
	if err != nil {
		return status.Errorf(codes.Internal, "umount %s: %v: %s", target, err, strings.TrimSpace(string(out)))
	}
	return nil
}
