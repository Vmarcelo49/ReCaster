# Stubs não implementados

Features stubadas ou parciais no ReCaster. Cada entrada descreve o que
falta, por que está stubado, e o esforço para implementar.

## Resumo

| # | Feature | Status |
|---|---|---|
| 1 | ASM patches de launch (skip config, forceGoto) | ✅ Implementado |
| 2 | Spectate mode (direto + relay) | Parcial |
| 3 | `--delay=N` (manual delay override) | Stub |
| 4 | Display name fallback (NetConnect.dat) | Stub |
| 5 | DX9 overlay (ImGui dentro do jogo) | Stub |
| 6 | Rollback netplay (save/restore de estado) | Parcial — ver `port-status.md` blockers #1–#3 |
| 7 | FSM de netplay (DLL-side) | Parcial — implementada, faltam 7 pontos |
| 8 | Injeção de input do controle (DLL-side) | ✅ Implementado |
| 9 | `entry/dll_stubs.cpp` (dead code) | Deletar |

---

## 1. ASM patches de launch — ✅ Implementado

`src/exe/launcher/launcher.cpp::apply_game_patches()` aplica 3 patches
ao `MBAA.exe` suspenso antes do `ResumeThread`:

| Endereço | Bytes | Efeito |
|---|---|---|
| `0x04A1D42` | `0xEB, 0x0E` | Skip config dialog (parte 1) |
| `0x04A1D4A` | `0xEB` | Skip config dialog (parte 2) |
| `0x42B475` | `0xEB, 0x22` (training) / `0xEB, 0x3F` (versus) | ForceGoto para Training ou Versus |

Assume MBAA.exe 1.07 Rev.1.4.0 (única versão suportada).

## 2. Spectate mode — Parcial

**O que falta**: GUI `do_spectate()` retorna "Not yet implemented". CLI
`run_netplay(Spectate)` rejeita relay; direto cai em join regular (não
spectator). Flag `is_spectator` em `NetplayConfig` nunca é setado para
`true`.

**O que precisaria**: `SpectatorManager` no host (aceita conexões de
spectator, broadcasta `BothInputs`/`RngState`/`RetryMenuIndex` throttled),
flag de spectator na handshake (spectators não trocam ping/config, só
recebem), e modo spectator na DLL (sem injeção de input, só replay).

**Esforço**: Portar `DllSpectatorManager.cpp` do CCCaster (~235 LOC).
Infra de relay já está no lugar. Esforço moderado. **Fora de escopo v1.**

## 3. `--delay=N` — Stub

**O que falta**: O flag é parseado e validado (0..8 em
`cli_args.cpp:62-66`) mas só é logado — não aplicado à `NetplaySession`.
A session sempre usa auto-delay (de RTT) no host.

**O que precisaria**: `NetplaySession::set_manual_delay(uint8_t)` que
seta `config_.manual_delay = true` e `config_.delay = delay`. O campo
`manual_delay` já existe em `NetplayConfig` e o auto-compute em
`session.cpp:712` já é gated em `!config_.manual_delay`. Falta só o
setter + call site em `cli.cpp:188`.

**Esforço**: ~5 LOC em `session.hpp` + 2 LOC em `cli.cpp`. Trivial.

## 4. Display name fallback (NetConnect.dat) — Stub

**O que falta**: Quando `display_name` está vazio no `config.ini`, o
launcher deveria ler o nome do jogador de `System/NetConnect.dat` do
jogo. Fallback não implementado.

**O que precisaria**: Reverse-engineering do formato `NetConnect.dat`
(INI-like ou binário fixed-offset). Constantes `CC_NETWORK_CONFIG_FILE`
e `CC_NETWORK_USERNAME_KEY = "UserName"` já estão em
`game/addresses.hpp`. zzcaster tem um parser em `common/config.zig`
(`fetchGameUserName`).

**Esforço**: ~30 LOC uma vez que o formato é conhecido. Baixo.

## 5. DX9 overlay (ImGui) — Stub

**O que falta**: Overlay não implementado. Os callbacks D3DHook
`PresentFrameBegin`, `EndScene`, `InvalidateDeviceObjects` em
`entry/dll_main.cpp` são no-ops vazios (não podem ser removidos —
`3rdparty/d3dhook/D3DHook.cc` referencia e chama esses símbolos).
Apenas `PresentFrameEnd` faz algo (delega para `frame_limiter`).

**O que precisaria**: Hook de `IDirect3DDevice9::Present`/`EndScene`
(vtable hooking — infra existe em `3rdparty/d3dhook/`, só `Present` é
hookado hoje), `imgui_impl_dx9.cpp` + `imgui_impl_win32.cpp` no build,
WindowProc hook roteando eventos para ImGui (hook existe em
`lifecycle.cpp` mas não roteia para ImGui), handling de device
lost/reset (alt-tab).

**Esforço**: ~600+ LOC. Portar `DllOverlayUi*.cpp` do CCCaster. **Fora
de escopo v1.**

## 6. Rollback netplay — ✅ Implementado

**Implementado**: `RollbackManager` completo
(`netplay/rollback_manager.{hpp,cpp}`), `rollback_addresses.hpp` com ~80
ranges, `MemDump` para save/restore, `RngState` save/restore via
`game_io`. Os 3 pontos antes listados como blockers estão todos wired:
- `isRemoteInputReady` gate no `frameStep` — `dll_main.cpp:1034-1084`
  (spin-lock `isRemoteInputReady && isRngStateReady`)
- `hijackIntroState` ASM hack — `asm_patches.cpp:263` + `dll_main.cpp:411`
- `CC_INTRO_STATE_ADDR = 0` durante rerun — `dll_main.cpp:873-877`

**Falta**: apenas 4 gaps menores de robustez (nenhum é blocker). Ver
`docs/port-status.md` seção "Gaps remanescentes".

## 7. FSM de netplay (DLL-side) — ✅ Implementado

**Implementado**: `NetplayManager` completo
(`netplay/manager.{hpp,cpp}`, ~1100 LOC) com FSM de 10 estados,
`InputsContainer` por player, histórico de `RngState`, geração de
inputs sintéticos por estado, detecção de divergência. `NetplayState`
enum + `isValidNextState()` em `netplay/states.hpp`. Conector ENet em
`netplay/connector.{hpp,cpp}`.

Os 7 pontos do teste de sanidade pós-F.1–F.7 estão **todos
implementados** (verificados por grep contra o código atual):
`isRemoteInputReady` gate, `hijackIntroState`, `CC_INTRO_STATE_ADDR=0`,
resend/timeout (100ms/10s), defaults de gameplay, stage anim off,
60s connect timeout. Ver `docs/port-status.md` seção "Motor de netplay"
para o mapeamento completo step-by-step vs CCCaster.

**Falta**: teste real contra MBAA.exe (two-instance full-duplex). O
código está pronto; os 4 gaps remanescentes são robustez/UX, não
blockers.

## 8. Injeção de input do controle (DLL-side) — ✅ Implementado

`input/input_reader.{hpp,cpp}` lê SDL2 joystick + Win32 keyboard, aplica
`ControllerMapping` (formato compartilhado com a GUI via
`caster/mapping.ini`), filtra SOCD, retorna numpad+buttons do MBAA.
`game/game_io.cpp::writeGameInput()` escreve em
`CC_PTR_TO_WRITE_INPUT_ADDR` (0x76E6AC) + offsets por player.
`frameStep` em `entry/dll_main.cpp` chama ambos a cada frame.

Validado contra MBAA.exe via Wine: inputs direcionais e de botões chegam
ao jogo (evidência em `debug.log`).

## 9. `entry/dll_stubs.cpp` — Deletar

**Status**: dead code. Não está no `CMakeLists.txt` (só
`entry/dll_main.cpp` é compilado). Todos os 6 símbolos (`callback`,
`stopDllMain`, `PresentFrameBegin`, `EndScene`, `InvalidateDeviceObjects`,
`PresentFrameEnd`) são duplicados em `dll_main.cpp:1301-1333`. Incluir
ambos causaria duplicate-symbol link errors.

**Ação**: deletar o arquivo. Leftover do Phase F antes do `dll_main.cpp`
ser implementado.

---

## Módulos portados mas não integrados

Análise comparativa arquivo-a-arquivo com o CCCaster upstream
(`/home/marcelo/Projetos/CCCaster/`). Cada item foi classificado por
verificação de uso real no CCCaster (DLL vs launcher) e no ReCaster.

### Manter — integração pendente (spectator mode, post-v1)

Estes formam a **API surface de spectator** — serão wired quando
`DllSpectatorManager.cpp` (~235 LOC) for portado.

| Item | Por que manter |
|---|---|
| `getBothInputs`/`setBothInputs` (`netplay/manager`) | CCCaster usa em `DllSpectatorManager:171` (broadcast) e `DllMain.cpp:1522` (receive) |
| `InitialGameState::readFromGame` (`entry/lifecycle`) | CCCaster chama de `DllSpectatorManager:87` para construir broadcast payload |
| `getFullCharaName` (`game/character_tables`) | CCCaster usa em LOG de spectate (`DllMain.cpp:1515-1516,1778-1779`) |

### Deletar — truly dead em ambos os projetos (alta confiança)

Zero callers no CCCaster inteiro (não só na DLL):

| Item | Evidência |
|---|---|
| `util/rolling_average.hpp` (arquivo inteiro) | CCCaster: 0 includes em todo o repo |
| `selectorToChara` (`character_tables`) | CCCaster: 0 callers |
| `upperCase` (`string_utils`) | CCCaster: 0 callers |
| `sorted` 1-arg (`algorithms`) | CCCaster: 0 callers (2-arg é usado) |
| `isPowerOfTwo` (`algorithms`) | CCCaster: 0 callers |
| `getNegativeQuadraticScale` (`algorithms`) | CCCaster: 0 callers |

### Deletar — superseded (ReCaster reimplementou inline)

| Item | O que substituiu |
|---|---|
| `util/thread.hpp` (arquivo inteiro) | `std::mutex`/`std::atomic` direto |
| `util/timer.hpp` (arquivo inteiro) | `GetTickCount` wall-clock counters (`dll_main.cpp:194-235`) |
| `memory/change_monitor.hpp` (arquivo inteiro) | Watcher inline em `dll_main.cpp:78-89` |
| `format` printf-like (`string_utils`) | `std::format` (C++23) |
| `normalizeWindowsPath` (`string_utils`) | `find_last_of` inline |
| `ConfirmConfig` (`messages`) | Launcher usa `MsgTag::Confirm` separado (`session.cpp`) |
| `checkHash`/`getHash(string)` (`hash`) | MD5 era p/ integridade de arquivo/msg; ReCaster usa ENet reliable |

### Deletar — launcher-only em CCCaster (DLL não precisa)

| Item | Uso em CCCaster |
|---|---|
| `util/statistics.hpp` (arquivo inteiro) | `Pinger` no launcher (`MainApp.cpp`) |
| `split` (`string_utils`) | Launcher config + dropped features |
| `lexical_cast` (`string_utils`) | Launcher config + replay parsing |
| `generateRandomId` (`algorithms`) | Logger + session ID no launcher |

### Deletar — debug/overlay (overlay fora de scope v1)

| Item | Re-add quando |
|---|---|
| `formatAsHex` (`string_utils`) | Overlay ImGui for implementado |
| `clamped` (`algorithms`) | Overlay ImGui for implementado |
| `getShortCharaName` (`character_tables`) | Replay/trial/palette (fora v1) |

### Decisão humana necessária

- **`ChangeConfig`** (`messages`): CCCaster usa só em debug hotkeys
  (F5/F6/F7, fora v1). Deletar agora e re-portar quando hotkeys
  entrarem, ou manter como placeholder?

**Total de cleanup**: ~400 LOC candidatas a deleção, mantendo apenas a
API surface de spectator (3 itens, post-v1). Nenhum impacto no path
crítico de v1.
