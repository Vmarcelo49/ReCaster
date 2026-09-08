# zzcaster relay protocol

Signaling protocol used by the ReCaster client (`src/common/net/relay/`)
and the relay server (`server/`) for NAT traversal. The relay is
**signaling-only**: it matches a host and a client by room code, learns
each peer's public UDP endpoint, tells each peer the other's endpoint,
and gets out of the way. Game traffic flows peer-to-peer via ENet.

- Default relay: `zzcaster.duckdns.org:3939`
- Game UDP port (informational, sent in HostRegister): `46318`
- All integers are **little-endian** (CCCaster-compatible).
- TCP is used for all signaling; UDP is used only for UdpData and the
  STUN probe.

## Room codes

- 4 characters, uppercase `A-Z` / `0-9` on the client side.
- The host generates the code locally and sends it in HostRegister;
  the server echoes it back in Hosted (and generates one if the host
  sent an empty code).
- Codes the **server** generates use the unambiguous alphabet
  `ABCDEFGHJKLMNPQRSTUVWXYZ23456789` (no I/O/0/1).
- A room lives for the configured TTL (default 60 s) and is deleted
  when both TunInfos have been sent (after a 2 s grace period), when
  either peer's TCP connection closes, or on TTL expiry.

## Messages

### TCP, peer → relay (initial, one per connection)

| Message | Wire format | Notes |
|---|---|---|
| HostRegister | `[u8 type 'T'\|'U'][u16 LE port][u8 code_len][code]` | `code_len` 0–4; 0 = server assigns a code |
| ClientJoin | `[u8 type 'T'\|'U'][u8 code_len=4][code]` | exactly 4-char code |

The two are disambiguated by total length (`4 + code_len` vs
`2 + code_len`), mirroring the original CCCaster server.

### TCP, relay → peer

| Message | Wire format | Notes |
|---|---|---|
| Hosted | `"Hosted" [code 4 bytes]` (10 B) | reply to HostRegister |
| MatchInfo | `"MatchInfo" [u32 LE matchId]` (13 B) | sent to **both** peers, atomically, when the match is made |
| TunInfo | `"TunInfo" [u32 LE matchId] [addr][NUL]` | sent to the **opposite** peer; `addr` = peer's public `ip:port` |
| Error | `"Error" [u8 code][message]` | no length prefix, no NUL |

Error codes:

| Code | Meaning |
|---|---|
| 1 | Room not found (ClientJoin for an unknown code) |
| 2 | Room expired (TTL elapsed) |
| 3 | Protocol error (malformed message / unexpected state) |
| 4 | Room code already taken |

### TCP keepalive

The C++ host sends a **1-byte `0x00`** every 15 s while waiting for a
client, to keep NATs from dropping the idle signaling connection. The
relay treats it as a keepalive and sends nothing back. The protocol has
no other peer→relay messages after the initial one.

### UDP

| Message | Wire format | Notes |
|---|---|---|
| UdpData | `[u8 isClient 0\|1][u32 LE matchId]` (5 B) | both peers → relay, every 50 ms, from after MatchInfo until the hole-punch completes |
| STUN probe | `[u8 'X']` (1 B) | client → relay; any packet that isn't UdpData is treated as a probe |
| STUN reply | `[4 B IPv4 BE][2 B port BE][2 B padding]` (8 B) | relay → probe sender: the sender's public endpoint as seen by the relay |

The relay records the source address of the **first** UdpData from each
side and forwards it as TunInfo to the opposite peer; later UdpData
packets (peers keep sending every 50 ms) are ignored.

## Flow

```
host                          relay                         client
  │  HostRegister(code,port)    │                             │
  │─────────────────────────────>│                            │
  │  Hosted(code)               │                             │
  │<─────────────────────────────│                            │
  │  (0x00 keepalive every 15s) │                             │
  │  ...                        │                             │
  │                             │  ClientJoin(code)          │
  │                             │<───────────────────────────│
  │  MatchInfo(matchId)         │  MatchInfo(matchId)        │
  │<─────────────────────────────│────────────────────────────│
  │  UdpData(0,matchId) 50ms    │  UdpData(1,matchId) 50ms   │
  │─────────────────────────────>│<───────────────────────────│
  │                             │ (learns both public UDP endpoints)
  │  TunInfo(matchId, client_addr)                        │
  │<─────────────────────────────│                          │
  │                             │  TunInfo(matchId, host_addr)
  │                             │───────────────────────────│
  │  NullMsg hole-punch probes (50 ms) to the learned peer addr
  │<───────────────────────────────────────────────────────>│
  │  ENet CONNECT + game traffic (peer-to-peer, done with relay)
```

Once both TunInfos are sent, the room is deleted after a 2 s grace
period (late UdpData is expected and harmless).

## Compatibility notes

- Superset of the CCCaster protocol (which matched on the host's public
  `ip:port` string instead of a room code); same magic headers and
  endianness, so CCCaster-style probes still parse.
- The relay does **not** relay game packets and cannot authenticate UDP
  senders: the `matchId` is a bearer token (see `server/README.md`,
  Limitations).
