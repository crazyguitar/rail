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

var valued = map[string]bool{
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

var bare = map[string]bool{
	"rdma":     true,
	"noverify": true,
}

func mountOptions(attrs map[string]string, capability *csi.VolumeCapability, readOnly bool) (string, error) {
	if attrs["host"] == "" || attrs["export"] == "" {
		return "", fmt.Errorf("volume needs both host and export attributes")
	}

	for k, v := range attrs {
		if k == "host" || k == "export" || valued[k] || bare[k] {
			if strings.Contains(v, ",") {
				return "", fmt.Errorf("%s contains a comma, which would split the mount options", k)
			}
		}
	}

	opts := []string{"host=" + attrs["host"], "export=" + attrs["export"]}

	keys := make([]string, 0, len(attrs))
	for k := range attrs {
		keys = append(keys, k)
	}
	sort.Strings(keys)

	for _, k := range keys {
		switch {
		case valued[k]:
			opts = append(opts, k+"="+attrs[k])
		case bare[k]:
			if attrs[k] != "false" {
				opts = append(opts, k)
			}
		}
	}

	if mount := capability.GetMount(); mount != nil {
		opts = append(opts, mount.GetMountFlags()...)
	}
	if readOnly {
		opts = append(opts, "ro")
	}

	return strings.Join(opts, ","), nil
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
	target := req.GetTargetPath()
	if target == "" {
		return nil, status.Error(codes.InvalidArgument, "no target path")
	}

	if capability := req.GetVolumeCapability(); capability.GetBlock() != nil {
		return nil, status.Error(codes.InvalidArgument, "railfs serves a filesystem, not a block device")
	}

	opts, err := mountOptions(req.GetVolumeContext(), req.GetVolumeCapability(), req.GetReadonly())
	if err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}

	// Already mounted is only success when it is this volume. A target left by
	// another one would otherwise be reported as published and never checked.
	if options, ok := mountedOptions(target); ok {
		if !serves(options, req.GetVolumeContext()) {
			return nil, status.Errorf(codes.FailedPrecondition, "%s already holds another mount: %s", target, options)
		}

		return &csi.NodePublishVolumeResponse{}, nil
	}

	if err := os.MkdirAll(target, 0o750); err != nil {
		return nil, status.Errorf(codes.Internal, "mkdir %s: %v", target, err)
	}

	out, err := exec.CommandContext(ctx, "mount", "-i", "-t", "railfs", "-o", opts, "none", target).CombinedOutput()
	if err != nil {
		return nil, status.Errorf(codes.Internal, "mount -o %s %s: %v: %s", opts, target, err, strings.TrimSpace(string(out)))
	}

	return &csi.NodePublishVolumeResponse{}, nil
}

func (*node) NodeUnpublishVolume(ctx context.Context, req *csi.NodeUnpublishVolumeRequest) (*csi.NodeUnpublishVolumeResponse, error) {
	target := req.GetTargetPath()
	if target == "" {
		return nil, status.Error(codes.InvalidArgument, "no target path")
	}

	if _, ok := mountedOptions(target); ok {
		out, err := exec.CommandContext(ctx, "umount", target).CombinedOutput()
		if err != nil {
			return nil, status.Errorf(codes.Internal, "umount %s: %v: %s", target, err, strings.TrimSpace(string(out)))
		}
	}

	if err := os.Remove(target); err != nil && !os.IsNotExist(err) {
		return nil, status.Errorf(codes.Internal, "remove %s: %v", target, err)
	}

	return &csi.NodeUnpublishVolumeResponse{}, nil
}
