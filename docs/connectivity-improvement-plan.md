# Connectivity improvement plan (multi-stage)

Implementation plan for the improvement list in
`docs/connectivity-study.md` (findings F1–F11, scenarios S1–S8). Each
stage is one or two PRs, lands on `main` independently, and is verified
with `scripts/nettest.sh` (add `--with-relay` for any stage touching
relay paths). Stages are ordered so every one is shippable on its own;
later stages depend only on earlier stages where noted.

## Ground rules

- **Versioning:** one unified version (`src/common/version.hpp`). Any
  change that touches the relay wire protocol or the DLL message layer
  bumps `kAppVersion`. Stages 3, 5 (partial), 8 and 9 do; the rest
  don't (client-only behavior).
- **Verification:** `scripts/nettest.sh` after every stage;
  `scripts/spectest.sh` for stages touching the spectator path;
  `smoke_test_*` in `scripts/` for protocol-level units (there is an
  existing `smoke_test_relay_protocol.cpp` to extend). No CI — the
  local run is the gate.
- **Invariants (AGENTS.md):** session FSM stays on the session worker
  thread; the DLL network thread owns its `ENetHost*`; `frameStep()`
  ordering is load-bearing; ENet queues CONNECT once — every poll site
  routes it onward.
- **Windows-only caveats:** things observable only on real Windows
  (UDP port rebind races, NAT behavior) are marked **WIN-VERIFY** and
  get an explicit user test step instead of a nettest gate.

## Stage map

| Stage | Name | Fixes | Size | Protocol? | Depends on |
|---|---|---|---|---|---|
| 0 | Diagnostics baseline | F1 (measure) | S | no | — |
| 1 | Launch-gap fix (re-punch + gap shrink) | F1, F5, S1 | M | no | 0 |
| 2 | Hole-punch budget + semantics | F3, S3, S4 | M | no | — |
| 3 | Relay v2: endpoint refresh + graceful room | F4, F9, S7, S8 | L | yes (server+client) | — |
| 4 | Relay failover + infra | F7, S6 (partial) | S–M | no | — |
| 5 | Delay/rollback: real stats + adaptive | F6, S5 | M | no | 0 |
| 6 | NAT/CGNAT preflight (STUN) | F2 (warn), F10, diagnostics | M | no (uses existing relay) | 3 (optional) |
| 7 | Dead code + error-text pass | F10, F2 (messaging) | S | no | — |
| 8 | TCP relay data fallback | F2, S2 | XL | yes | 3 |
| 9 | Mid-match reconnect + resync | F8 | XL | yes | 1, 8 (partial) |
| 10 | IPv6 evaluation + dual-stack | F7, S6 | XL | yes | 8 (transport abstraction) |
| 11 | Test rig (continuous) | all | M (ongoing) | no | 0 |

S/M/L/XL = roughly 1/2–3/4–6/1+ week of focused work.

---

## Stage 0 — Diagnostics baseline

**Goal:** make every later stage's effect measurable, and turn the
generic "Initial connect timeout" into a diagnosable event.

Changes:

- `src/dll/netplay/network_thread.cpp`
  - Log the exact endpoint at connect time (already partially done) and
    a monotonically increasing "CONNECT attempt N" line per
    `enet_host_service` cycle while unconnected (cheap: only logs
    while `!connected_`).
  - Expose a `connect_stats()` snapshot (elapsed since start, packets
    sent via `peer_->...` if available, RTT once connected).
- `src/dll/entry/dll_main.cpp`
  - On initial-connect success: log `initial connect established
    (N ms after netplay::start, endpoint X:Y)`.
  - On initial-connect failure: log endpoint, elapsed, and whether the
    peer ever answered (`peer_ != nullptr` and any traffic vs none) →
    two distinct messages: "no response at all" (NAT/firewall likely)
    vs "connection dropped after establishing".
- `src/exe/session/session.cpp` + `src/exe/launcher/game_runner.cpp`
  - Timestamped log lines: `session: relay phase X completed (t=…ms
    since session start)` for each relay FSM transition (the relay
    client already logs — promote the phase transitions to one
    greppable line each), `session: deinit (t=…)`, `game_runner:
    CreateProcess (t=…)`.
- No behavior changes, no env gating (all once-per-session info lines).

Verification: `nettest.sh` + `nettest.sh --with-relay`; `watch-logs.sh`
shows a clean timeline `relay → handshake → deinit → launch → DLL
connect → established`.

Exit criteria: from two side-by-side runs, the full
deinit→established gap is readable from logs alone.

---

## Stage 1 — Launch-gap fix (the F1/F5 fix)

**Goal:** stop the host's inbound NAT mapping from dying between the
launcher handshake and the DLL's reconnect (scenario S1 — the top CGNAT
complaint).

Design (why this shape): the double process is inherent (the launcher
cannot hand its ENet state to the DLL process), and a *second* live
socket on the same port is not an option on Windows: with
`SO_REUSEADDR` an incoming datagram is delivered to the **first**
socket that bound the port, which would let the launcher's ENet host
steal the DLL's CONNECT. So the keepalive must live in the process that
will own the port.

Changes:

1. **DLL host-side re-punch** — `src/dll/netplay/network_thread.cpp`
   - While `isHost_ && !connected_`: send a 1-byte `0x00` NullMsg to
     `peerAddr_:peerPort_` every 50 ms for the first 3 s, then every
     2 s until connected (or the 60 s timeout). This is the host's
     outbound traffic to the joiner endpoint — exactly the flow whose
     NAT entry the joiner's CONNECT depends on (F5).
   - The joiner side needs no change: its ENet CONNECT retransmits are
     already outbound traffic that (re)creates its own mapping.
   - Inbound NullMsgs on the peer's side are 1-byte non-ENet packets
     that ENet drops silently — verify no error log spam (ENet's
     protocol handler returns on bad magic; check under
     `CASTER_LOG_*` verbosity).
2. **Shrink the gap** — `src/exe/launcher/game_runner.cpp`
   - Remove the legacy 1 s "release UDP port" sleep in
     `LaunchAfterHandshake` (UDP has no TIME_WAIT; the DLL's ENet host
     uses `SO_REUSEADDR` anyway). **WIN-VERIFY** on a real machine:
     host + joiner boot-to-chara-select with no timeout, twice.
3. **Timeout message split** (from Stage 0) — surface
   "no response at all" vs "dropped after establishing" in the stop
   reason shown to the user.

Files: `network_thread.cpp/hpp`, `dll_main.cpp` (message only),
`game_runner.cpp`.

Verification: `nettest.sh` (direct) + `--with-relay`; **WIN-VERIFY**
boot-to-chara-select. Stage 11's NAT rig (when ready) gets a
short-entry-timeout case targeting this stage.

Exit criteria: no new timeout in 20/20 nettest runs; on a CGNAT test
box (when available) the S1 failure rate drops; log shows the re-punch
burst starting < 1 s after `netplay::start`.

Risks: re-punch burst adds ~20 packets of 1 byte at boot — negligible.
The only real risk is the Windows dual-socket rule being *more*
permissive than documented on some systems — the WIN-VERIFY step covers
it (if the launcher's socket were still bound when the DLL binds, the
joiner would time out; it isn't, deinit happens before launch).

---

## Stage 2 — Hole-punch budget + consistent failure semantics

**Goal:** F3 — a punch that takes 12 s shouldn't kill the match; the
silent smart-host downgrade should be visible and bounded; "success"
should mean something.

Changes — `src/common/net/relay/relay_client.cpp`:

- `kHolePunchTimeoutMs` 10 s → 30 s.
- **Re-punch rounds:** on hole-punch timeout, don't `fail()` — restart
  the punch sub-phase (re-burst NullMsg + UdpData, reset the 30 s
  clock) up to 3 rounds before failing. Track `punch_round_` in the
  status text ("hole-punching, round 2/3").
- **Require a confirming probe:** remove the 500 ms "learned endpoint,
  no confirm needed" accept (`kLearnedConfirmMs` path). Keep
  `learn_peer_port` retargeting, but declare success only on a 1-byte
  NullMsg from the (possibly retargeted) endpoint, or after the final
  round expires (then fail, with the round count in the log).
- **Session semantics** — `src/exe/session/session.cpp`
  - `StartRelayHost`/`StartRelayJoin`: unchanged shape (they already
    fail on `RelayError`), but the error now carries the round count
    and a better suggestion (Stage 7 text).
  - `step_parallel_relay` (smart host): on `RelayError`, publish a
    `relay_status` field in `SessionSnapshot` (new: `RelayHealth` enum
    `{None, Active, Degraded, Unavailable}`) + status text
    "relay unavailable — waiting for direct connection only (5 min)",
    and give the direct-only wait a finite 5 min budget that fails with
    a clear error instead of 1 h silence.
- `src/exe/pages/waiting_for_peer.cpp` — render `relay_status`
  (badge + one-line hint when Unavailable).

Files: `relay_client.cpp/hpp`, `session.cpp/hpp`,
`waiting_for_peer.cpp`.

Verification: `nettest.sh --with-relay`; extend
`scripts/smoke_test_relay_protocol.cpp` (or a new
`smoke_test_relay_client_fsm.cpp` if the FSM is testable standalone —
it isn't fully, sockets are involved; smoke the pure parts: retry
backoff, round math, error labels). Manual: point one side at a
non-routable relay to force the smart-host downgrade path and check the
badge + 5 min cap.

Exit criteria: a punch taking up to ~90 s (3 rounds) still completes
when the network allows; smart-host downgrade is visible and bounded;
no regressions in `--with-relay`.

---

## Stage 3 — Relay v2: endpoint refresh + graceful room teardown

**Goal:** F4 (TunInfo captured once) + F9 (room dies on any blip).
Client and server change together; bump `kAppVersion`.

Server (`server/room.go`, `udp_listener.go`, `tcp_listener.go`,
`protocol.go`):

- **Endpoint refresh:** `RecordPeerUdpAddr` currently no-ops after the
  first record (`*alreadySent`). Change: if the sender's public
  endpoint *changed*, record the new one and send a fresh TunInfo to
  the opposite peer (repeat allowed; cap e.g. 10 refreshes per side to
  avoid flapping). Room stays `RoomMatched` (TTL-extended) until both
  sides' TunInfos have been sent **and** a short grace (the existing 2 s
  post-done grace covers it).
- **Half-open room teardown:** when one TCP closes, mark the room
  `HalfOpen` instead of deleting; the surviving peer can still receive
  the other side's (late) packets and can re-register the same code
  within a grace (60 s) — implement as: re-`RegisterHost`/`JoinClient`
  with the same code while a `HalfOpen` room exists reattaches instead
  of `ErrRoomTaken`/`ErrRoomNotFound` (send the peer a new MatchInfo +
  fresh TunInfo round). After grace, delete.
- **Distinct "host left" error:** new error code 5
  (`ErrHostLeft`/`ErrPeerLeft`), sent to the waiting peer when its
  counterpart's TCP closes *and* the room is being torn down
  (vs "room not found").
- **Server keepalive tolerance:** the client starts sending the 15 s
  0x00 keepalive also from `WaitingForTunInfo`/`HolePunching` (client
  change below), so `waitForConnClose` stays as-is (it already ignores
  0x00).

Client (`src/common/net/relay/relay_client.cpp`):

- **Second TunInfo:** `try_parse_server_msg` `TunInfo` case must update
  `peer_addr_` (and reset the punch round clock) when already in
  `HolePunching`, instead of being a no-op/ignored.
- **Keepalive expansion:** send the 15 s 0x00 in `WaitingForTunInfo`
  and `HolePunching` too (fixes the idle-TCP-during-punch drop, F9).
- **Rejoin on `ErrPeerLeft`:** treat it as retriable (it already is,
  via `is_retriable` — just make sure the retry re-joins with the same
  room code, which it does).

Protocol impact: no new message types (TunInfo is already
self-describing; error code 5 is additive). Bump `kAppVersion` for
client/server pairing sanity.

Verification: `nettest.sh --with-relay`; manual server runs
(`go run ./server/` locally, point `relays` config at `127.0.0.1:3939`):
  1. kill the host's TCP mid-punch → joiner gets "host left" (not
     "room not found") → host re-registers → they pair again.
  2. force an endpoint change (bind the client from a second port
     mid-flow, or a small script that replays UdpData from another
     port) → opposite peer receives a fresh TunInfo and completes.

Exit criteria: S7 ("Room not found" on a blip) is gone in the manual
scenario; endpoint drift mid-punch recovers; `--with-relay` green.

Risks: re-TunInfo flapping (mitigated by the refresh cap + only on
*changed* address); room state machine gains a state — keep the
`DeleteIfSame`/`DeleteIfMatch` guards as they are (they exist for the
re-registration race this stage widens).

---

## Stage 4 — Relay failover + relay infra

**Goal:** F7 — one dead relay shouldn't be a global outage.

Changes:

- `src/exe/session/relay_setup.cpp` / `relay_client.cpp`
  - Pass the **whole** `RelayList` (not `[0]`) into `RelayClientInit`.
  - Resolve all relay hosts upfront (`resolve_host` on each; keep the
    resolvable ones, log the rest).
  - On relay-level failure (`TcpConnectFailed`, `TcpTimeout`,
    `RelayError` from the server) rotate to the next relay *before*
    spending a retry-backoff round; keep per-relay attempt counts.
    `MaxRetriesExceeded` only after all relays have been tried.
  - Room code: host regenerates on retry only when the code was
    server-confirmed and then lost (existing behavior) — with relay
    rotation a re-register on a *new* relay uses the same code if it
    was confirmed there, otherwise regenerates (document this in the
    code).
- Config/GUI: `[network] relays` already supports multiple lines
  (`config.cpp:69`); verify `config_page.cpp` edits it (it should, per
  the config plumbing) — add a "test connection" button per relay if
  not present (cheap, uses the non-matching probe from Stage 7's
  server peek — or just a TCP-connect check to keep Stage 4
  server-free).
- Infra (ops, not code): stand up a second relay instance in another
  region with a static IP; add both to the default
  `kDefaultRelayList` (keep the duckdns one first for continuity).
  Client-side, the duckdns name stays in the list for users with a
  working DNS path.

Verification: `nettest.sh --with-relay` with `relays` config =
`"127.0.0.1:1\n<good relay>"` → session succeeds via the good one,
log shows the rotation. `smoke_test_relay_protocol.cpp` or new smoke
for the rotation bookkeeping (pure logic).

Exit criteria: a dead first relay costs < 6 s (one TCP timeout) and
the session proceeds on the second; user-visible behavior unchanged.

---

## Stage 5 — Delay/rollback from real stats + adaptive in-match

**Goal:** F6 — stop guessing; use what ENet already measures.

Changes:

- **Launcher suggestion** — `src/exe/session/session.cpp`
  `finish_ping_exchange()`:
  - Collect per-ping RTTs (already there), use **worst observed** (max
    of the 5, or 90th pct with more pings) instead of the running
    average for tier selection; bump the ping count 5 → 8 (cost:
    ~1–4 s on the joiner's path, bounded by the per-attempt timeout).
  - Add the ENet peer stats available at that point
    (`transport_.get_stats()`: `rtt_ms`, `jitter_ms`, `packet_loss_pct`)
    to the margin: `effective = max(rtt, rtt + 2*jitter)`; if
    `packet_loss_pct > 0`, raise the rollback tier by one (inputs
    recover, but the delay tier should cover the RTT growth that
    accompanies loss).
  - All-pings-lost → worst-case tier (today's 200 ms assumption is
    already conservative; keep, but log it).
- **DLL stats exposure** — `connector.hpp/network_thread.cpp`:
  `netplay::stats()` returning the ENet peer stats (RTT, variance,
  loss) — the data is already there in `get_stats()`, just not exposed
  above the transport.
- **Adaptive nudge** — `src/dll/netplay/manager.cpp` (or a small
  `delay_advisor` in `dll_main.cpp`):
  - Every 5 s while InGame: recompute the suggested delay from live
    stats. If suggested > current for 3 consecutive samples, raise
    `config.delay` (only up; never auto-lower — users may have chosen
    manually, and auto-lowering mid-match is worse than a slightly high
    delay). Cap at 8. Log each change; surface one overlay toast
    ("delay auto-adjusted to N").
  - Honor `manual_delay` (user set Ctrl+digit) — stop auto-nudging once
    the user touches it.
- **Ctrl+0..9 delay hotkeys** (backlog #13) — `keymapper.cpp`'s
  per-VK `GetAsyncKeyState` edge-detection mechanism is reusable (the
  digit VKs aren't mapped yet — add them); Ctrl+digit →
  `NetplayManager::setDelay(n)` + set a `manual_delay` flag.
- **Overlay ping/delay** (backlog #1/#2) — wire `netplay::stats()` +
  `config.delay` into the info overlay and playername overlay.
- **Input-stall behavior** (F8, partial — full fix is Stage 9):
  replace the 10 s hard stop in `dll_main.cpp` with: connected but no
  inputs → show "waiting for opponent…" overlay with an elapsed
  counter, keep the spin-lock, extend the stop to 60 s; disconnect
  detected → immediate "Opponent disconnected" (unchanged). A stalled
  but-alive peer no longer kills the match at 10 s.

Verification: `nettest.sh` with the existing network simulator
(`CASTER_SIM_LAG_MS/JITTER_MS/LOSS_PCT` env vars) at 20 ms/10 ms/2 %
and 60 ms/30 ms/8 % — the match completes, desync count 0, and the
log shows the expected delay tier per the formula (add the expected
tier to the test command line as a comment). `spectest.sh` unaffected.
Manual: Ctrl+0..9 changes delay live; overlay shows plausible numbers.

Exit criteria: on the simulated-bad run, no desync, delay adapts
monotonically up, 10 s "Timed out!" no longer fires while connected.

Risks: auto-delay raising mid-match changes both players' timing —
the *host* picks the initial value for both (existing model); make the
auto-nudge host-side-only for symmetry (the client's view of its own
RTT is half the picture). Document that.

---

## Stage 6 — NAT/CGNAT preflight (revive the dead STUN work)

**Goal:** F2/F10 — know *before* the punch what kind of network you're
on, warn the user, and pick the strategy. This is also the field
diagnostic that will eventually prove/disprove the F1 sizing.

Changes:

- `src/common/net/relay/` (new `nat_probe.cpp/hpp`, reusing
  `relay_protocol`'s unused `encode_stun_probe`/`decode_stun_reply`):
  - From the session (worker thread), before/parallel to the relay
    handshake: open a throwaway UDP socket, send 2 STUN probes to the
    relay's UDP port (1 byte each) from two different local ports, read
    the 8-byte replies (timeout 3 s each — the relay already answers
    any non-UdpData packet with the sender's public endpoint,
    `udp_listener.go:118`).
  - Classify:
    - both replies' public ports equal → **cone/EIM**;
    - different → **symmetric** (the CGNAT signature);
    - also compare the reply IP with `ip_discovery::get_public_ip()`
      (ipify): a mismatch is not conclusive (proxy/VPN), but "STUN
      works but the relay's *recorded* endpoint keeps changing during
      the punch" (Stage 3 refresh count) is a strong CGNAT indicator —
      report both signals.
  - Output: `NatType {Unknown, Cone, Symmetric}` + the two observed
    public endpoints, into the session snapshot (extend the existing
    `connection_type` plumbing, which currently only carries
    wired/wireless).
- `waiting_for_peer.cpp` / `play_page.cpp`:
  - Symmetric + local role = host → warning line: "you look like you're
    behind a strict NAT (CGNAT?) — direct connection may fail; if it
    does, ask your opponent to host instead."
  - Symmetric + joiner → lighter note ("your NAT may need the
    opponent to host; waiting…").
  - Cone → no warning (log only).
- Keep the probe **non-blocking and best-effort** (3 s budget, runs
  alongside the relay handshake; a failed probe = `Unknown`, never
  blocks the session).

Verification: manual on ≥2 different networks (home router vs mobile/
CGNAT if available); `smoke_test` for the classifier (feed canned
replies). No protocol change (STUN already exists server-side).

Exit criteria: home network classifies Cone; mobile/CGNAT box
classifies Symmetric; warning shows only in the right rows; zero
session-time cost (probe overlaps the relay handshake).

---

## Stage 7 — Dead code + error-text pass

**Goal:** F10/F2-messaging — remove landmines; make the remaining
errors honest.

Changes:

- Delete `relay_client::validate_room_code` + `RoomValidationResult`
  plumbing (never called; would consume a room if ever enabled —
  `session.hpp:66`, `waiting_for_peer.cpp:71`, snapshot field). If the
  GUI ever wants "is this room live?", the correct shape is a
  server-side non-matching **peek** (new relay message, Stage 3-style
  additive) — out of scope here, note in the commit.
- Remove or wire the unused `encode_stun_probe`/`decode_stun_reply` —
  Stage 6 wires them; if Stage 6 lands first, this is just cleanup of
  anything left unused after.
- `error_label`/`error_suggestion` rewrite (`relay_client.cpp:144`):
  - `HolePunchFailed`: drop "or maybe this is a skill issue"; explain
    CGNAT in one plain sentence; suggest (1) retry, (2) switch relay
    (Stage 4 makes that real), (3) switch network, (4) **have the
    opponent host** (Stage 6 makes it targeted).
  - `TcpConnectFailed`/`TcpTimeout`: mention relay failover state
    ("tried N relays").
  - `MatchInfoTimeout`: keep the 60 s framing, add "the host may have
    closed their room — ask them to re-host".
  - Initial-connect (DLL): use Stage 1's split messages
    ("no response at all" → NAT/firewall wording + host-swap hint).
- `ip_discovery.cpp`: add a second IP source fallback (ipify →
  `api.ip.sb` or the relay's own STUN reply from Stage 6 — the STUN
  reply *is* the public IP, so the STUN path can replace ipify for the
  UI display; keep ipify as fallback). Display-only, no behavior risk.

Verification: `nettest.sh` (unchanged behavior), compile + smoke for
the deleted paths (the `room_validation` removal touches
`waiting_for_peer.cpp` rendering — run the GUI by hand for 5 min).

Exit criteria: no references to removed symbols (build is the gate);
every user-facing connection error names a next step.

---

## Stage 8 — TCP relay data fallback (the F2 guarantee)

**Goal:** "cannot play" → "can play, slightly worse" when UDP hole-punch
is impossible (strict CGNAT pair, carrier UDP block). This is
CCCaster's `'T'` mode, which the protocol byte already anticipates.

Design:

- **Relay data plane** (`server/`): a TCP forward. When a match is in
  relay-UDP mode and the punch has failed (client tells the relay via a
  new TCP message `TcpFallback(matchId)` — or the relay itself detects
  "no TunInfo ever arrived" after grace), the relay opens the data path
  by **piping**: both peers' existing TCP signaling conns are upgraded —
  after the control handshake, the conn becomes a length-prefixed
  datagram stream `[u32 len][bytes]` carrying the *game message layer*
  (the DLL's serialized messages), not ENet frames (ENet is UDP-only;
  wrapping its frames over TCP buys nothing over framing our own
  messages, which are already the unit the DLL cares about).
  - Server is a dumb byte pump between the two conns (no per-packet
    state beyond the length framing), so a slow peer just backpressures
    — acceptable for 60 fps × ~10–30 bytes.
  - Both peers must be in the relay TCP mode for the match; if only one
    side can punch, the relay tells the other "peer is in TCP relay
    mode" and the punchable side also drops to TCP mode (symmetric
    requirement, simpler protocol; note it in the UI).
- **Client transport abstraction** (the real work):
  - Today `NetworkThread` owns the `ENetHost*` directly and the
    `connector` outbox/inbox speak serialized messages. Introduce
    `class GameTransport` (DLL-side) with two backends:
    `EnetBackend` (current path, unchanged) and
    `RelayTcpBackend` (socket to the relay, length-framed
    `MsgType` stream, same outbox/inbox queues — the `connector` facade
    stays the single face for the game thread; only the backend that
    fills/drains the queues changes).
  - Reliable-vs-unreliable mapping: ENet UNRELIABLE PlayerInputs →
    TCP relay mode has no loss, so everything is "reliable"; keep the
    flag for parity.
  - Switch trigger: after Stage 2's 3 punch rounds fail (or after
    Stage 6's preflight says both sides are symmetric — *future*
    optimization, not v1), the client tells the relay; both sides
    restart the DLL-side connection in TCP mode. The launcher-level
    handshake (version/name/ping/config) runs over the relay TCP
    channel too (move `session.cpp`'s handshake off the raw ENet
    `send_reliable` onto the same `GameTransport` abstraction at the
    launcher level, or — simpler v1 — run the handshake over the relay
    TCP before the game launches, i.e. the relay becomes a *session
    transport* as well). **Decision point:** v1 keeps the launcher
    handshake on ENet-only; TCP mode is selected *before* the launcher
    handshake (punch attempt first; on failure, both sides re-enter the
    launcher handshake over relay TCP). This avoids duplicating the
    handshake in two transports.
- **UI:** "playing via relay (higher ping)" badge in the info overlay +
  playername overlay; suggested delay gets +2 frames in relay mode
  (Stage 5 formula input).
- **Server capacity:** a relay in TCP mode carries match bytes
  (~60–180 kB/s per match at 60 fps) — fine for the current scale;
  note bandwidth monitoring in the ops checklist.

Protocol/version: new relay messages (`TcpFallback` request/ack,
possibly `PeerInTcpMode`), additive; `kAppVersion` bump; old clients +
new server must still work (server treats absence of the message as
UDP-only — it already does).

Verification:
- Local: `go run` server + two clients with **UDP blocked** to the
  peer (iptables `DROP` on the game port, or a second NIC) → match
  plays over relay TCP, badge shows, no desync (nettest can't do this
  yet — Stage 11 rig first for the repeatable version; manual for v1).
- `nettest.sh` (UDP path) unchanged; `--with-relay` unchanged.
- Mixed: old client + new server (server must not hang waiting for the
  new message — 10 s grace, then UDP-only as today).

Exit criteria: with direct UDP blocked, a match completes end-to-end
over the relay; added latency vs direct measured and logged (expect +
one relay hop); no desync across 30 min of simulated play.

Risks: this is the largest protocol surface in the plan — keep the
`GameTransport` abstraction thin and the ENet path byte-identical
(bisect any regression on it). Consider splitting into 8a (relay data
plane + manual "force relay mode" env flag) and 8b (auto-switch on
punch failure).

---

## Stage 9 — Mid-match reconnect + resync (F8)

**Goal:** a network blip becomes a pause + catch-up, not a lost match.

Prereq study (do first, ~1–2 days, no code):

- Measure the save-state size (`mem_dump.cpp` already dumps state) and
  transfer time at 100 Mbps uplink vs a 2 Mbps phone uplink → decide
  resync = full state (likely fine: MBAA state is small) or
  state+RNG.
- Confirm the spectator state-transfer primitives
  (`SpectateConfig` + `InitialGameState` + `RngState` + `BothInputs`,
  `spectate_client.cpp`) are sufficient for a *mid-match* resync or
  need a mid-match variant (they were designed for chara-select join —
  a mid-match resync must also carry the input history both sides need
  to resume prediction; check against `NUM_INPUTS = 30`).

Changes (after the study):

- `dll_main.cpp` / `manager.cpp`: on opponent DISCONNECT, don't
  `delayedStop` — enter `Reconnecting` state: pause the game
  (CC_SKIP_FRAMES=1, like PreInitial), show "reconnecting… (N s)"
  overlay, NetworkThread retries `enet_host_connect` with the Stage 1
  re-punch burst (host side) until connected or 60 s.
- On reconnect: the host (state authority) sends the resync bundle
  (InitialGameState + RngState + last 2×NUM_INPUTS inputs); the joiner
  loads state, reseeds RNG, resumes with delay = suggested.
- The Stage 1 launcher keepalive is gone by mid-match (the game has
  long since booted) — the re-punch comes from the DLL socket, which is
  the right process. NAT entries for the *new* ENet peer may be stale;
  the re-punch burst handles the host inbound side; the joiner's
  outbound re-CONNECT handles its own side.
- If reconnect fails after 60 s → the old "Opponent disconnected" stop
  (now honest: we tried).

Verification: manual + Stage 11 rig — drop the UDP path mid-match
(iptables) for 5 s / 30 s / 120 s windows; 5 s and 30 s recover with
zero desync (SyncHash check), 120 s gives the disconnect message.
`nettest.sh` + `spectest.sh` regression (spectators connect mid-match
already — the resync path and the spectator path share primitives;
check they don't interfere: a reconnecting opponent + a spectator on
the same host).

Exit criteria: 5 s and 30 s blips invisible to the score (no desync,
no lost rounds), 120 s blip = clean disconnect message; spectator
still works during a reconnect.

Risks: resync correctness is the hard part (RNG + input history
alignment) — the SyncHash at `CASTER_SYNCHASH_INTERVAL=30` is the
safety net: a bad resync desyncs fast and is detectable, and the
rollback engine already handles divergence; worst case is the current
behavior.

---

## Stage 10 — IPv6 evaluation + dual-stack (F7/S6)

**Goal:** IPv6-only networks can play (S6).

Evaluation first (this stage's real start):

- **ENet is the blocker:** ENet 1.3's `ENetAddress` is IPv4-only
  (`u32 host`). Options: (a) ENet 2.x (IPv6-capable, but a major
  rewrite — API changes to `enet_host_create`, events, intercept
  callback shape); (b) keep ENet 1.3 for direct IPv4 and gate IPv6 on a
  relay-only path (Stage 8's relay TCP works over any IP family — the
  signaling relay is the natural IPv6 entry point, since the relay
  server itself can be dual-stack).
- Recommendation from the study: **v1 = dual-stack relay + IPv6
  signaling + relay-TCP play for IPv6 users; direct IPv6 only if ENet
  2.x lands stable.** This keeps Stage 10 scoped and shippable.

Changes (v1 scope):

- Server: `net.Listen("tcp"/"udp", ":3939")` is already dual-stack;
  verify it binds `[::]` with `IPV6_V6ONLY=0` semantics on the target
  host; room-code flow unchanged.
- Client: `resolve_host` (relay_client.cpp:71) already uses
  `getaddrinfo` — extend to return an **address list** (AF_INET6 then
  AF_INET) and connect in order (happy-eyeballs-lite: try v6 with a
  1.5 s timeout, fall back v4). TCP signaling socket + UDP UdpData
  socket become family-generic (`AF_INET6` where applicable — the
  relay client is currently hardwired `sockaddr_in`; needs a
  `sockaddr_storage` pass).
- `ip_discovery` + Stage 6 STUN: v6-aware (STUN reply format already
  8-byte v4; add a 16-byte v6 variant to the relay's STUN handler).
- Direct IPv6 (later): ENet 2.x port **or** a documented "IPv6 users
  play via relay" limitation in the error text.

Verification: a v6-only test (mobile AP, or a local `af_inet6`
loopback relay) — join works; v4 regression nettest green.

Exit criteria: IPv6-only client + IPv4 client play (via relay TCP);
v4-only users unaffected.

Risks: `sockaddr_storage` plumbing touches every socket in
`relay_client.cpp` — keep it behind a small `addr` wrapper to keep the
diff reviewable; ENet 2.x stays out of scope unless evaluation says the
direct path is demanded.

---

## Stage 11 — Test rig (continuous; start its pieces early)

**Goal:** make S1–S8 reproducible headlessly so stages 1/3/8/9 have
real gates instead of "manual when possible".

Pieces (each independently useful):

1. **nettest.sh simulator presets** (week 1, ~0.5 d):
   `nettest.sh --sim="lag=30,jitter=15,loss=5"` plumbs the existing
   `CASTER_SIM_LAG_MS/_JITTER_MS/_LOSS_PCT/_SIM_SEED` env vars into
   both instances and asserts 0 desync at a table of presets
   (5/1/1, 30/15/5, 80/40/10, 150/60/20). This is the bad-connection
   axis and needs no new infra.
2. **NAT emulator** (week 2–3, ~2 d): a small Go program (or
   `iptables`+`socat` if simpler) that presents two "public" UDP ports
   and forwards between internal sockets with (a) per-flow entries
   (symmetric mode) vs shared port (cone mode) and (b) configurable
   entry TTL (the CGNAT-short-timeout knob). nettest runs both MBAA
   instances against their own NAT view, relay included. This is the
   launch-gap (Stage 1) and punch (Stage 2/3) gate: e.g. "TTL=5 s,
   symmetric, both sides" must pass after Stage 1+2 and is the
   regression test for S1/S2/S4/S8.
3. **UDP-block scenario:** the NAT emulator in "drop UDP to peer" mode
   → Stage 8's TCP fallback gate.
4. **Field data collection (passive):** Stage 0's timeline logs give
   users a greppable connection story; add a `scripts/summarize-log.sh`
   that reduces a `{host,join}_debug.log` pair to the phase timings
   (so user bug reports are one-line-analyzable, and we can measure
   real CGNAT TTLs from the gap sizes over time).

Verification: the rig itself is the deliverable; each piece lands with
a scripted scenario that fails today (documented) and passes after the
corresponding stage.

---

## Explicit non-goals (for this plan)

- ENet 2.x migration as a standalone project (only via Stage 10
  evaluation).
- 2v2, matchmaking/lobby, room listing — out of scope (architecture
  changes, different study).
- Server clustering/HA beyond "second relay" (Stage 4 infra).
- Moving the handshake into the DLL process (would change the
  launcher/DLL split — revisit only if Stage 1 proves the double
  connection itself is the dominant residual failure).

## Suggested sequencing in calendar terms

1. **Week 1:** Stage 0 + Stage 1 (one PR each) + Stage 11.1 (sim
   presets) + Stage 7 (dead code) — the CGNAT S1 fix ships with
   measurement.
2. **Week 2:** Stage 2 + Stage 3 (relay v2, version bump) — punch
   robustness; `--with-relay` + manual server scenarios.
3. **Week 3:** Stage 4 (failover + second relay ops) + Stage 5 (stats,
   adaptive delay, hotkeys, overlay, stall behavior).
4. **Week 4:** Stage 6 (NAT preflight) + Stage 11.2 (NAT emulator) —
   preflight gives the field data; the rig turns the manual CGNAT tests
   into regressions.
5. **Weeks 5–7:** Stage 8 (TCP relay fallback) — split 8a/8b as
   decided; the UDP-block scenario gate lands with it.
6. **Weeks 8–10:** Stage 9 (reconnect/resync) after its prereq study.
7. **Backlog:** Stage 10 (IPv6) once the relay TCP mode (8) is in
   production and the IPv6-only share of complaints is confirmed from
   Stage 6 field data.
