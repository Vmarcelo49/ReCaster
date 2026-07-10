# Arquitetura da DLL (`src/dll/`)

A `hook.dll` é o payload injetado no `MBAA.exe`. Ela aplica patches
binários no jogo, substitui o frame limiter, lê input local, conecta
ao peer via ENet, roda a FSM de netplay e implementa rollback netplay
estilo GGPO.

## Estrutura de pastas

```
src/dll/
├── entry/      DllMain + lifecycle do hook + stubs
├── game/       Dados e I/O específicos do MBAA 1.07 Rev.1.4.0
├── hooks/      Patches x86 + frame limiter custom
├── input/      Leitura de joystick/teclado → formato MBAA
├── ipc/        Cliente do named pipe (DLL ← launcher)
├── memory/     Snapshot/restore de ranges de memória
├── netplay/    FSM + transporte ENet + rollback
├── protocol/   Tipos de mensagem do wire-protocol + decoder
└── util/       Helpers genéricos (sem deps DLL-internas)
```

## Responsabilidades por pasta

### `entry/` — Entry point + lifecycle
- `dll_main.cpp` — `DllMain` + `callback()` (chamado a cada frame pelo hook
  do main-loop). Orquestra: ChangeMonitor → frameStep → writeGameInput.
  Single-threaded; tudo roda na thread main do jogo.
- `lifecycle.{hpp,cpp}` — `initializePreLoad()` / `initializePostLoad()` /
  `deinitialize()`. Hook de `WindowProc` via MinHook, hook de DX9 `Present`
  via D3DHook. Também contém `InitialGameState::readFromGame` e
  `SyncHash::readFromGame` (snapshots de memória para sync).
- `dll_stubs.cpp` — **Não compilado** (não está no CMakeLists). Leftover
  do Phase F antes do `dll_main.cpp` ser implementado. Candidato a deletar.

### `game/` — Dados hardcodeados do MBAA 1.07
- `addresses.hpp` — Todos os `CC_*_ADDR` (offsets de memória do MBAA.exe
  1.07 Rev.1.4.0), button masks, `IndexedFrame`, constantes de netplay.
  Version-specific: se o jogo for atualizado, todo `CC_*_ADDR` precisa
  ser re-verificado.
- `character_tables.{hpp,cpp}` — Lookup tables chara ↔ selector ↔ nome.
- `game_io.{hpp,cpp}` — `writeGameInput(player, direction, buttons)` e
  `getRngState()` / `setRngState()`. I/O puro com a memória do jogo.
- `rollback_addresses.hpp` — Builder do `MemDumpList` com ~80 ranges
  que o RollbackManager salva/restaura. Substitui o `rollback.bin`
  blob do CCCaster (construído em runtime).

### `hooks/` — Hooking de baixo nível
- `asm_patches.{hpp,cpp}` — Patches x86 no MBAA.exe: `hookMainLoop`,
  `hijackControls`, `hijackMenu`, `detectRoundStart`, `hookPresentCaller`,
  + patches singulares (`multiWindow`, `hijackEscapeKey`, `disableFpsLimit`,
  etc.). Define `extern "C" void callback()` — o entry point por-frame.
- `frame_limiter.{hpp,cpp}` — FPS limiter via `QueryPerformanceCounter`,
  acionado pelo hook de D3D9 `Present`. Substitui o limiter nativo do
  jogo para sincronizar com o netplay rollback. **Desativado no Wine**
  (o hook de Present não funciona lá; o limiter nativo é preservado).

### `input/` — Leitura de input local
- `input_reader.{hpp,cpp}` — `read_local_input(SDL_Joystick*, ControllerMapping&)
  → GameInput`. Lê SDL2 joystick + Win32 keyboard, aplica o `ControllerMapping`
  compartilhado com a GUI (formato `caster/mapping.ini`), filtra SOCD,
  retorna no formato numpad+buttons do MBAA.

### `ipc/` — IPC com o launcher
- `receiver.{hpp,cpp}` — Cliente do named pipe aberto pelo launcher.
  Recebe um `config_buffer` (modo host/client/training/spectator, delay,
  rollback, match_seed, mappings) e publica thread-safe para os demais
  módulos da DLL.

### `memory/` — Snapshot/diff de memória
- `mem_dump.{hpp,cpp}` — Árvore save/restore de ranges de memória. Um
  `MemDump` é um root (addr+size) com children `MemDumpPtr` que seguem
  pointers. Usado pelo RollbackManager.
- `change_monitor.hpp` — Observer-pattern para mudanças em endereços.
  **Atualmente não usado** — `dll_main.cpp` implementa seu próprio watcher
  inline para 3 watchpoints (gameMode, gameState, roundStartCounter).

### `netplay/` — Engine de netplay
- `states.hpp` — Enum `NetplayState` (PreInitial → Initial → AutoCharaSelect
  → CharaSelect → Loading → CharaIntro → Skippable → InGame → RetryMenu →
  ReplayMenu) + `isValidNextState()` (tabela de transições).
- `manager.{hpp,cpp}` — `NetplayManager`: o "cérebro". Possui a FSM,
  `InputsContainer` por player, histórico de `RngState`, retry-menu-index,
  `IndexedFrame` atual. `frameStep()` dispatcha por estado para gerar
  inputs sintéticos (navegação de menu, mashing) e detectar divergência
  para rollback.
- `connector.{hpp,cpp}` — Transporte ENet single-threaded. Polla a cada
  frame na thread main do jogo. Inbox/outbox por tipo de mensagem.
  RELIABLE para controle/RNG/SyncHash, UNRELIABLE+UNSEQUENCED para
  `PlayerInputs` por-frame.
- `inputs_container.hpp` — `InputsContainer<T>`: mapa indexado
  {transition_index → frame → T} com semântica de predição GGPO.
  `getLastChangedFrame()` é o gatilho de rollback.
- `rollback_manager.{hpp,cpp}` — Save/restore de estado via pool de
  buffers. `saveState(netMan)` / `loadState(target, netMan)`. Friend de
  `NetplayManager` para escrever de volta `_state`/`_indexedFrame`.

### `protocol/` — Wire-protocol
- `messages.{hpp,cpp}` — Structs de mensagem: `BothInputs`, `PlayerInputs`,
  `RngState`, `InitialGameState`, `NetplayConfigMsg`, `SyncHash`, `MenuIndex`,
  `ChangeConfig`, `TransitionIndex`, `ConfirmConfig`, `ClientMode`. Cada
  uma com `serialize()` / `deserialize()` / `wire_size()` (binário
  little-endian manual, sem cereal).
- `decoder.{hpp,cpp}` — Tag-dispatch decoder: lê 1 byte de `MsgType`,
  dispatcha para o `T::deserialize()` correspondente.

### `util/` — Helpers genéricos
Sem dependências de DLL-internals. Poderiam migrar para `src/common/`
um dia.
- `algorithms.hpp` — `sorted` (pointer-indirect sort), `clamped`,
  `generateRandomId`, etc.
- `string_utils.{hpp,cpp}` — `trimmed`, `split`, `formatAsHex`,
  `lexical_cast`, `normalizeWindowsPath`.
- `exceptions.{hpp,cpp}` — `Exception` + `WinException` (com
  `GetLastError` / `WSAGetLastError`).
- `hash.{hpp,cpp}` — `getHash` / `checkHash` via xxHash128 (`XXH3_128bits`,
  16 bytes). Usado pelo `SyncHash` para detecção de desync.
- `rolling_average.hpp` — Média móvel template (janela fixa compile-time).
- `statistics.hpp` — Estatística streaming (Welford).
- `thread.hpp` — Wrappers `Mutex`/`Lock`/`CondVar`/`Thread`.
- `timer.hpp` — `TimerManager` singleton com callbacks one-shot.

> **Nota**: `rolling_average`, `statistics`, `thread`, `timer` e
> `change_monitor` atualmente não são incluídos por nenhum arquivo. Foram
> portados do CCCaster mas a integração ainda não aconteceu. Ver
> `docs/stubs.md`.

## Convenções de include

- Includes **dentro da mesma pasta** usam forma relativa:
  `#include "manager.hpp"` (de `connector.cpp` em `netplay/`).
- Includes **de outra pasta** usam prefixo da pasta:
  `#include "netplay/manager.hpp"` (de `dll_main.cpp` em `entry/`).
- Includes de `src/common/` usam path relativo:
  `#include "../common/logger.hpp"`.
- O include dir `src/dll` está no CMake, então `#include "game/addresses.hpp"`
  resolve de qualquer arquivo da DLL.

## Namespaces

Tudo está em `caster::dll::*` (flat, não reflete a estrutura de pastas).
Sub-namespaces existem para alguns módulos: `caster::dll::asm_patches`,
`caster::dll::frame_limiter`, `caster::dll::netplay` (o connector),
`caster::dll::ipc_receiver`, etc.

## Build

MinGW-w64 i686 (32-bit), C++23. Ver `CMakeLists.txt` target `hook`.
Dependências: SDL2, ENet, ImGui (não usado na DLL ainda), MinHook, D3DHook,
xxHash (inline). Cross-compilado via `cmake/toolchain-mingw32.cmake`.

## Referência: CCCaster upstream

O porte foi feito contra o CCCaster (fork do zzcaster). O inventário
completo de arquivos `Dll*` do CCCaster e a recomendação de porte está
em `docs/cccaster-dll-targets.md` (referência histórica, não muda).
