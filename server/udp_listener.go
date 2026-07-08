// Package main — UDP listener for the relay server.
//
// The UDP socket serves two purposes:
//   1. Receives UdpData packets from matched peers (5 bytes: isClient + matchId)
//   2. Acts as a STUN probe — any packet that isn't a valid UdpData is
//      treated as a STUN request and the server replies with the sender's
//      public ip:port (8 bytes).
//
// When a UdpData packet arrives, the server records the sender's public
// UDP endpoint and sends TunInfo to the OPPOSITE peer's TCP connection.
// Once both peers have sent UdpData and received their TunInfo, the
// relay's job is done — game traffic flows peer-to-peer via ENet.
package main

import (
        "context"
        "fmt"
        "log"
        "net"
)

// UDPListenerConfig configures the UDP listener.
type UDPListenerConfig struct {
        Addr string // ":3939" — same port as TCP, but UDP
}

func DefaultUDPConfig() UDPListenerConfig {
        return UDPListenerConfig{Addr: ":3939"}
}

// StartUDPListener starts the UDP listener. Blocks until ctx is cancelled.
func StartUDPListener(ctx context.Context, cfg UDPListenerConfig, rm *RoomManager, logger *log.Logger) error {
        conn, err := net.ListenPacket("udp", cfg.Addr)
        if err != nil {
                return fmt.Errorf("udp listen on %s: %w", cfg.Addr, err)
        }
        defer conn.Close()
        logger.Printf("UDP listening on %s", cfg.Addr)

        // Close on context cancel
        go func() {
                <-ctx.Done()
                conn.Close()
        }()

        buf := make([]byte, 4096)
        for {
                n, addr, err := conn.ReadFrom(buf)
                if err != nil {
                        if ctx.Err() != nil {
                                return nil
                        }
                        logger.Printf("udp read error: %v", err)
                        continue
                }
                if n == 0 {
                        continue
                }

                udpAddr, ok := addr.(*net.UDPAddr)
                if !ok {
                        continue
                }

                // Log every UDP packet received — helps diagnose firewall /
                // Docker UDP forwarding issues. Without this, it's impossible
                // to tell whether packets are arriving at all.
                logger.Printf("udp: recv %d bytes from %s:%d", n, udpAddr.IP, udpAddr.Port)

                // Try to parse as UdpData first (5-byte packet).
                if n >= 5 {
                        data, err := DecodeUdpData(buf[:5])
                        if err == nil && data.MatchId != 0 {
                                handleUdpData(rm, data, udpAddr, logger)
                                continue
                        }
                }

                // Otherwise treat as STUN probe — reply with 8 bytes:
                // [4 bytes IPv4][2 bytes port BE][2 bytes padding=0]
                handleStunProbe(conn, udpAddr, logger)
        }
}

// handleUdpData is called when a valid 5-byte UdpData packet arrives.
// It looks up the room by matchId, records the sender's public UDP
// endpoint, and sends TunInfo to the opposite peer over TCP.
func handleUdpData(rm *RoomManager, data UdpData, udpAddr *net.UDPAddr, logger *log.Logger) {
        addrStr := fmt.Sprintf("%s:%d", udpAddr.IP.String(), udpAddr.Port)

        tunInfo, opposite, err := rm.RecordPeerUdpAddr(data.MatchId, data.IsClient, addrStr)
        if err != nil {
                // Most common: room not found (peer sent late UdpData after
                // match was deleted). Log at info level — not an error.
                logger.Printf("udp: UdpData matchId=%d isClient=%v addr=%s — %v",
                        data.MatchId, data.IsClient, addrStr, err)
                return
        }

        if tunInfo == nil {
                // Duplicate (already sent TunInfo for this side). No-op.
                return
        }

        logger.Printf("udp: UdpData matchId=%d isClient=%v addr=%s — sent TunInfo to %s",
                data.MatchId, data.IsClient, addrStr, opposite.TCPAddr)
}

// handleStunProbe replies with the sender's public UDP endpoint.
// Format: [4 bytes IPv4 BE][2 bytes port BE][2 bytes padding=0]
// (8 bytes total)
//
// The client compares the port the server sees vs the port it bound
// locally — if they differ, it's behind NAT. A second probe from a
// different local port reveals whether the NAT is symmetric (different
// public port per outbound flow) or cone (same public port).
func handleStunProbe(conn net.PacketConn, addr *net.UDPAddr, logger *log.Logger) {
        ip4 := addr.IP.To4()
        if ip4 == nil {
                // IPv6 — skip for now, zzcaster is IPv4-only
                return
        }
        resp := make([]byte, 8)
        copy(resp[0:4], ip4)
        resp[4] = byte(addr.Port >> 8)
        resp[5] = byte(addr.Port & 0xFF)
        // resp[6:8] = 0,0 (padding)

        if _, err := conn.WriteTo(resp, addr); err != nil {
                logger.Printf("stun: write reply to %s: %v", addr, err)
        }
}
