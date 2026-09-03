package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"net"
	"net/url"
	"os"
	"os/signal"
	"syscall"

	"github.com/container-storage-interface/spec/lib/go/csi"
	"google.golang.org/grpc"
)

type driverConfig struct {
	endpoint   string
	nodeID     string
	driverName string
}

func main() {
	config := parseFlags()

	socket, err := unixSocketPath(config.endpoint)
	if err != nil {
		log.Fatalf("railfs-csi: %v", err)
	}

	listener, err := listenFresh(socket)
	if err != nil {
		log.Fatalf("railfs-csi: %v", err)
	}

	server := newDriverServer(config)
	stopOnSignal(server)

	log.Printf("railfs-csi: %s on %s, node %s", config.driverName, socket, config.nodeID)
	if err := server.Serve(listener); err != nil {
		log.Fatalf("railfs-csi: serve: %v", err)
	}
}

func parseFlags() driverConfig {
	var config driverConfig
	flag.StringVar(&config.endpoint, "endpoint", "unix:///csi/csi.sock", "CSI endpoint")
	flag.StringVar(&config.nodeID, "nodeid", "", "name of the node this instance runs on")
	flag.StringVar(&config.driverName, "drivername", "railfs.csi.rail.io", "registered driver name")
	flag.Parse()

	if config.nodeID == "" {
		log.Fatal("railfs-csi: -nodeid is required")
	}
	return config
}

func unixSocketPath(endpoint string) (string, error) {
	parsed, err := url.Parse(endpoint)
	if err != nil {
		return "", fmt.Errorf("endpoint %s: %w", endpoint, err)
	}
	if parsed.Scheme != "unix" {
		return "", fmt.Errorf("endpoint %s: only unix sockets are served", endpoint)
	}
	return parsed.Path, nil
}

func listenFresh(socket string) (net.Listener, error) {
	if err := os.Remove(socket); err != nil && !os.IsNotExist(err) {
		return nil, fmt.Errorf("stale socket %s: %w", socket, err)
	}
	listener, err := net.Listen("unix", socket)
	if err != nil {
		return nil, fmt.Errorf("listen %s: %w", socket, err)
	}
	return listener, nil
}

func newDriverServer(config driverConfig) *grpc.Server {
	server := grpc.NewServer(grpc.UnaryInterceptor(logFailures))
	csi.RegisterIdentityServer(server, &identity{name: config.driverName})
	csi.RegisterNodeServer(server, &node{id: config.nodeID})
	return server
}

func stopOnSignal(server *grpc.Server) {
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGTERM, syscall.SIGINT)
	go func() {
		<-stop
		server.GracefulStop()
	}()
}

func logFailures(ctx context.Context, req any, info *grpc.UnaryServerInfo, handler grpc.UnaryHandler) (any, error) {
	resp, err := handler(ctx, req)
	if err != nil {
		log.Printf("railfs-csi: %s: %v", info.FullMethod, err)
	}
	return resp, err
}
