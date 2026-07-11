# ReCaster

C++23 rewrite of CCCaster — a rollback netplay client for MBAACC.

Extensively used AI to make this possible.

<img width="1027" height="796" alt="image" src="https://github.com/user-attachments/assets/c71a086c-abd8-4ba4-a6b8-967943d5bebd" />


## Current Status

- **Online netplay working** via relay (room codes) and direct connection (IP:port)
- **Rollback** with state save/restore, RNG synchronization, desync detection
- **GUI** SDL2 + ImGui with host/join/spectate, controller mapping, settings
- **Controllers** keyboard and more controller support via SDL2, with customizable mapping
- **Relay server** custom server with NAT traversal via hole-punching (no port forwarding)


### CLI mode

All features are also available from the command line — run `caster.exe --help` for details:
- `caster.exe --host` — host a netplay session
- `caster.exe --join=192.168.1.10:46318` — direct join
- `caster.exe --join=#ABCD` — relay join via room code
- `caster.exe --training` — offline training mode
- `caster.exe --versus` — offline versus mode

### Temporarily removed (planned to return)

Spectator, custom palettes, combo trials, in-game overlay, auto-updater.

## Differences from CCCaster

| Area | CCCaster | ReCaster |
|---|---|---|
| **Language** | C++14 | C++23 |
| **SDL** | SDL 1.2 | SDL 2.0 |
| **Networking** | Manual Winsock sockets (TCP/UDP) | ENet (reliable UDP library) |
| **Compression** | miniz (zlib) for hash compression | Removed — xxHash128 is fast enough without compression |
| **Hashing** | MD5 | xxHash128 (XXH3) |
| **Architecture** | Multi-threaded (EventManager + Timer + Socket callbacks) | Single-threaded synchronous (step() on game thread) for now |
| **GUI** | AntTweakBar / ImGui (SDL 1.2) | ImGui (SDL 2.0 + OpenGL 3.0) |
| **Build system** | Makefile | CMake + MinGW cross-compile |
| **Relay** | External Python script | Built-in Go server with room codes |
| **Dependencies** | Prebuilt static libs (SDL, AntTweakBar, etc.) | Fetched from source via CMake FetchContent (SDL2, ImGui, ENet) |

## Requirements

- **Compiler:** MinGW-w64 i686 (cross-compile from Linux) or native MinGW on Windows
- **CMake:** 3.20+
- **Build deps:** All fetched automatically via CMake FetchContent (SDL2, Dear ImGui, ENet, xxHash, MinHook)

## Building

```bash
# Install MinGW-w64 cross-compiler (Debian/Ubuntu)
sudo apt-get install mingw-w64 cmake zip

# Build (configures + compiles + strips + zips)
./scripts/build.sh

# Rebuild (skip configure, build only)
./scripts/build.sh rebuild
```

Output:
- `build/bin/caster.exe` — launcher (SDL2 + ImGui GUI)
- `build/bin/hook.dll` — payload (rollback netplay engine, injected into MBAA.exe)
- `release/caster.zip` — both binaries zipped

Place `caster.exe` and `hook.dll` in the same folder as `MBAA.exe`.

## Roadmap

Tons of features coming from other projects

| Feature | Description |
|---|---|
| **Air dash macro** | Jump → airdash macro (9AB/7AB), already implemented in zzcaster, just needs porting |
| **Training while hosting** | Training mode while waiting for opponent; on connect, kills and relaunches in netplay mode |
| **Auto updater** | Automatic updates via GitHub releases, with progress bar |
| **DX9 Overlay** | imgui overlay, enables in-game config, frame data, debug info |
| **Spectator** | Spectator mode: late-join an ongoing match, receives BothInputs and replays via rollback |
| **Combo trials** | Input sequences with hit/miss detection, displayed via in-game overlay |
| **Discord integration** | "Playing MBAACC" status on Discord profile, with Click to Join via invite protocol |
| **Custom palette** | Custom palettes but easier to setup |
| **Challonge integration** | Automatic check-in for tournaments and result reporting via Challonge API |
| **Extended Training** | Reversals, save states, frame data display, hitbox viewer, RNG control, position lock — integration of fangdreth's mod |
| **Replay takeover** | Watch a saved replay and take control of a player at any frame to practice punishes |
| **Steamworks provider** | Steam P2P networking replacing ENet + relay, with invites, friends list and native rich presence from SkyLeite |
| **2v2 online** | 4-player online mode with adapted rollback — from Inana (meepster99) |


## zzcaster

zzcaster is an abandoned attempt on trying to rewrite CCCaster on zig, got stuck in endless issues with rollback and RNG synching, that why this project exists
