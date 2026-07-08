# zzcaster-relay

A signaling-only NAT traversal server for zzcaster. Helps two peers
behind NAT find each other's public UDP endpoint so they can hole-punch
a direct ENet connection.

**Does NOT relay game packets.** Game traffic flows peer-to-peer. The
relay only forwards endpoint information over TCP, then gets out of the
way. Bandwidth cost: ~1 KB/s per active match (just the 50ms UdpData
packets from both peers, for ~10s per match).

## Quick start (Docker)

```bash
cd server
docker compose up -d
```

This builds and runs the relay on a VPS, exposing:
- `3939/tcp` — signaling (HostRegister, ClientJoin, MatchInfo, TunInfo, Error)
- `3939/udp` — UdpData (hole-punch endpoint discovery) + STUN probe

Check it's running:
```bash
docker compose logs -f
# Should see: "TCP listening on :3939" and "UDP listening on :3939"
```

Smoke test from another machine:
```bash
# Should connect and stay open (waiting for HostRegister)
nc -v your.vps.ip 3939
```

## Quick start (no Docker)

Requires Go 1.22+.

```bash
cd server
go build -o zzcaster-relay .
./zzcaster-relay -addr :3939 -ttl 60s -log info
```

## Configuration

All flags can be set via command-line or environment variable (env
var wins if both are set).

| Flag    | Env var           | Default  | Description                                |
|---------|-------------------|----------|--------------------------------------------|
| `-addr` | `ZZ_RELAY_ADDR`   | `:3939`  | TCP+UDP listen address                     |
| `-ttl`  | `ZZ_RELAY_TTL`    | `60s`    | Room TTL — rooms expire after this         |
| `-log`  | `ZZ_LOG_LEVEL`    | `info`   | Log level: `info` or `error`               |

## Deploying to a VPS

### 1. Pick a VPS

Any cheap VPS with a public IPv4 address works. Hetzner CX11 ($4/mo),
DigitalOcean droplet ($4/mo), Vultr, Linode — all fine. The relay is
CPU-light and uses ~50MB RAM.

### 2. Open firewall ports

```bash
# UFW example
sudo ufw allow 3939/tcp
sudo ufw allow 3939/udp
```

Both TCP and UDP must be open on the same port (3939).

### 3. Install Docker

```bash
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER
# log out and back in for group change to take effect
```

### 4. Clone and run

```bash
git clone https://github.com/Vmarcelo49/zzcaster.git
cd zzcaster/server
docker compose up -d
```

### 5. Verify

From a different machine:
```bash
# Should connect and stay open
nc -v your.vps.ip 3939

# Send a STUN probe (1 byte) — should get 8 bytes back
echo -n "X" | nc -u your.vps.ip 3939 | xxd
```

### 6. Note the IP for the client config

The zzcaster client (in future Slice 2+) will read the relay address
from `config.ini`:

```ini
[Network]
RelayServer=your.vps.ip:3939
```

If `RelayServer` is empty, the client falls back to a hardcoded default
(defined in `src/net/relay_config.zig` — to be written in Slice 2).

## Wire protocol

See [`docs/nat-traversal-protocol.md`](../docs/nat-traversal-protocol.md)
for the full spec. Short version:

```
Host → TCP → relay:  HostRegister(code, port)
Host ← TCP ← relay:  Hosted(code)
Client → TCP → relay: ClientJoin(code)
Both ← TCP ← relay:  MatchInfo(matchId)
Both → UDP → relay:  UdpData(isClient, matchId)  [every 50ms]
Both ← TCP ← relay:  TunInfo(matchId, peer_addr)  [once per side]
Both → UDP → peer:   NullMsg  [hole-punch probes]
Both ← UDP ← peer:   ENet game traffic
```

## File layout

```
server/
├── go.mod                # Go module definition (no external deps)
├── main.go               # Entry point — flag parsing, starts TCP+UDP listeners
├── protocol.go           # Wire format encode/decode (the contract)
├── room.go               # Room struct + RoomManager (in-memory state)
├── tcp_listener.go       # TCP accept loop + per-conn handlers
├── udp_listener.go       # UDP recvfrom loop — UdpData + STUN probe
├── Dockerfile            # Multi-stage Go → scratch (~10MB image)
├── docker-compose.yml    # Single-service deploy with port mapping
└── README.md             # This file
```

## Testing

(Skeleton phase — tests will be added in Slice 1.5. For now, manual
testing only.)

### Manual test with Python probe

Adapt `/home/z/my-project/scripts/probe_cccaster.py` (from the CCCaster
analysis) to use the zzcaster protocol:

```python
# Host side:
import socket, struct
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(("your.vps.ip", 3939))
# Send HostRegister: 'U' + port LE + code_len + "ABCD"
s.sendall(b"U" + struct.pack("<H", 46318) + b"\x04ABCD")
print(s.recv(64))  # Should be b"HostedABCD"
# Wait for MatchInfo (will block — no client joined)
# ...

# Client side (different terminal/machine):
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(("your.vps.ip", 3939))
# Send ClientJoin: 'U' + code_len + "ABCD"
s.sendall(b"U\x04ABCD")
print(s.recv(64))  # Should be b"MatchInfo" + matchId (4 bytes LE)
```

## Limitations

- **No persistence.** Rooms are in-memory only. Server restart = all
  pending matches lost. Peers reconnect and re-register. (Acceptable
  for a signaling server.)
- **No authentication.** Anyone who knows the relay address can host
  or join rooms. Room codes are 4 chars from a 32-char alphabet =
  ~1M combinations — guessable in theory, but the relay doesn't care
  since it doesn't relay game traffic. If abuse becomes a problem,
  add per-IP rate limiting.
- **No IPv6.** STUN probe rejects IPv6 senders. UDP UdpData works
  with IPv6 if both peers have IPv6, but the relay only stores the
  string form of the address — no family tracking. Fix in a future
  version if needed.
- **No metrics / observability.** Just `log.Printf`. If you want
  Prometheus metrics, add a `/metrics` HTTP endpoint in `main.go`.
- **Symmetric NAT users can't host.** This is a fundamental limit
  of UDP hole-punching, not a relay bug. The zzcaster client should
  detect this via STUN probe and warn the user.

## License

Same as the rest of zzcaster (public domain, no warranty).
