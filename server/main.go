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
// ZZ_RELAY_TTL, ZZ_LOG_LEVEL).
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

	logger := log.New(os.Stdout, "", log.LstdFlags|log.Lmicroseconds)
	logger.Printf("zzcaster-relay starting: addr=%s ttl=%s log=%s", *addr, ttl, *logLevel)

	// For "error" level, suppress info logs.
	if *logLevel == "error" {
		logger = log.New(os.Stderr, "[ERROR] ", log.LstdFlags)
	}

	tcpCfg := DefaultTCPConfig()
	tcpCfg.Addr = *addr
	tcpCfg.RoomTTL = ttl

	udpCfg := DefaultUDPConfig()
	udpCfg.Addr = *addr

	// Context cancelled on Ctrl+C / SIGTERM
	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	rm := NewRoomManager()

	// Start TCP and UDP listeners in parallel. Both block; we run them
	// in goroutines and wait for either to error out (or for the context
	// to cancel on shutdown signal).
	errCh := make(chan error, 2)
	go func() { errCh <- StartTCPListener(ctx, tcpCfg, rm, logger) }()
	go func() { errCh <- StartUDPListener(ctx, udpCfg, rm, logger) }()

	// Wait for shutdown signal or first listener to error.
	for i := 0; i < 2; i++ {
		select {
		case err := <-errCh:
			if err != nil {
				logger.Printf("listener error: %v", err)
			}
		case <-ctx.Done():
			logger.Printf("shutdown signal received")
			cancel()
		}
	}
	logger.Printf("zzcaster-relay stopped")
}

// getenvDefault returns the env var value if set, else def.
func getenvDefault(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

// defaultRand is the default source of randomness for room code generation.
// Replaced in tests with a deterministic source.
var defaultRand = func() int { return rand.Intn(1 << 30) }
