// Package main — room manager.
//
// Rooms represent a pending match between a host (who registered a room
// code) and a client (who wants to join that code). Once both peers are
// present, the server generates a matchId and sends MatchInfo to both.
// Then each peer starts sending UdpData on UDP, the server learns their
// public UDP endpoints, and forwards TunInfo to the opposite peer.
//
// After both TunInfos are sent, the room is deleted — the relay's job
// is done; the peers now talk UDP directly (hole-punched).
package main

import (
        "sync"
        "time"
)

// RoomState tracks the lifecycle of a relay-assisted match.
type RoomState int

const (
        RoomWaiting  RoomState = iota // host registered, waiting for client
        RoomMatched                   // client joined, MatchInfo sent to both, waiting for UdpData on UDP
        RoomDone                      // both TunInfos sent, room deleted
)

// Room is a pending match between a host and a client.
type Room struct {
        Code       string    // 4-letter room code (key in RoomManager.rooms)
        MatchId    uint32    // 0 until matched, then unique non-zero
        State      RoomState
        HostConn   *PeerConn // host's TCP connection
        ClientConn *PeerConn // client's TCP connection (nil until joined)

        // UDP endpoint tracking — populated by the UDP handler as UdpData
        // packets arrive. Once both are set AND both TunInfos have been sent,
        // the room is deleted.
        HostUdpAddr   string
        ClientUdpAddr string
        HostTunSent   bool // true after TunInfo was sent to host (with client's addr)
        ClientTunSent bool // true after TunInfo was sent to client (with host's addr)

        Created time.Time
        Expires time.Time // Created + TTL — room is garbage-collected after this
}

// PeerConn wraps a TCP connection with metadata the relay needs.
type PeerConn struct {
        TCPAddr  string // "host:port" as seen by the relay (peer's public TCP endpoint)
        Conn     interface {
                Write([]byte) (int, error)
                Close() error
        }
        IsClient bool // false = host, true = client
}

// RoomManager is the in-memory store of rooms. Safe for concurrent access.
//
// The relay is intentionally stateless across restarts — all room state
// lives in memory and is lost on restart. Peers reconnect and re-register.
type RoomManager struct {
        mu    sync.Mutex
        rooms map[string]*Room
        // matchIndex generates unique matchIds. Starts at 1 (0 is reserved as
        // "no match yet" sentinel). Wraps at 2^32-1 then back to 1.
        matchIndex uint32
}

func NewRoomManager() *RoomManager {
        return &RoomManager{
                rooms:      make(map[string]*Room),
                matchIndex: 0,
        }
}

// nextMatchId returns a non-zero, monotonically-increasing matchId.
// Caller must hold rm.mu.
func (rm *RoomManager) nextMatchId() uint32 {
        rm.matchIndex++
        if rm.matchIndex == 0 { // wraparound
                rm.matchIndex = 1
        }
        return rm.matchIndex
}

// RegisterHost creates a new room with the given code.
// If code is empty, generates a random one.
// If code already exists, returns ErrRoomTaken.
//
// Returns: the assigned code (for the Hosted reply) and the new Room.
func (rm *RoomManager) RegisterHost(code string, hostConn *PeerConn, ttl time.Duration) (string, *Room, error) {
        rm.mu.Lock()
        defer rm.mu.Unlock()

        assignedCode := code
        if assignedCode == "" {
                // Generate a unique random code — try up to 10 times to avoid
                // collisions (4-letter alphabet has ~1B codes, so collisions
                // are rare unless we're at scale).
                for i := 0; i < 10; i++ {
                        candidate := GenerateRoomCode(defaultRand)
                        if _, exists := rm.rooms[candidate]; !exists {
                                assignedCode = candidate
                                break
                        }
                }
                if assignedCode == "" {
                        return "", nil, ErrRoomTaken
                }
        } else {
                if _, exists := rm.rooms[assignedCode]; exists {
                        return "", nil, ErrRoomTaken
                }
        }

        now := time.Now()
        room := &Room{
                Code:     assignedCode,
                State:    RoomWaiting,
                HostConn: hostConn,
                Created:  now,
                Expires:  now.Add(ttl),
        }
        rm.rooms[assignedCode] = room
        return assignedCode, room, nil
}

// JoinClient looks up a room by code and attaches the client to it.
// Generates a matchId and transitions to RoomMatched. MatchInfo is
// sent to BOTH host and client atomically inside this function, under
// the lock — this guarantees the host receives MatchInfo BEFORE any
// TunInfo that might be triggered by the client's UdpData (which the
// client starts sending immediately after receiving MatchInfo).
//
// Returns the matchId. The caller does NOT need to send MatchInfo —
// it's already done.
//
// Returns ErrRoomNotFound if the code doesn't exist, ErrRoomExpired if
// the room's TTL has elapsed, ErrProtocolError if the room is already
// matched or a peer's TCP write fails.
func (rm *RoomManager) JoinClient(code string, clientConn *PeerConn, ttl time.Duration) (uint32, error) {
        rm.mu.Lock()
        defer rm.mu.Unlock()

        room, exists := rm.rooms[code]
        if !exists {
                return 0, ErrRoomNotFound
        }

        if time.Now().After(room.Expires) {
                delete(rm.rooms, code)
                return 0, ErrRoomExpired
        }

        if room.State != RoomWaiting {
                return 0, ErrProtocolError
        }

        room.ClientConn = clientConn
        room.MatchId = rm.nextMatchId()
        room.State = RoomMatched
        // Extend TTL — give peers time to exchange UdpData and complete
        // the hole-punch.
        room.Expires = time.Now().Add(ttl)

        // Send MatchInfo to BOTH host and client atomically. This MUST
        // happen under the lock to prevent the race where the client sends
        // UdpData → server sends TunInfo to host → host receives TunInfo
        // before MatchInfo.
        matchInfo := EncodeMatchInfo(room.MatchId)
        if room.HostConn != nil {
                if _, err := room.HostConn.Conn.Write(matchInfo); err != nil {
                        // Host disconnected — clean up and fail.
                        delete(rm.rooms, code)
                        return 0, ErrProtocolError
                }
        }
        if _, err := clientConn.Conn.Write(matchInfo); err != nil {
                // Client disconnected — clean up and fail.
                delete(rm.rooms, code)
                return 0, ErrProtocolError
        }

        return room.MatchId, nil
}

// FindByMatchId returns the room associated with a matchId, or nil.
// Used by the UDP handler when a UdpData packet arrives.
func (rm *RoomManager) FindByMatchId(matchId uint32) *Room {
        rm.mu.Lock()
        defer rm.mu.Unlock()
        return rm.findByMatchIdLocked(matchId)
}

// RecordPeerUdpAddr is called by the UDP handler when a UdpData packet
// arrives. It records the public UDP endpoint of the sender and, if
// this is the first packet from this side, sends TunInfo to the OPPOSITE
// peer's TCP connection.
//
// Returns the TunInfo bytes that were sent (for logging) and the
// opposite peer's TCP addr. Returns ErrRoomNotFound if no room has
// this matchId, ErrProtocolError if the room is in an unexpected state.
//
// If the sender has already been recorded (duplicate UdpData — normal,
// peers send these every 50ms), this is a no-op returning nil.
func (rm *RoomManager) RecordPeerUdpAddr(matchId uint32, isClient bool, udpAddr string) ([]byte, *PeerConn, error) {
        rm.mu.Lock()
        defer rm.mu.Unlock()

        room := rm.findByMatchIdLocked(matchId)
        if room == nil {
                return nil, nil, ErrRoomNotFound
        }

        var sender, opposite *PeerConn
        var alreadySent *bool
        var addrToRecord *string
        if isClient {
                sender = room.ClientConn
                opposite = room.HostConn
                alreadySent = &room.ClientTunSent
                addrToRecord = &room.ClientUdpAddr
        } else {
                sender = room.HostConn
                opposite = room.ClientConn
                alreadySent = &room.HostTunSent
                addrToRecord = &room.HostUdpAddr
        }

        if sender == nil || opposite == nil {
                return nil, nil, ErrProtocolError
        }

        // Idempotent — if we already sent TunInfo for this side, no-op.
        // (Peers keep blasting UdpData every 50ms; we only forward once.)
        if *alreadySent {
                return nil, opposite, nil
        }

        *addrToRecord = udpAddr

        // Build & send TunInfo to the OPPOSITE peer.
        tunInfo := EncodeTunInfo(matchId, udpAddr)
        if _, err := opposite.Conn.Write(tunInfo); err != nil {
                return nil, nil, err
        }
        *alreadySent = true

        // If both sides have now been recorded, mark room as done and delete.
        // The TCP connections stay open — the peers' TCP handlers will close
        // them when the peer disconnects (or context cancels).
        if room.HostTunSent && room.ClientTunSent {
                room.State = RoomDone
                // Defer the delete — return first so the caller can log.
                //
                // We pass the matchId to the deletion goroutine so it can
                // verify the room hasn't been recycled (same 4-letter code
                // re-registered by a different host during the grace period).
                // Without this check, a delayed Delete could remove a new,
                // unrelated room that happened to get the same code.
                go func(code string, matchId uint32) {
                        time.Sleep(2 * time.Second) // grace period for late UdpData
                        rm.DeleteIfMatch(code, matchId)
                }(room.Code, room.MatchId)
        }

        return tunInfo, opposite, nil
}

// Delete removes a room by code. Called when:
//   - Both TunInfos have been sent (room is done, after a grace period)
//   - Either TCP connection closes before match completes
//   - TTL expires (via Cleanup)
func (rm *RoomManager) Delete(code string) {
        rm.mu.Lock()
        defer rm.mu.Unlock()
        delete(rm.rooms, code)
}

// DeleteIfMatch removes a room by code ONLY if its matchId matches.
// This prevents a delayed grace-period deletion from removing a new,
// unrelated room that was re-registered with the same 4-letter code
// during the 2-second grace period.
//
// Used by RecordPeerUdpAddr's deferred deletion goroutine.
func (rm *RoomManager) DeleteIfMatch(code string, matchId uint32) {
        rm.mu.Lock()
        defer rm.mu.Unlock()
        if r, ok := rm.rooms[code]; ok && r.MatchId == matchId {
                delete(rm.rooms, code)
        }
}

// DeleteByMatchId removes a room by matchId.
func (rm *RoomManager) DeleteByMatchId(matchId uint32) {
        rm.mu.Lock()
        defer rm.mu.Unlock()
        for code, r := range rm.rooms {
                if r.MatchId == matchId {
                        delete(rm.rooms, code)
                        return
                }
        }
}

// LookupByHostCode returns the room for a given host code, or nil.
// Used by the polling loop in the TCP host handler.
func (rm *RoomManager) LookupByHostCode(code string) *Room {
        rm.mu.Lock()
        defer rm.mu.Unlock()
        return rm.rooms[code]
}

// Cleanup removes all expired rooms. Should be called periodically
// (e.g., every 10s) by a goroutine.
func (rm *RoomManager) Cleanup() int {
        rm.mu.Lock()
        defer rm.mu.Unlock()
        now := time.Now()
        removed := 0
        for code, r := range rm.rooms {
                if now.After(r.Expires) {
                        delete(rm.rooms, code)
                        removed++
                }
        }
        return removed
}

// Stats returns the current room count (for observability).
func (rm *RoomManager) Stats() (waiting, matched int) {
        rm.mu.Lock()
        defer rm.mu.Unlock()
        for _, r := range rm.rooms {
                switch r.State {
                case RoomWaiting:
                        waiting++
                case RoomMatched:
                        matched++
                }
        }
        return
}

// findByMatchIdLocked is the lock-free inner lookup.
func (rm *RoomManager) findByMatchIdLocked(matchId uint32) *Room {
        for _, r := range rm.rooms {
                if r.MatchId == matchId && (r.State == RoomMatched || r.State == RoomDone) {
                        return r
                }
        }
        return nil
}

// ============================================================================
// Room code generation
// ============================================================================

// GenerateRoomCode returns a random 4-letter room code from the
// unambiguous alphabet (no I/O/0/1).
//
// rand is a function returning a non-negative int — abstracted so
// tests can inject a deterministic source.
func GenerateRoomCode(rand func() int) string {
        const alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
        buf := make([]byte, 4)
        for i := range buf {
                buf[i] = alphabet[rand()%len(alphabet)]
        }
        return string(buf)
}

// ============================================================================
// Errors
// ============================================================================

type RoomError struct {
        Code byte
        Msg  string
}

func (e *RoomError) Error() string { return e.Msg }

var (
        ErrRoomNotFound  = &RoomError{Code: 1, Msg: "room not found"}
        ErrRoomExpired   = &RoomError{Code: 2, Msg: "room expired"}
        ErrProtocolError = &RoomError{Code: 3, Msg: "protocol error"}
        ErrRoomTaken     = &RoomError{Code: 4, Msg: "room code already taken"}
)
