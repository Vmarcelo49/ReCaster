// Package main — zzcaster relay server entry point.
//
// The relay is a signaling-only server for NAT traversal. It does NOT
// relay game packets — game traffic flows peer-to-peer via ENet once
// the hole-punch is complete. The relay only:
//
//   1. Matches host and client by room code (TCP)
//   2. Learns each peer's public UDP endpoint (via UdpData packets on UDP)
//   3. Forwards each endpoint to the opposite peer (TunInfo over TCP)
//
// After that, the relay's job is done. Peers talk ENet directly.
//
// Usage:
//   zzcaster-relay [-addr :3939] [-ttl 60s] [-log info]
//
// All flags can also be set via environment variables (ZZ_RELAY_ADDR,
// ZZ_RELAY_TTL, ZZ_LOG_LEVEL); an explicitly set flag wins.
package main

import (
	"context"
	"flag"
	"log"
	"math/rand"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func main() {
	var (
		addr     = flag.String("addr", getenvDefault("ZZ_RELAY_ADDR", ":3939"), "TCP+UDP listen address")
		ttlStr   = flag.String("ttl", getenvDefault("ZZ_RELAY_TTL", "60s"), "room TTL")
		logLevel = flag.String("log", getenvDefault("ZZ_LOG_LEVEL", "info"), "log level (debug/info/error)")
	)
	flag.Parse()

	ttl, err := time.ParseDuration(*ttlStr)
	if err != nil {
		log.Fatalf("invalid -ttl %q: %v", *ttlStr, err)
	}
	level, err := parseLogLevel(*logLevel)
	if err != nil {
		log.Fatalf("%v", err)
	}

	logger := newRelayLogger(level)
	logger.Infof("zzcaster-relay starting: addr=%s ttl=%s log=%s", *addr, ttl, *logLevel)

	tcpCfg := DefaultTCPConfig()
	tcpCfg.Addr = *addr
	tcpCfg.RoomTTL = ttl

	udpCfg := DefaultUDPConfig()
	udpCfg.Addr = *addr

	// Context cancelled on Ctrl+C / SIGTERM
	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	rm := NewRoomManager()

	// Start TCP and UDP listeners in parallel. Both block until the
	// context is cancelled (or fail), so run them in goroutines and
	// wait for the first result.
	errCh := make(chan error, 2)
	go func() { errCh <- StartTCPListener(ctx, tcpCfg, rm, logger) }()
	go func() { errCh <- StartUDPListener(ctx, udpCfg, rm, logger) }()

	// The first result decides the outcome. A non-nil error (e.g. the
	// listen port is already bound) is FATAL: a relay with only one of
	// its two listeners running is useless, and staying up would only
	// mask the failure (and spin a Docker restart loop). A nil error
	// means a listener exited because the context was cancelled — i.e.
	// the shutdown signal already fired and we're done.
	if err := <-errCh; err != nil {
		logger.Errorf("listener error: %v", err)
		cancel()
		os.Exit(1)
	}
	logger.Infof("zzcaster-relay stopped")
}

// getenvDefault returns the env var value if set, else def.
func getenvDefault(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

// defaultRand is the default source of randomness for room code generation.
var defaultRand = func() int { return rand.Intn(1 << 30) }
