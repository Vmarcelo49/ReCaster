// Package main — protocol wire format for the zzcaster relay.
//
// This is a CCCaster-compatible protocol with one extension: room codes.
// The original CCCaster protocol matches host and client purely on the
// host's public IP:port string. zzcaster uses 4-letter room codes instead
// — friendlier UX, works behind any NAT, and the relay doesn't need to
// know either peer's public IP up front (it learns it from the UDP
// UdpData packets).
//
// All integers are LITTLE-ENDIAN (matching CCCaster).
//
// See docs/nat-traversal-protocol.md for the full spec.
package main

import (
        "encoding/binary"
        "errors"
        "fmt"
)

// ============================================================================
// Constants
// ============================================================================

// Socket type byte — first byte of HostRegister / ClientJoin.
const (
        TypeTCP byte = 'T'
        TypeUDP byte = 'U'
)

// Magic header strings — same as CCCaster.
var (
        MatchInfoHeader = []byte("MatchInfo") // 9 bytes
        TunInfoHeader   = []byte("TunInfo")   // 7 bytes
        ErrorHeader     = []byte("Error")     // 5 bytes
        HostedHeader    = []byte("Hosted")    // 6 bytes — NEW: reply to host with assigned room code
)

// Error codes — sent as a single byte after the "Error" header.
//
// These are the wire-format values. The *RoomError instances in room.go
// (ErrRoomNotFound, ErrRoomExpired, etc.) implement the error interface
// and carry both the code (in .Code) and a human-readable message (in
// .Msg). Use those for return values; use their .Code field when calling
// EncodeError.
//
// DO NOT redeclare these as standalone constants here — that would
// conflict with the *RoomError vars in room.go (same names, different
// types). The codes are documented here for reference only.
//
//   1 = ErrRoomNotFound  — ClientJoin referenced a code that doesn't exist
//   2 = ErrRoomExpired   — Room existed but TTL elapsed
//   3 = ErrProtocolError — Malformed message or unexpected state
//   4 = ErrRoomTaken     — HostRegister with a code that already exists

// Room code configuration.
const (
        RoomCodeLen     = 4    // 4 letters — easy to type, share verbally, paste
        RoomCodeAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789" // no I/O/0/1 (ambiguous)
)

// Size limits for sanity-checking incoming packets.
const (
        MaxHostRegisterLen = 1 + 2 + 1 + RoomCodeLen // type + port + code_len + code = 8 bytes
        MaxClientJoinLen   = 1 + 1 + RoomCodeLen     // type + code_len + code = 6 bytes
        MaxTunInfoLen      = 7 + 4 + 22 + 1          // header + matchId + "255.255.255.255:65535" + null
        MaxErrorLen        = 5 + 1 + 64              // header + code + message
)

// ============================================================================
// Outgoing message encoders
// ============================================================================

// EncodeMatchInfo builds a MatchInfo message: "MatchInfo" + uint32 matchId.
// Sent to BOTH host and client when a match is made.
func EncodeMatchInfo(matchId uint32) []byte {
        buf := make([]byte, 0, 9+4)
        buf = append(buf, MatchInfoHeader...)
        buf = binary.LittleEndian.AppendUint32(buf, matchId)
        return buf
}

// EncodeTunInfo builds a TunInfo message: "TunInfo" + uint32 matchId + "ip:port\0".
// Sent to the OPPOSITE peer — host receives client's address, client receives host's.
// addr must be "ip:port" (no scheme, no null terminator — we add it).
func EncodeTunInfo(matchId uint32, addr string) []byte {
        buf := make([]byte, 0, 7+4+len(addr)+1)
        buf = append(buf, TunInfoHeader...)
        buf = binary.LittleEndian.AppendUint32(buf, matchId)
        buf = append(buf, addr...)
        buf = append(buf, 0) // null terminator
        return buf
}

// EncodeError builds an Error message: "Error" + uint8 code + message bytes.
// Sent when a join fails, protocol is violated, etc.
func EncodeError(code byte, msg string) []byte {
        buf := make([]byte, 0, 5+1+len(msg))
        buf = append(buf, ErrorHeader...)
        buf = append(buf, code)
        buf = append(buf, msg...)
        return buf
}

// EncodeHosted builds a Hosted message: "Hosted" + 4-byte room code.
// Sent in reply to a successful HostRegister so the host learns its code.
// (The host generated the code locally and sent it in HostRegister — this
// is just a confirmation. If the host sent an empty code, the server
// generates one and returns it here.)
func EncodeHosted(code string) []byte {
        buf := make([]byte, 0, 6+RoomCodeLen)
        buf = append(buf, HostedHeader...)
        buf = append(buf, code...)
        return buf
}

// ============================================================================
// Incoming message decoders
// ============================================================================

// HostRegister is the host's initial TCP message.
// Wire format: [u8 type 'T'|'U'][u16 le port][u8 code_len][code bytes]
//
// If code is empty (code_len == 0), the server generates a random 4-letter
// code and returns it via EncodeHosted.
type HostRegister struct {
        Type byte   // 'T' or 'U'
        Port uint16 // local port the host is listening on (informational)
        Code string // room code — may be empty (server assigns one)
}

func DecodeHostRegister(data []byte) (HostRegister, error) {
        // Minimum: type + port + code_len = 4 bytes
        if len(data) < 4 {
                return HostRegister{}, errors.New("HostRegister too short")
        }
        t := data[0]
        if t != TypeTCP && t != TypeUDP {
                return HostRegister{}, fmt.Errorf("invalid type byte %d", t)
        }
        port := binary.LittleEndian.Uint16(data[1:3])
        codeLen := int(data[3])
        if codeLen > RoomCodeLen {
                return HostRegister{}, fmt.Errorf("code length %d exceeds max %d", codeLen, RoomCodeLen)
        }
        if len(data) < 4+codeLen {
                return HostRegister{}, errors.New("HostRegister truncated")
        }
        code := string(data[4 : 4+codeLen])
        return HostRegister{Type: t, Port: port, Code: code}, nil
}

// EncodeHostRegister encodes a HostRegister for the client side
// (the zzcaster Zig client uses the same format).
func EncodeHostRegister(t byte, port uint16, code string) []byte {
        buf := make([]byte, 0, 4+len(code))
        buf = append(buf, t)
        buf = binary.LittleEndian.AppendUint16(buf, port)
        buf = append(buf, byte(len(code)))
        buf = append(buf, code...)
        return buf
}

// ClientJoin is the client's initial TCP message.
// Wire format: [u8 type 'T'|'U'][u8 code_len][code bytes]
//
// The type byte tells the relay whether the client wants TCP or UDP
// hole-punching — currently always 'U' for ENet. We keep the byte for
// CCCaster compatibility and future flexibility.
type ClientJoin struct {
        Type byte   // 'T' or 'U'
        Code string // 4-letter room code
}

func DecodeClientJoin(data []byte) (ClientJoin, error) {
        if len(data) < 2 {
                return ClientJoin{}, errors.New("ClientJoin too short")
        }
        t := data[0]
        if t != TypeTCP && t != TypeUDP {
                return ClientJoin{}, fmt.Errorf("invalid type byte %d", t)
        }
        codeLen := int(data[1])
        if codeLen != RoomCodeLen {
                return ClientJoin{}, fmt.Errorf("expected %d-char room code, got %d", RoomCodeLen, codeLen)
        }
        if len(data) < 2+codeLen {
                return ClientJoin{}, errors.New("ClientJoin truncated")
        }
        return ClientJoin{Type: t, Code: string(data[2 : 2+codeLen])}, nil
}

// EncodeClientJoin encodes a ClientJoin (used by the Zig client).
func EncodeClientJoin(t byte, code string) []byte {
        buf := make([]byte, 0, 2+len(code))
        buf = append(buf, t)
        buf = append(buf, byte(len(code)))
        buf = append(buf, code...)
        return buf
}

// UdpData is the 5-byte UDP packet both peers send to the relay every 50ms.
// Wire format: [u8 isClient][u32 le matchId]
//
// The relay uses the source address of this packet to learn each peer's
// public UDP endpoint, then sends TunInfo to the opposite peer over TCP.
type UdpData struct {
        IsClient bool
        MatchId  uint32
}

func DecodeUdpData(data []byte) (UdpData, error) {
        if len(data) < 5 {
                return UdpData{}, errors.New("UdpData too short")
        }
        return UdpData{
                IsClient: data[0] != 0,
                MatchId:  binary.LittleEndian.Uint32(data[1:5]),
        }, nil
}

func EncodeUdpData(isClient bool, matchId uint32) []byte {
        buf := make([]byte, 5)
        if isClient {
                buf[0] = 1
        }
        binary.LittleEndian.PutUint32(buf[1:5], matchId)
        return buf
}

// ============================================================================
// Server-side TCP message dispatcher
// ============================================================================

// IncomingTCP is a decoded incoming TCP message from a peer.
// Only one field will be set, depending on Kind.
type IncomingTCP struct {
        Kind         MessageKind
        HostRegister *HostRegister
        ClientJoin   *ClientJoin
}

type MessageKind int

const (
        KindUnknown MessageKind = iota
        KindHostRegister
        KindClientJoin
)

// ClassifyIncomingTCP looks at the first byte to decide which kind of
// message this is. HostRegister starts with 'T' or 'U'; ClientJoin also
// starts with 'T' or 'U' — so we distinguish by length:
//   - 4+ bytes starting with T|U + uint16 port = HostRegister
//   - 2-3 bytes starting with T|U + code_len = ClientJoin
//
// This mirrors CCCaster's server.py logic (which uses string length to
// distinguish TypedHostingPort from TypedConnectionAddress).
//
// Note: this is called on the FIRST read from a fresh TCP connection.
// After the first message, the connection enters "matched" state and
// only receives MatchInfo / TunInfo / Error from the server.
func ClassifyIncomingTCP(data []byte) (IncomingTCP, error) {
        if len(data) < 2 {
                return IncomingTCP{Kind: KindUnknown}, errors.New("packet too short")
        }
        t := data[0]
        if t != TypeTCP && t != TypeUDP {
                return IncomingTCP{Kind: KindUnknown}, fmt.Errorf("invalid type byte %d (expected 'T' or 'U')", t)
        }

        // Heuristic: if we have at least 4 bytes AND the third byte is a
        // plausible port number's low byte AND the fourth byte is a small
        // code_len (0..4), treat it as HostRegister.
        //
        // Simpler heuristic that matches CCCaster: HostRegister is exactly
        // 4 + code_len bytes; ClientJoin is exactly 2 + code_len bytes.
        // Since the caller reads a full message before calling us, length
        // alone is enough.
        //
        // We expect the caller to know how many bytes they read (via the
        // length-prefix or by reading until a sentinel). For now we use a
        // pragmatic check: if len >= 4 and data[3] <= RoomCodeLen, it's
        // HostRegister; otherwise ClientJoin.
        if len(data) >= 4 && data[3] <= RoomCodeLen && len(data) == 4+int(data[3]) {
                hr, err := DecodeHostRegister(data)
                if err != nil {
                        return IncomingTCP{Kind: KindUnknown}, err
                }
                return IncomingTCP{Kind: KindHostRegister, HostRegister: &hr}, nil
        }

        // Otherwise treat as ClientJoin
        cj, err := DecodeClientJoin(data)
        if err != nil {
                return IncomingTCP{Kind: KindUnknown}, err
        }
        return IncomingTCP{Kind: KindClientJoin, ClientJoin: &cj}, nil
}

// ============================================================================
// Server-side TCP response decoder (used by tests, not by the server itself)
// ============================================================================

// ServerResponse is a decoded message flowing from server to peer.
type ServerResponse struct {
        Kind     ResponseKind
        MatchId  uint32 // for MatchInfo, TunInfo
        Addr     string // for TunInfo
        ErrCode  byte   // for Error
        ErrMsg   string // for Error
        RoomCode string // for Hosted
}

type ResponseKind int

const (
        RespUnknown ResponseKind = iota
        RespMatchInfo
        RespTunInfo
        RespError
        RespHosted
)

func DecodeServerResponse(data []byte) ServerResponse {
        if len(data) >= 9+4 && string(data[:9]) == string(MatchInfoHeader) {
                return ServerResponse{
                        Kind:    RespMatchInfo,
                        MatchId: binary.LittleEndian.Uint32(data[9:13]),
                }
        }
        if len(data) >= 7+4 && string(data[:7]) == string(TunInfoHeader) {
                matchId := binary.LittleEndian.Uint32(data[7:11])
                // Address is null-terminated
                addrEnd := 11
                for addrEnd < len(data) && data[addrEnd] != 0 {
                        addrEnd++
                }
                return ServerResponse{
                        Kind:    RespTunInfo,
                        MatchId: matchId,
                        Addr:    string(data[11:addrEnd]),
                }
        }
        if len(data) >= 5+1 && string(data[:5]) == string(ErrorHeader) {
                return ServerResponse{
                        Kind:    RespError,
                        ErrCode: data[5],
                        ErrMsg:  string(data[6:]),
                }
        }
        if len(data) >= 6+RoomCodeLen && string(data[:6]) == string(HostedHeader) {
                return ServerResponse{
                        Kind:     RespHosted,
                        RoomCode: string(data[6 : 6+RoomCodeLen]),
                }
        }
        return ServerResponse{Kind: RespUnknown}
}

// ============================================================================
// Room code generation
// ============================================================================

// GenerateRoomCode is defined in room.go (it needs access to the room
// manager's state to guarantee uniqueness). This file only defines the
// alphabet and length constants.
