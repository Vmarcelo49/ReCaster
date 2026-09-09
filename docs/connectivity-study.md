# Initial online connectivity — connections & robustness study

Study of how the initial online connection is established and where it is
fragile, driven by user reports that ReCaster feels unstable/unpredictable
on CGNAT and on bad (lossy/high-latency) connections.

Read with `docs/nat-traversal-protocol.md` (wire protocol) and
`docs/port-status.md` (netplay status). All code references are to the
current tree.

---

## 1. How the initial connection works today

The connection is established in **two passes in two different processes**,
plus a TCP signaling phase in between:

```
┌──────────────────────── launcher (caster.exe) ───────────────────────┐
│ 1. Relay signaling (TCP :3939)   relay_client.cpp                    │
│    HostRegister/ClientJoin → Hosted → MatchInfo → UdpData(50ms)     │
│    → TunInfo (peer's public ip:port) → NullMsg hole-punch (50ms)    │
│ 2. Launcher-level ENet handshake  session.cpp                        │
│    CONNECT → Version(5s) → Name(5s) → Ping×5 (833ms/try)            │
│    → delay/rollback suggestion (host) → Config/Confirm               │
│ 3. deinit_async() — the launcher's ENet transport is DESTROYED       │
│    (the UDP socket is closed)                                        │
└───────────────────────────────────────────────────────────────────────┘
        1 s legacy sleep → CreateProcess(MBAA.exe) → inject hook.dll
┌──────────────────────── game (hook.dll) ──────────────────────────────┐
│ 4. NetworkThread::start() — a BRAND-NEW ENet host on the same local   │
│    port, re-connecting to the same peer endpoint (network_thread.cpp) │
│ 5. 60 s initial-connect timeout (dll_main.cpp: INITIAL_CONNECT_TIMEOUT)│
│    → on failure the game exits: "Initial connect timeout"             │
└───────────────────────────────────────────────────────────────────────┘
```

Key facts:

- The relay is **signaling-only** (`server/`): it matches room codes, learns
  each peer's public UDP endpoint from the *first* `UdpData` packet, tells
  each peer the other's endpoint, and then the room is deleted. Game traffic
  is peer-to-peer ENet, never routed through the relay.
- Both passes rely on the **NAT mappings created in pass 1 still being
  alive** when pass 4 sends its ENet CONNECT. Nothing keeps them alive in
  between.
- The host listens on a fixed port (default 46318); the joiner binds an
  ephemeral port (`EnetTransport::bind_only(0)`).
- ENet 1.3.18, 2 channels, unlimited bandwidth; peers configured with
  connect timeout 0 (retransmit), 30 s idle, 120 s global
  (`enet_transport.cpp:91,146`).

### Relay timeouts (relay_client.cpp)

| Phase | Timeout | Notes |
|---|---|---|
| TCP connect to relay | 5 s | |
| Hosted (room confirm) | 5 s | |
| MatchInfo (opponent registers) | 60 s | = server room TTL (60 s) |
| TunInfo (peer endpoint learned) | 10 s | needs ≥1 UdpData from the peer |
| Hole-punch | 10 s | 1-byte NullMsg every 50 ms both ways |
| Symmetric-NAT retarget confirm | 500 ms | then accepted "best-effort" |
| Retry budget | 2 min total, ≤10 attempts | exponential backoff 1→5 s |

### Session timeouts (session.cpp)

Direct connect 30 s; Version/Name 5 s each; ping 5 attempts × 833 ms;
host waits for config confirm 30 s; listening 1 h.

---

## 2. Findings (ranked by likelihood of matching the complaints)

### F1 — The "launch gap": NAT mappings age out between the two passes
**Likelihood: high. This is the prime CGNAT suspect.**

After the launcher handshake completes, the session is deinited (UDP
socket closed) and *then* the game is launched
(`main_menu.cpp` KillingTraining→Deinitsession→LaunchingNetplay;
`cli.cpp:91`; `game_runner.cpp` "sleeping 1s to release UDP port").
The DLL then creates a **new** ENet host on the same local port and
re-connects (`network_thread.cpp:76-125`).

Whether that CONNECT reaches the host depends on the host-side NAT entry
for the flow `host:46318 → joiner_public_endpoint` still existing. That
entry was last refreshed by the host's relay-client NullMsg probes, which
stop at `deinit`. The gap then contains: training-game kill (when
coming from training) + deinit + 1 s legacy sleep + `CreateProcess` +
game boot + DLL init — seconds of real time.

The launcher *does* keep the mappings fresh while it owns the socket —
the 2 s ENet PING heartbeat (`maybe_heartbeat`, session.cpp:541) runs in
every state including `WaitingConfirmation`/`Launching` — so the mapping
is fresh exactly up to the deinit moment. The vulnerable window is purely
**deinit → DLL first CONNECT ACK**:

- Home router NAT: UDP entry timeout typically 60–300 s → works.
- **CGNAT: entry timeouts are often 2–30 s (carrier-dependent)** → the
  entry frequently expires in the gap → the joiner's CONNECT is silently
  dropped by the host's carrier NAT → the joiner burns its 60 s
  initial-connect timeout → game exits with "Initial connect timeout —
  peer never connected". Non-deterministic per boot → exactly the
  "unstable and unpredictable" report.
- The joiner side is self-healing (its CONNECT is outbound, so it
  re-creates its own mapping); **the host's inbound mapping is the fragile
  side**, so it is the *host's* CGNAT that hurts, regardless of which
  user experiences the failure.
- Nothing in the DLL re-punches: it only re-issues ENet CONNECTs to an
  endpoint that may no longer be reachable, and it has no way to know
  the difference between "peer is slow" and "my mapping died".

### F2 — No data-relay fallback: hole-punch is all-or-nothing
**Likelihood: high for strict CGNAT / UDP-filtered networks.**

The relay protocol still carries the `'T'` (TCP) type byte for
CCCaster compatibility, but only `'U'` is implemented. If the hole-punch
fails — carrier blocks inbound UDP, strict symmetric CGNAT pair (see
§3 matrix), or the NAT simply won't forward the punch — the match dies
with "Hole-punch failed (NAT too restrictive?)". CCCaster had a TCP
relay mode for exactly this case; ReCaster does not. There is no other
path to play, and the error text ("...or maybe this is a skill issue")
does not help.

### F3 — Hole-punch budget is short and failure semantics are inconsistent

- `kHolePunchTimeoutMs = 10 s`. On a saturated mobile uplink the
  *signaling* (TCP) is fine, but 200 NullMsg probes at 50 ms cadence plus
  the peer's ENet CONNECT have to all land while NAT windows are fresh.
  Ten seconds is a coin flip on a bad connection.
- The same timeout has **different consequences per entry path**:
  - `StartRelayHost`/`StartRelayJoin` (the actual room-code paths):
    relay error → session `Failed`, match over.
  - `StartSmartHost` (`Listening` path): relay error → *silently* drops
    the relay and keeps waiting for a direct connection that — on CGNAT —
    can never happen, for up to an hour.
  So one user gets an error, the other stares at "Listening for direct
  connection..." with no idea that the relay gave up.
- The 500 ms "learned endpoint, no confirm needed" acceptance
  (`kLearnedConfirmMs`) can mark the punch "succeeded" without any
  confirming packet from the peer — on drifting CGNAT endpoints the
  joiner then CONNECTs to a port nobody is listening from.

### F4 — TunInfo is captured once; CGNAT endpoints drift

The relay records the source of the **first** `UdpData` from each side and
ignores all later ones (`room.go:242-244`). CGNATs routinely assign a
different public port per flow and re-assign ports after entries expire.
Consequences:

- The endpoint in TunInfo can be stale before the first probe even lands;
  the `learn_peer_port` retarget logic (relay_client.cpp:459) recovers
  *one* direction and only while the peer is still probing.
- There is no re-TunInfo / endpoint-refresh mechanism after the room is
  deleted (which happens 2 s after both TunInfos are sent).
- Combined with F1, any re-attempt in the DLL pass uses endpoints that
  were true ~10–60 s ago.

### F5 — The host's inbound NAT entry is the single fragile state

Even when the punch succeeds in the launcher pass, the host's carrier
NAT must keep forwarding `joiner_endpoint → host:46318` across the
launch gap (F1). Only outbound traffic from the host to the joiner
refreshes that entry; after `deinit` there is no such traffic. (See F1 —
listed separately because it is the actionable root cause.)

### F6 — Delay/rollback suggestion is a weak signal, and never revisited

- Suggestion uses 5 pings (usually < 1 s of data; up to ~4 s of pure
  timeouts on a dead link) and only the **average RTT**
  (`session.cpp:963-1023`). ENet already tracks RTT, RTT variance and
  packet loss per peer (`get_stats()`) but the suggestion ignores loss and
  jitter entirely.
- If all 5 pings time out, the code assumes 200 ms ("bad connection") —
  i.e. total loss is treated as *moderate* badness.
- The suggestion is computed once, on the launcher connection, and never
  re-evaluated. A connection that degrades mid-session (mobile) keeps
  whatever delay was chosen at join time. CCCaster/GGPO-style in-match
  delay adjustment (Ctrl+0..9) is already on the backlog
  (`docs/future-improvements.md` #13) but not implemented.
- The host decides for both sides; a client whose own uplink is the bad
  one cannot correct the setting.

### F7 — Single relay, IPv4-only, dynamic-DNS endpoint

- `relay_setup.cpp:32` uses `relay_list_[0]`; the multi-relay config
  exists but **failover is never implemented**. DNS failure, relay
  outage, or the duckdns host flapping = everyone fails; retries just
  hammer the same dead server for 2 minutes.
- Everything is IPv4-only (`resolve_host` AF_INET, ENet via IPv4, relay
  `To4()` check). IPv6-only networks (common on mobile, some residential)
  can't reach the relay at all and get "Relay unreachable" with no
  alternative.
- No relay health check, no fallback IP list, no second region.

### F8 — In-match drops are terminal; "Timed out!" after 10 s

- If the ENet connection drops mid-match (carrier NAT re-key, brief
  outage, phone network switch), the match ends
  ("Opponent disconnected"). There is no reconnection or resync path at
  any layer.
- When remote inputs stall (heavy loss/burst), the game thread spin-locks
  and gives up after 10 s (`dll_main.cpp` MAX_WAIT_INPUTS) → "Timed
  out!" → game exits. Inputs are UNRELIABLE+UNSEQUENCED with a
  `NUM_INPUTS = 30` (500 ms) recovery window, which is good, but a loss
  burst exceeding the window or a stalled reliable channel ends the match
  instead of pausing/resuming.

### F9 — Room lifecycle is fragile on flaky TCP

- The server deletes a room the moment **either** TCP connection closes
  (`tcp_listener.go:206,246`). A momentary network blip on the host
  (or a phone locking the radio) kills the room while the joiner is
  still trying to join → "Room not found" with no explanation on either
  side.
- Host waiting timeout (60 s) == room TTL (60 s): a joiner who arrives at
  t≈60 s is racing the expiry.
- The 15 s 0x00 TCP keepalive is only sent from `WaitingForMatchInfo`
  (in practice the host's up-to-60 s "waiting for opponent" phase). After
  `MatchInfo`, **no TCP traffic flows at all** (peers only send UDP
  UdpData) until the room is deleted — so if a CGNAT blackholes the now
  idle signaling TCP connection mid-punch, the server's TunInfo push
  silently dies and the client only finds out via the 10 s
  `TunInfoTimeout`, then restarts from scratch (F3). The client should
  keepalive in `WaitingForTunInfo`/`HolePunching` too.

### F10 — Dead code and landmines

- `relay_client::validate_room_code` is **never called** (the UI renders
  `snap.room_validation`, which is never set). Worse: if it were wired
  in, its probe sends a real `ClientJoin`, which the relay *pairs into
  the room* and then deletes when the probe closes — i.e. "checking" a
  room code would destroy the host's room. If this code is ever
  re-enabled, the server needs a non-matching "peek".
- The STUN probe/reply machinery (`relay_protocol.cpp:106`, server
  `handleStunProbe`) is implemented but unused — NAT-type detection was
  clearly intended and never finished.
- `ip_discovery::get_public_ip` calls `https://api.ipify.org` (WinINet)
  — a third-party dependency on the UI path; failure is silent (empty
  string). Display-only, but another single point of "unpredictable"
  behavior.

### F11 — Handshake margins are okay but thin on bad connections

Version/Name phases use 5 s each. ENet reliable retransmission makes this
usually fine, but the initial RTT estimate on a fresh peer is 200 ms, so
retransmits are dense early — still, a congested first RTT of a couple of
seconds leaves little margin. The ping exchange (833 ms/attempt × 5)
burns up to ~4 s on a dead link before the session proceeds with the
"assume 200 ms" fallback (F6).

---

## 3. Hole-punch success matrix (why CGNAT feels random)

With the relay learning *one* endpoint per side (F4) and both sides
probing, success depends on the NAT filtering behavior of the pair:

| Host NAT / Joiner NAT | Punch succeeds? |
|---|---|
| Cone / EIM (any) / Cone / EIM | Yes — both endpoints stable, both probes land |
| Symmetric / Symmetric | Only if the relay's recorded endpoints happen to be the punch-time endpoints (drift → F3/F4 recovery, best-effort accept can false-positive) |
| Strict symmetric (address-based filtering) both | Generally **no** — each side's probes arrive from an "unknown" flow and are dropped; `learn_peer_port` can't bootstrap because the first probe never arrives |
| Either side UDP-blocked by carrier | No — no fallback (F2) |
| Host on CGNAT with short entry timeout | Launcher punch may succeed, then **F1** kills the DLL pass |

CGNAT in practice is usually endpoint-independent-mapping with
address-based filtering and *short* entry timeouts — which lands most
pairs in the "works sometimes, dies sometimes" band rather than a clean
failure. That is precisely what the users report.

---

## 4. Failure scenarios matching the complaints

| # | User situation | What they see | Root cause |
|---|---|---|---|
| S1 | Host on CGNAT, joiner anywhere | Joiner: game boots, sits ~60 s, "Initial connect timeout", game closes. Sometimes works, sometimes not | F1 + F5 |
| S2 | Either side on strict CGNAT / UDP-filtered carrier | "Hole-punch failed (NAT too restrictive?) … or maybe this is a skill issue" | F2 |
| S3 | Smart-host user, relay flaky | Host: "Listening for direct connection…" forever. Joiner: failed | F3 (silent downgrade) |
| S4 | Mobile/4G, bad uplink at punch time | Random 10 s hole-punch timeout on one side only | F3 (short budget) |
| S5 | Mobile, connection degrades in match | Inputs stall → 10 s → "Timed out!", or "Opponent disconnected" | F6, F8 |
| S6 | IPv6-only network | "Relay unreachable", no way to play online | F7 |
| S7 | Host's phone/network blips while joiner is joining | Joiner: "Room not found" | F9 |
| S8 | CGNAT endpoint drifts mid-punch | Joiner "succeeds" punch, then CONNECT goes nowhere; or 60 s timeout | F3 + F4 |

---

## 5. Improvement list

### P0 — quick wins (small, low-risk, directly hit the complaints)

1. **Keep NAT mappings alive across the launch gap (fixes F1/F5 — the
   biggest one).**
   - Launcher: after the relay result, keep sending 1-byte NullMsgs to the
     peer endpoint (host side: to the joiner's endpoint) on the still-open
     UDP socket until the game process is running + a grace window, instead
     of destroying the transport at `deinit`. The launcher process lives
     for the whole match, so the socket can stay open.
   - DLL: while the host waits for its opponent to connect, emit the same
     NullMsg keepalive to `cfg.peer_addr` every ~1–2 s (re-punch on
     demand). This also makes the DLL self-healing if its own entry
     expired.
   - Shrink or remove the legacy 1 s sleep in `game_runner.cpp` (UDP has
     no TIME_WAIT; verify on real Windows).
2. **Lengthen & unify hole-punch semantics (F3).**
   - `kHolePunchTimeoutMs` 10 s → 20–30 s (probes are 1 byte / 50 ms —
     cost is negligible).
   - On hole-punch timeout in the relay host/join paths, do not fail the
     session immediately: re-enter the punch (new UdpData burst refreshes
     both endpoints on the relay — requires the server-side change in
     P1-3, or a local re-punch with the already-learned endpoints) and only
     give up after N rounds.
   - Make `StartSmartHost`'s silent direct-only downgrade visible in the
     UI ("relay unavailable — waiting for direct connection"), and give it
     a finite, honest timeout instead of 1 h.
   - Require at least one confirming probe before declaring success;
     drop the 500 ms "no confirm needed" accept (F3/F4).
3. **Relay failover (F7).** `relay_setup`/`RelayClient` should iterate the
   configured relay list (resolve all upfront, try next on
   `TcpConnectFailed`/`TcpTimeout`) instead of using only `[0]`.
4. **Better delay/rollback suggestion (F6).** Use the ENet peer stats that
   are already available (RTT min/avg, `lastRoundTripTimeVariance`,
   `packetLoss`): pick the tier from worst-observed RTT, add loss/jitter
   margin, treat "all pings lost" as worst case, and re-derive the
   suggestion if the connection stats drift after join (or at least expose
   it so P1-5 hotkeys can act on it).
5. **Error-message pass (F2/F3/F7).** Replace "maybe this is a skill
   issue"; distinguish "relay unreachable" vs "UDP hole-punch failed" vs
   "UDP likely blocked"; add the actionable hints: try again, switch
   relay, switch network, and — for S1/S2 — *ask the opponent to host
   instead* (the host's NAT is the fragile side).
6. **Defuse the dead code (F10).** Either delete `validate_room_code` +
   the `room_validation` snapshot plumbing, or (if it's wanted) add a
   server-side non-matching "peek" message so a probe can never consume a
   room. Same for the unused STUN client path (or use it — see P1-4).

### P1 — medium effort, structural robustness

7. **Server: endpoint refresh.** Stop recording only the *first* UdpData:
   if the sender's public endpoint changes, send a fresh TunInfo to the
   opposite peer (room stays alive a bit longer for this). Fixes F4 at the
   source and makes F1 keepalives more trustworthy.
8. **Server: graceful room teardown.** Don't delete the room on the first
   TCP close: mark it "half-open" for a grace period (e.g. 30–60 s) that
   the surviving peer can re-register/rejoin into, and send the joiner a
   distinct "host left" error instead of "room not found". Separates the
   host-wait timeout from the TTL (F9).
9. **Client-side NAT/relay preflight (revive the STUN work, F10/F2).**
   Use the existing (unused) STUN probe + a second local port to classify
   the local NAT (cone vs symmetric, double NAT/CGNAT detection) before
   the punch. Outcomes: warn "you appear to be behind CGNAT — direct
   connection may fail", pick strategy (e.g. host-side users are
   encouraged to let the *other* player host), and set per-NAT-timeout
   expectations. This is also the diagnostic tool for the field complaints.
10. **In-match adaptive delay + visibility (F6/F8).** Wire the existing
    ENet stats into the overlay (backlog #1/#2) and add the Ctrl+0..9
    delay hotkeys (backlog #13); optionally auto-nudge delay when loss/
    RTT drift persists for a few seconds. Longer term, replace the 10 s
    "Timed out!" hard stop with a pause/hold when inputs stall but the
    connection is alive (GGPO-style), resuming when inputs flow again.
11. **DLL connect diagnostics (F1).** Log the exact endpoint being
    connected to, count CONNECT retransmits, and distinguish "never got an
    ACK" from "reset". On initial-connect failure, surface a specific
    message ("your connection to X:Y never answered — NAT mapping may have
    expired; try again, or ask your opponent to host") instead of the
    generic timeout.
12. **Relay infrastructure.** Second relay in another region, static IPs
    (drop the dynamic-DNS single point of failure), optional client-side
    health probe at startup, and a config-visible relay list in the GUI.

### P2 — big bets (highest ceiling for CGNAT users)

13. **TCP relay data fallback (fixes F2 for good).** Implement the `'T'`
    relay mode CCCaster had: when the hole-punch fails after N attempts,
    both peers open outbound TCP (or a second UDP flow) to the relay and
    stream the ENet payload (or raw game messages) through it. Latency
    penalty is real, but it turns "cannot play" into "can play, slightly
    worse". Requires: relay data plane (forwarding two TCP conns), client
    transport abstraction (game traffic via relay socket instead of
    direct ENet peer), and a "relay mode active" UI indicator.
14. **Mid-match reconnection / resync (fixes F8).** On disconnect, keep
    the game paused (not exited), retry the ENet connection with
    re-punching (launcher socket keepalive still available — see P0-1),
    and on re-connect resync via the existing save/load state + RNG
    machinery (send full state to the rejoined peer). This is the
    difference between "unstable" and "occasionally hiccups".
15. **IPv6 support (F7).** Dual-stack relay (UDP+TCP), ENet IPv6 peers,
    room-code flow unchanged. Needed for IPv6-only mobile/residential
    networks; also future-proofs the relay endpoints.

### Further study (before/while building)

- **Measure real CGNAT entry timeouts** (field logs from users, or a
  small "keepalive probe" diagnostic in the app) to size the P0-1
  keepalive interval and the P0-2 punch budget with real numbers.
- **Validate the launch-gap hypothesis**: add timestamps
  (deinit → CreateProcess → DLL ENet connect → CONNECT ACK) to the DLL
  log and correlate "Initial connect timeout" reports with gap size. If
  confirmed, P0-1 is the fix; if not, F2/F4 share the blame.
- **NAT emulation test rig**: extend `scripts/nettest.sh` with (a) the
  existing `CASTER_SIM_*` loss/lag/jitter for the bad-connection axis and
  (b) a symmetric-NAT/CGNAT emulator (e.g. two `iptables`/`socat`
  masquerading hops or a small Go NAT) so hole-punch + launch-gap
  regressions are testable headlessly. Today nettest only exercises
  localhost direct + relay signaling, never the punch or the gap.
- **Punch matrix verification**: empirically map which NAT pairs succeed
  with the current protocol (table in §3 is theory) — informs whether the
  TCP fallback (P2-13) is needed for a small or a large slice of users.
- **TCP relay bandwidth/latency budget**: measure added RTT through a
  relay hop for the default relay region; decide if a "relay mode" badge
  + suggested delay bump is acceptable.
- **Resync cost study**: full-state transfer size and time for
  MBAA save-state; whether it can be done during the pause without a
  desync (RNG sync already exists — `sendRngState`/SyncHash).
- **ENet 2.x evaluation**: ENet 2 (alpha) rewrites the internals
  (better loss handling, congestion control); probably not worth it now,
  but worth tracking before investing in F8 work.

---

## 6. Suggested order of work

1. P0-1 (launch-gap keepalive) + P1-11 (DLL connect diagnostics +
   timestamps) — one PR, directly testable against S1.
2. P0-2 (punch timeout/semantics) + P1-3 (server endpoint refresh) +
   P1-4 (server graceful teardown) — one relay-protocol PR (bump
   `kAppVersion`, run `nettest.sh --with-relay`).
3. P0-3 (relay failover) + P0-6 (dead code) + P0-5 (error text) — small.
4. P1-5 (NAT preflight) — unlocks the field diagnostics for everything
   else.
5. P0-4 (suggestion) + P1-6 (adaptive delay) together.
6. P2-13 (TCP fallback) after the §5 study items confirm the CGNAT slice
   is large enough to justify a relay data plane.
