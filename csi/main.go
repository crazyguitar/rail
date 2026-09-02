package main

import (
	"context"
	"flag"
	"log"
	"net"
	"net/url"
	"os"
	"os/signal"
	"syscall"

	"github.com/container-storage-interface/spec/lib/go/csi"
	"google.golang.org/grpc"
)

func main() {
	endpoint := flag.String("endpoint", "unix:///csi/csi.sock", "CSI endpoint")
	nodeID := flag.String("nodeid", "", "name of the node this instance runs on")
	name := flag.String("drivername", "railfs.csi.rail.io", "registered driver name")
	flag.Parse()

	if *nodeID == "" {
		log.Fatal("railfs-csi: -nodeid is required")
	}

	u, err := url.Parse(*endpoint)
	if err != nil {
		log.Fatalf("railfs-csi: endpoint %s: %v", *endpoint, err)
	}
	if u.Scheme != "unix" {
		log.Fatalf("railfs-csi: endpoint %s: only unix sockets are served", *endpoint)
	}
	if err := os.Remove(u.Path); err != nil && !os.IsNotExist(err) {
		log.Fatalf("railfs-csi: stale socket %s: %v", u.Path, err)
	}

	listener, err := net.Listen("unix", u.Path)
	if err != nil {
		log.Fatalf("railfs-csi: listen %s: %v", u.Path, err)
	}

	server := grpc.NewServer(grpc.UnaryInterceptor(logFailures))
	csi.RegisterIdentityServer(server, &identity{name: *name})
	csi.RegisterNodeServer(server, &node{id: *nodeID})

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGTERM, syscall.SIGINT)
	go func() {
		<-stop
		server.GracefulStop()
	}()

	log.Printf("railfs-csi: %s on %s, node %s", *name, u.Path, *nodeID)
	if err := server.Serve(listener); err != nil {
		log.Fatalf("railfs-csi: serve: %v", err)
	}
}

func logFailures(ctx context.Context, req any, info *grpc.UnaryServerInfo, handler grpc.UnaryHandler) (any, error) {
	resp, err := handler(ctx, req)
	if err != nil {
		log.Printf("railfs-csi: %s: %v", info.FullMethod, err)
	}
	return resp, err
}
