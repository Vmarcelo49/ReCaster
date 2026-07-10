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

## 6. Rollback netplay — Parcial

**Implementado**: `RollbackManager` completo
(`netplay/rollback_manager.{hpp,cpp}`), `rollback_addresses.hpp` com ~80
ranges, `MemDump` para save/restore, `RngState` save/restore via
`game_io`.

**Falta**: 3 blockers que impedem o rollback de funcionar. Ver
`docs/port-status.md` seção "Blockers da Fase F" pontos #1, #2, #3:
- `isRemoteInputReady` gate no frameStep (sem ele, rollback nunca dispara)
- `hijackIntroState` ASM hack (sem ele, intro re-executa durante rerun)
- `CC_INTRO_STATE_ADDR = 0` durante rerun (depende do anterior)

**Esforço**: ~30 LOC total. Depende de #7 estar estabilizado.

## 7. FSM de netplay (DLL-side) — Parcial

**Implementado**: `NetplayManager` completo
(`netplay/manager.{hpp,cpp}`, ~1100 LOC) com FSM de 10 estados,
`InputsContainer` por player, histórico de `RngState`, geração de
inputs sintéticos por estado, detecção de divergência. `NetplayState`
enum + `isValidNextState()` em `netplay/states.hpp`. Conector ENet em
`netplay/connector.{hpp,cpp}`.

**Falta**: 7 pontos do teste de sanidade. Ver `docs/port-status.md`
seção "Blockers da Fase F" — 3 blockers de rollback (#1, #2, #3) + 4
de robustez/UX (#4 timeout, #5 defaults de gameplay, #6 stage anim, #7
connect timeout).

**Esforço**: ~90 LOC total, ~0.5 dia.

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

Estes arquivos foram portados do CCCaster mas atualmente não são
incluídos por nenhum arquivo do projeto. Podem ser integrados conforme
oporte avança, ou deletados se decidirmos não usar:

| Arquivo | Status | Nota |
|---|---|---|
| `util/rolling_average.hpp` | Não incluído | Média móvel. Útil para telemetria/ping smoothing. |
| `util/statistics.hpp` | Não incluído | Stats streaming (Welford). Útil para métricas de rollback. |
| `util/thread.hpp` | Não incluído | Wrappers Mutex/CondVar/Thread. Design single-threaded não usa. |
| `util/timer.hpp` | Não incluído | TimerManager one-shot. Pode ser útil para o blocker #4 (timeout). |
| `memory/change_monitor.hpp` | Não incluído | Observer de mudanças. `dll_main.cpp` tem watcher inline próprio. |

**Atenção**: antes de deletar, verificar se algum desses será necessário
para os blockers #4 (timer) ou para futuras features (rolling_average,
statistics para telemetria de rollback). O porte do CCCaster ainda não
está completo, então código aparentemente morto pode ser integração
pendente.
