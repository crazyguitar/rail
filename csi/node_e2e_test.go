package main

import (
	"context"
	"os"
	"os/exec"
	"strings"
	"testing"

	"github.com/container-storage-interface/spec/lib/go/csi"
)

func settings(t *testing.T) map[string]string {
	t.Helper()

	host := os.Getenv("RAILFS_CSI_HOST")
	export := os.Getenv("RAILFS_CSI_EXPORT")
	if host == "" || export == "" {
		t.Skip("set RAILFS_CSI_HOST and RAILFS_CSI_EXPORT to run these")
	}

	attrs := map[string]string{"host": host, "export": export}
	if port := os.Getenv("RAILFS_CSI_PORT"); port != "" {
		attrs["port"] = port
	}
	if os.Getenv("RAILFS_CSI_TCP") == "" {
		attrs["rdma"] = "true"
	}

	return attrs
}

func mountedAt(t *testing.T, target string) bool {
	t.Helper()
	return exec.Command("findmnt", "-rno", "TARGET", "--mountpoint", target).Run() == nil
}

func publish(ctx context.Context, target string, attrs map[string]string) error {
	n := &node{id: "test"}
	_, err := n.NodePublishVolume(ctx, &csi.NodePublishVolumeRequest{
		VolumeId:         "test",
		TargetPath:       target,
		VolumeContext:    attrs,
		VolumeCapability: &csi.VolumeCapability{AccessType: &csi.VolumeCapability_Mount{Mount: &csi.VolumeCapability_MountVolume{}}},
	})
	return err
}

func unpublish(ctx context.Context, target string) error {
	n := &node{id: "test"}
	_, err := n.NodeUnpublishVolume(ctx, &csi.NodeUnpublishVolumeRequest{VolumeId: "test", TargetPath: target})
	return err
}

// The unmount is what regressed once already: findmnt was asked with two
// mutually exclusive flags, so the driver never saw its own mount and left it
// behind on every pod that went away.
func TestPublishThenUnpublishLeavesNothingMounted(t *testing.T) {
	attrs := settings(t)
	ctx := context.Background()
	target := t.TempDir() + "/mount"

	if err := publish(ctx, target, attrs); err != nil {
		t.Fatalf("publish: %v", err)
	}

	if !mountedAt(t, target) {
		t.Fatal("published, but nothing is mounted there")
	}

	if err := unpublish(ctx, target); err != nil {
		t.Fatalf("unpublish: %v", err)
	}

	if mountedAt(t, target) {
		t.Error("unpublished, and the mount is still there")
	}
}

func TestPublishIsIdempotentForTheSameVolume(t *testing.T) {
	attrs := settings(t)
	ctx := context.Background()
	target := t.TempDir() + "/mount"

	if err := publish(ctx, target, attrs); err != nil {
		t.Fatalf("first publish: %v", err)
	}
	defer unpublish(ctx, target)

	if err := publish(ctx, target, attrs); err != nil {
		t.Errorf("second publish of the same volume: %v", err)
	}
}

// A target already holding someone else's mount is a precondition failure, not
// a success: reporting it published would hand the pod the wrong filesystem.
func TestPublishRefusesATargetHoldingAnotherVolume(t *testing.T) {
	attrs := settings(t)
	ctx := context.Background()
	target := t.TempDir() + "/mount"

	if err := publish(ctx, target, attrs); err != nil {
		t.Fatalf("publish: %v", err)
	}
	defer unpublish(ctx, target)

	other := map[string]string{}
	for k, v := range attrs {
		other[k] = v
	}
	other["export"] = attrs["export"] + "-somewhere-else"

	err := publish(ctx, target, other)
	if err == nil {
		t.Fatal("accepted a target already holding another volume")
	}

	if !strings.Contains(err.Error(), "already holds another mount") {
		t.Errorf("refused for the wrong reason: %v", err)
	}
}

func TestPublishRefusesABlockVolume(t *testing.T) {
	attrs := settings(t)
	ctx := context.Background()
	target := t.TempDir() + "/mount"

	n := &node{id: "test"}
	_, err := n.NodePublishVolume(ctx, &csi.NodePublishVolumeRequest{
		VolumeId:         "test",
		TargetPath:       target,
		VolumeContext:    attrs,
		VolumeCapability: &csi.VolumeCapability{AccessType: &csi.VolumeCapability_Block{Block: &csi.VolumeCapability_BlockVolume{}}},
	})

	if err == nil {
		t.Fatal("accepted a block volume")
	}

	if !strings.Contains(err.Error(), "not a block device") {
		t.Errorf("refused for the wrong reason: %v", err)
	}
}

// These need no mount and no peer, so they run wherever the tests do.

func TestMountOptionsNeedsHostAndExport(t *testing.T) {
	for _, attrs := range []map[string]string{{}, {"host": "10.0.0.1"}, {"export": "models"}} {
		if _, err := mountOptions(attrs, nil, false); err == nil {
			t.Errorf("accepted %v", attrs)
		}
	}
}

// A comma in a value would end the option early and start another, so the
// mount would land somewhere nobody asked for.
func TestMountOptionsRefusesACommaThatWouldSplitThem(t *testing.T) {
	for _, attrs := range []map[string]string{
		{"host": "10.0.0.1", "export": "mo,dels"},
		{"host": "10.0.0.1,10.0.0.2", "export": "models"},
		{"host": "10.0.0.1", "export": "models", "port": "18,600"},
	} {
		_, err := mountOptions(attrs, nil, false)
		if err == nil || !strings.Contains(err.Error(), "comma") {
			t.Errorf("%v gave %v", attrs, err)
		}
	}
}

func TestMountOptionsCarriesTheKnownKeysAndDropsTheRest(t *testing.T) {
	got, err := mountOptions(map[string]string{
		"host": "10.0.0.1", "export": "models", "port": "18600", "rdma": "true", "nonsense": "x",
	}, nil, true)
	if err != nil {
		t.Fatal(err)
	}

	for _, want := range []string{"host=10.0.0.1", "export=models", "port=18600", "rdma", "ro"} {
		if !strings.Contains(got, want) {
			t.Errorf("%q missing from %q", want, got)
		}
	}

	if strings.Contains(got, "nonsense") {
		t.Errorf("an unknown key reached the mount: %q", got)
	}
}

func TestIdentityNamesItselfAndReportsReady(t *testing.T) {
	i := &identity{name: "railfs.csi.rail.io"}
	ctx := context.Background()

	info, err := i.GetPluginInfo(ctx, &csi.GetPluginInfoRequest{})
	if err != nil || info.GetName() != "railfs.csi.rail.io" {
		t.Errorf("got %q, %v", info.GetName(), err)
	}

	ready, err := i.Probe(ctx, &csi.ProbeRequest{})
	if err != nil || !ready.GetReady().GetValue() {
		t.Errorf("probe said %v, %v", ready.GetReady().GetValue(), err)
	}

	// No controller service: this driver only ever publishes on a node.
	caps, err := i.GetPluginCapabilities(ctx, &csi.GetPluginCapabilitiesRequest{})
	if err != nil || len(caps.GetCapabilities()) != 0 {
		t.Errorf("claimed %d capabilities, %v", len(caps.GetCapabilities()), err)
	}
}

func TestNodeReportsTheIdItWasGiven(t *testing.T) {
	n := &node{id: "test-node"}
	ctx := context.Background()

	info, err := n.NodeGetInfo(ctx, &csi.NodeGetInfoRequest{})
	if err != nil || info.GetNodeId() != "test-node" {
		t.Errorf("got %q, %v", info.GetNodeId(), err)
	}

	caps, err := n.NodeGetCapabilities(ctx, &csi.NodeGetCapabilitiesRequest{})
	if err != nil || len(caps.GetCapabilities()) != 0 {
		t.Errorf("claimed %d capabilities, %v", len(caps.GetCapabilities()), err)
	}
}
