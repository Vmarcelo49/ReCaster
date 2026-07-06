# Plano: Port da DLL (hook.dll) — Versão 1: Partidas Online

Este documento descreve o plano para portar a DLL do CCCaster para o ReCaster,
focando exclusivamente no que é necessário para **partidas online funcionarem**.
Features adicionais (trial mode, paletas customizadas, overlay de combo,
frame rate limiter, replay editor) ficam de fora desta versão.

> **FOCO ATUAL — Fase F**: As Fases A–E estão completas e validadas contra
> MBAA.exe via Wine. A Fase F (Netplay engine) é o foco ativo de
> desenvolvimento e tem um plano de execução dedicado em
> [`docs/phase-f-execution-plan.md`](phase-f-execution-plan.md). Esse plano
> detalha, linha-a-linha contra o CCCaster, as divergências encontradas nos
> arquivos já portados (`InputsContainer`, `NetplayState`, `RollbackManager`,
> `rollback_addresses`) e a sequência de 7 sub-etapas para completar a Fase F
> com validação incremental. A entrada "F — Netplay engine" da seção de
> status abaixo é substituída por aquele documento.

---

## Princípios

1. **Copiar e adaptar**: não há problema em copiar o código original do
   CCCaster. Adaptar para C++23 onde aplicável (std::format, std::span,
   constexpr, concepts), mas sem reescrever lógica que já funciona.
2. **Mínimo viável**: só portar o que é necessário pra uma partida online
   acontecer de ponta a ponta. Tudo else fica documentado como stub.
3. **Uma exclusão explícita**: o controle de rollback via keyboard
   (Ctrl+0..9 pra mudar delay/rollback dinamicamente durante a partida)
   será removido — é uma feature de debug que não pertence na versão 1.

---

## Arquitetura da DLL no CCCaster

```
DllMain.cpp (entry point + state machine orquestradora)
  ├── DllHacks.cpp        (aplica ASM patches + hooks WindowProc + DX9)
  ├── DllAsmHacks.cpp     (catálogo de patches binários)
  ├── DllProcessManager   (escreve inputs no jogo + lê RNG + IPC pipe)
  ├── DllControllerManager (lê controles → inputs do jogo)
  ├── DllControllerUtils  (SOCD + conversão bitmask → numpad+buttons)
  ├── DllNetplayManager   (FSM de netplay: menu nav, input gen, etc)
  ├── DllRollbackManager  (save/restore de estado do jogo)
  ├── DllSpectatorManager (broadcast pra espectadores)
  ├── DllFrameRate        (FPS limiter custom — SKIP)
  ├── DllOverlayUi*       (overlay ImGui/text — SKIP nesta versão)
  ├── DllTrialManager     (trial mode — SKIP)
  └── DllPaletteManager   (paletas custom — SKIP)
```

---

## O que VAMOS portar (versão 1)

### Prioridade 1 — Fundação (sem isso nada funciona)

| # | Arquivo origem (CCCaster) | Arquivo destino (ReCaster) | LOC | Descrição |
|---|---|---|---|---|
| 1 | `netplay/Constants.hpp` | `src/dll/constants.hpp` | ~320 | TODOS os `CC_*_ADDR` offsets do MBAA.exe 1.07. Cópia direta, adaptar `#pragma once` + `namespace`. |
| 2 | `netplay/NetplayStates.hpp` | `src/dll/netplay_states.hpp` | ~35 | Enum `NetplayState` (PreInitial → Initial → AutoCharaSelect → CharaSelect → Loading → CharaIntro → Skippable → InGame → RetryMenu → ReplayMenu). |
| 3 | `netplay/CharacterSelect.hpp/.cpp` | `src/dll/character_select.{hpp,cpp}` | ~210 | Tabelas chara↔selector + nomes de personagens. Lookup tables puras. |
| 4 | `netplay/Messages.hpp` | `src/dll/messages.hpp` | ~510 | Tipos do protocolo: NetplayConfig, InitialGameState, RngState, SyncHash, BothInputs, PlayerInputs, etc. **Adaptar**: trocar cereal por serialização manual (como já fizemos no config_buffer). |

### Prioridade 2 — Infraestrutura compartilhada

| # | Arquivo origem | Arquivo destino | LOC | Descrição |
|---|---|---|---|---|
| 5 | `lib/Enum.hpp` | `src/dll/enum.hpp` | ~55 | Macro `ENUM(NAME, ...)` que gera enum + `str()`. Usado em todo lugar. |
| 6 | `lib/Algorithms.hpp` | `src/dll/algorithms.hpp` | ~115 | `clamped`, `sorted`, `generateRandomId`, etc. Templates utilitários. |
| 7 | `lib/StringUtils.hpp/.cpp` | `src/dll/string_utils.{hpp,cpp}` | ~230 | `format`, `split`, `trimmed`, `lexical_cast`. **Adaptar**: `std::format` onde possível. |
| 8 | `lib/Logger.hpp/.cpp` | — | — | **Reusar** nosso `src/common/logger.hpp` já existente. Adaptar macros `LOG`/`ASSERT`. |
| 9 | `lib/Exceptions.hpp/.cpp` | `src/dll/exceptions.{hpp,cpp}` | ~105 | `Exception` + `WinException` com `GetLastError`. |
| 10 | `lib/Timer.hpp/.cpp` + `lib/TimerManager.hpp/.cpp` | `src/dll/timer.{hpp,cpp}` | ~300 | Timer one-shot + TimerManager singleton com `QueryPerformanceCounter`. **Adaptar**: usar `std::chrono` onde possível. |
| 11 | `lib/Thread.hpp/.cpp` | `src/dll/thread.{hpp,cpp}` | ~200 | `Mutex`, `Lock`, `CondVar`, `Thread`. **Adaptar**: usar `std::mutex`/`std::thread` de C++23. |
| 12 | `lib/Compression.hpp/.cpp` | `src/dll/compression.{hpp,cpp}` | ~80 | MD5 + zlib compress/uncompress. Para SyncHash. |

### Prioridade 3 — Protocolo de aplicação (transport já é ENet)

| # | Arquivo origem | Arquivo destino | LOC | Descrição |
|---|---|---|---|---|
| 13 | `lib/Protocol.hpp/.cpp` + `lib/ProtocolEnums.hpp` | `src/dll/protocol.{hpp,cpp}` | ~590 | Framework de serialização. **Adaptar**: sem cereal — serialização manual binária (como config_buffer). |
| 14 | `lib/RollingAverage.hpp` | `src/dll/rolling_average.hpp` | ~70 | Média móvel. Template puro. |
| 15 | `lib/Statistics.hpp` | `src/dll/statistics.hpp` | ~105 | Estatísticas online (Welford). |

**NÃO portamos** (ENet já cobre — elimina ~2944 LOC):
- ~~`lib/Socket.hpp`~~ — ENetHost/ENetPeer substituem
- ~~`lib/TcpSocket.hpp/.cpp`~~ — `enet_host_connect()` substitui
- ~~`lib/UdpSocket.hpp/.cpp`~~ — ENet com `ENET_PACKET_FLAG_RELIABLE` substitui
- ~~`lib/GoBackN.hpp/.cpp`~~ — ENet já é reliable UDP (selective repeat, melhor que go-back-n)
- ~~`lib/SmartSocket.hpp/.cpp`~~ — nosso `relay_client` já faz hole-punching
- ~~`lib/SocketManager.hpp/.cpp`~~ — `enet_host_service()` substitui
- ~~`lib/IpAddrPort.hpp/.cpp`~~ — `std::string` + `ENetAddress` substituem

A DLL usa **nosso `EnetTransport` existente** (`src/common/net/enet_transport.{hpp,cpp}`) como transport layer, igual o launcher já faz. Os tipos de mensagem do CCCaster (`Messages.hpp`) viram o protocolo de aplicação em cima do ENet.

### Prioridade 4 — Input (simplificado: reusa SDL2 + mapping existente)

O CCCaster tem ~4065 LOC de código de input porque reimplementa abstrações
que SDL2 + nosso `mapping.hpp` já fornecem. **Não portamos nenhum desses
arquivos** — escrevemos apenas 1 arquivo novo de ~200 LOC.

**Já temos** (reusado diretamente pela DLL):
- `src/common/controller/mapping.{hpp,cpp}` (~390 LOC) — `InputBinding`,
  `BindingTarget`, `ControllerMapping` (13 bindings + SOCD + deadzone +
  device_index), `default_xbox()`, save/load INI. O mesmo formato que a
  GUI usa pra configurar os bindings.
- `src/common/controller/binder.{hpp,cpp}` (~130 LOC) —
  `poll_for_bind_input()` que já lê SDL_Joystick + Win32 keyboard. A DLL
  reusa a mesma lógica de leitura.

**NÃO portamos** (elimina ~3955 LOC):
- ~~`lib/Controller.hpp/.cpp`~~ (~1100) — nossa `ControllerMapping` +
  `InputBinding` já fazem o mesmo. SDL2 é a abstração de hardware.
- ~~`lib/ControllerManager.hpp/.cpp`~~ (~990) — singleton com polling
  thread. A DLL não precisa — o hook `callback()` roda a cada frame do
  jogo, é lá que lemos o estado.
- ~~`lib/KeyboardManager.hpp/.cpp`~~ (~280) — hook global de teclado via
  socket. Usamos `GetAsyncKeyState` direto (como `binder.cpp` já faz).
- ~~`lib/KeyboardState.hpp/.cpp`~~ (~150) — edge detection (pressed/
  released). Só útil pra hotkeys, que estamos removendo.
- ~~`lib/JoystickDetector.hpp/.cpp`~~ (~250) — SDL2 já faz via
  `SDL_JOYDEVICEADDED/REMOVED`.
- ~~`lib/KeyboardVKeyNames.hpp`~~ (~35) — só pra UI (launcher já tem).
- ~~`targets/DllControllerManager.hpp/.cpp`~~ (~1150) — trial overlay +
  mapping overlay + hotkeys saem. O útil (`updateControls`) vira nosso
  `input_reader`.
- ~~`targets/DllControllerUtils.hpp`~~ (~110) — SOCD + conversão
  numpad+buttons. Reescrito mais simples em `input_reader`.

| # | Arquivo origem | Arquivo destino | LOC | Descrição |
|---|---|---|---|---|
| 16 | (novo — não portado do CCCaster) | `src/dll/input_reader.{hpp,cpp}` | ~200 | `read_local_input(SDL_Joystick*, ControllerMapping&) → uint16_t` — lê SDL_Joystick + Win32 keyboard, aplica o ControllerMapping (mesmo formato da GUI), filtra SOCD, retorna no formato numpad+buttons do MBAA. Combina a lógica de `binder.cpp` (que já lê os mesmos inputs) com a conversão de formato do `DllControllerUtils`. |

### Prioridade 5 — Game hooks (o coração da DLL)

| # | Arquivo origem | Arquivo destino | LOC | Descrição |
|---|---|---|---|---|
| 24 | `targets/DllAsmHacks.hpp` | `src/dll/asm_hacks.hpp` | ~605 | Catálogo de patches. **Adaptar**: REMOVER patches de palette, trial, SFX filter, screenshot. Manter: hookMainLoop, hijackControls, hijackMenu, detectRoundStart, multiWindow, hijackEscapeKey, enableDisabledStages, **hookPresentCaller** (crítico pra sync de frames). |
| 25 | `targets/DllAsmHacks.cpp` | `src/dll/asm_hacks.cpp` | ~615 | Implementação dos patches + callbacks. **Adaptar**: REMOVER palette/PNG/trial callbacks. Manter: `Asm::write/revert`, `hookMainLoop`, `hijackControls`, `hijackMenu`, `detectRoundStart`, **`_naked_presentFuncCaller`** (trampoline do Present hook). |
| 26 | `targets/DllHacks.hpp/.cpp` | `src/dll/dll_hacks.{hpp,cpp}` | ~335 | Lifecycle: `initializePreLoad()`, `initializePostLoad()`, `deinitialize()`. WindowProc hook via MinHook. **MANTER DX9 hook** (`InitDirectX` + `HookDirectX` + vtable hooking de `Present`) — é infraestrutura crítica pra sync de frames, não só overlay. O que NÃO portamos: o código de **rendering** do overlay (DllOverlayUi*, ImGui, ID3DXFont, vertex buffers). O hook de Present fica ativo mas só chama `limitFPS()`, não desenha nada. |
| 27 | `targets/DllProcessManager.cpp` | `src/dll/dll_process_manager.cpp` | ~120 | `writeGameInput()`, `getRngState()`, `setRngState()`, `connectPipe()`. **Adaptar**: já temos `ipc_receiver` — estender. |
| 28 | `lib/MemDump.hpp/.cpp` | `src/dll/mem_dump.{hpp,cpp}` | ~510 | Save/restore de ranges de memória para rollback. |
| 29 | `lib/ChangeMonitor.hpp/.cpp` | `src/dll/change_monitor.{hpp,cpp}` | ~210 | Monitora mudanças em endereços de memória (dispara callbacks quando values mudam). |

### Prioridade 5b — DX9 hook infrastructure + frame sync

| # | Arquivo origem | Arquivo destino | LOC | Descrição |
|---|---|---|---|---|
| 30 | `3rdparty/d3dhook/D3DHook.h/.cc` + `3rdparty/d3dhook/CHookJump.h/.cc` | `src/dll/d3d_hook.{hpp,cpp}` | ~300 | Vtable hooking de `IDirect3DDevice9::Present` e `EndScene`. Cria device temporário pra obter vtable, instala jump hook. **Portar direto** — é game-agnostic. |
| 31 | `targets/DllFrameRate.hpp/.cpp` | `src/dll/frame_rate.{hpp,cpp}` | ~165 | FPS limiter via `QueryPerformanceCounter` + `PresentFrameEnd()` (chamado após Present). **Adaptar**: manter `enable()`, `limitFPS()`, `PresentFrameEnd()`. Remover `CC_FPS_COUNTER_ADDR` display (sem overlay). O `limitFPS()` é o que alinha o pacing dos frames com o netplay rollback. |

### Prioridade 6 — Netplay engine (o cérebro)

| # | Arquivo origem | Arquivo destino | LOC | Descrição |
|---|---|---|---|---|
| 30 | `targets/DllNetplayManager.hpp` | `src/dll/netplay_manager.hpp` | ~240 | Classe `NetplayManager`: inputs containers, RNG states, indexed frame, state transitions. |
| 31 | `targets/DllNetplayManager.cpp` | `src/dll/netplay_manager.cpp` | ~1270 | FSM lógica: `frameStepNormal()`, `getCharaSelectInput()`, `getInGameInput()`, etc. **Adaptar**: REMOVER UDP debug logger (porta 17474). |
| 32 | `netplay/InputsContainer.hpp` | `src/dll/inputs_container.hpp` | ~195 | Template `InputsContainer<T>` — mapa indexado frame→input. |
| 33 | `targets/DllRollbackManager.hpp/.cpp` | `src/dll/rollback_manager.{hpp,cpp}` | ~350 | Save/load de estado via memory pool. **Adaptar**: REMOVER SFX history (mute-during-reroll). Manter: `saveState()`, `loadState()`, `RepInputContainer` fixup. **Depende**: `binary_res_rollback_bin` (precisa ser extraído do CCCaster ou regenerado). |
| 34 | `targets/DllSpectatorManager.cpp` | `src/dll/spectator_manager.cpp` | ~235 | Broadcast de inputs para espectadores. **Pode ser cortado** se spectate não for prioridade — mas é genérico o suficiente pra portar direto. |
| 35 | `targets/DllMain.cpp` | `src/dll/dll_main.cpp` (refatorar) | ~2255 | Orquestrador central: `frameStepNormal()`, `frameStepRollback()`, `frameStepSpectator()`, callbacks de socket, `callback()` (hook de main-loop). **Adaptar**: REMOVER trial manager, replay manager, frame-step hotkeys (F5/F6/F7), delay/rollback hotkeys (Ctrl+0..9), broadcast mode. Manter: netplay FSM principal, socket callbacks, IPC. |

### Prioridade 7 — Resource binário (rollback states)

| # | Item | Descrição |
|---|---|---|
| 36 | `res/rollback.o` (binary blob) | Lista de (address, size) pairs que o RollbackManager salva/restaura. **Precisa ser extraído do build do CCCaster ou regenerado** a partir de análise do MBAA.exe. Sem isso, rollback não funciona. É um array de structs `{void* addr; size_t size;}` compilado como resource. |

---

## O que NÃO VAMOS portar (versão 1)

### Features adicionais (fora do escopo de partidas online)

| Arquivo origem | LOC | Razão da exclusão |
|---|---|---|
| `targets/DllTrialManager.hpp/.cpp` | ~2100 | Trial mode (combo training). Feature adicional, não necessária pra partidas online. |
| `targets/DllPaletteManager.cpp` | ~50 | Paletas customizadas via PNG. Feature cosmética. |
| `targets/DllOverlayUi.hpp/.cpp` | ~157 | Overlay de texto (3 colunas). Sem overlay nesta versão. |
| `targets/DllOverlayUiImGui.cpp` | ~116 | Overlay ImGui de debug. |
| `targets/DllOverlayUiText.cpp` | ~535 | Overlay de texto + trial combo text. |
| `targets/DllOverlayPrimitives.hpp` | ~103 | Primitivas de desenho D3D9 (só usadas pelo overlay). |
| `targets/oCallDraw.c` + `targets/CallDraw.s` | ~215 | Wrappers ASM pra funções de draw internas do jogo. Só usado por trial. |
| `netplay/ReplayCreator.hpp/.cpp` | ~770 | Criação de replay. Legacy, não referenciado pelo pipeline ativo. |
| `netplay/PaletteManager.hpp/.cpp` | ~340 | Gerenciamento de paletas. Feature cosmética. |

### Nota: DX9 hook vs overlay rendering

São coisas diferentes:

- **DX9 hook** (`D3DHook` + `hookPresentCaller` + `DllFrameRate`): intercepta
  `IDirect3DDevice9::Present` para ter um ponto de sincronização de frames.
  **Crítico** — é assim que a DLL sabe quando um frame foi renderizado e
  alinha o pacing do netplay rollback. **Portamos.**

- **Overlay rendering** (`DllOverlayUi*` + `DllOverlayPrimitives` + ImGui +
  `ID3DXFont`): desenha texto/ImGui em cima do framebuffer do jogo.
  **Não crítico** pra partidas online nesta versão. **Não portamos.**

O hook de Present fica ativo, mas em vez de chamar o código de overlay,
só chama `DllFrameRate::limitFPS()`.

### Features de debug (a remover explicitamente)

| Feature | Onde está | Razão |
|---|---|---|
| **Delay/rollback hotkeys (Ctrl+0..9)** | `DllMain.cpp` | User pediu pra tirar — é controle de rollback via keyboard durante a partida, não pertence na versão 1. |
| Frame-step hotkeys (F5/F6/F7) | `DllMain.cpp` | Debug feature. |
| UDP debug logger (porta 17474) | `DllNetplayManager.cpp` | Debug feature. |
| F3 = trial menu | `DllControllerManager.cpp` | Trial feature. |
| F4 = overlay toggle | `DllControllerManager.cpp` | Overlay feature. |

### Main-only (nunca esteve na DLL)

| Arquivo | Razão |
|---|---|
| `lib/ConsoleUi.hpp/.cpp` | UI de console do launcher. |
| `lib/MatchmakingManager.hpp/.cpp` | Matchmaking do launcher. |
| `lib/Lobby.hpp/.cpp` | Lobby do launcher. |
| `targets/Main*.cpp` | Launcher-only. |

---

## Dependências externas

### Bibliotecas que precisaremos adicionar ao build

| Lib | Versão | Uso | De onde |
|---|---|---|---|
| **MinHook** | 1.3.3 | Hooking de WindowProc (e futuramente DX9) | `3rdparty/minhook/` no CCCaster — copiar direto |
| **zlib** | qualquer | Compressão de SyncHash + messages | Já no mingw-w64 sysroot (`-lz`) |
| **md5** | — | SyncHash (detecção de desync) | `3rdparty/md5.c` no CCCaster — copiar direto |

### Bibliotecas que JÁ temos e vamos reusar

- **SDL2** — já integrado (joystick + timer)
- **ENet** — já integrado (mas a DLL do CCCaster usa sockets raw, não ENet — ver decisão abaixo)
- **ImGui** — já integrado (mas sem overlay nesta versão)

### Decisão de arquitetura: ENet (não portar socket layer do CCCaster)

O CCCaster tem uma camada própria de sockets (`lib/Socket.hpp` + TCP/UDP +
GoBackN + SmartSocket) porque **na época não usavam ENet**. Essa camada
reinventa o que ENet já fornece:

- GoBackN ARQ → ENet usa selective repeat (melhor)
- SmartSocket (TCP + UDP tunnel) → nosso `relay_client` já faz hole-punching
- SocketManager (select/poll) → `enet_host_service()`

**Decisão: NÃO portar o socket layer.** A DLL usa nosso `EnetTransport`
existente (`src/common/net/enet_transport.{hpp,cpp}`) como transport,
igual o launcher já faz. Isso elimina ~2944 LOC.

Consequência: ReCaster só joga contra ReCaster (protocolo ENet é
incompatível com SmartSocket do CCCaster). Isso já era o caso desde a
Fase 8 — o launcher já usa ENet.

---

## Ordem de implementação

```
Fase A — Fundação (1-4)
  constants.hpp + netplay_states + character_select + messages
  ↓
Fase B — Infra (5-12)
  enum + algorithms + string_utils + logger adapt + exceptions
  + timer + thread + compression
  ↓
Fase C — Protocolo (13-15)
  protocol + rolling_average + statistics
  ↓
Fase D — Input (16)
  input_reader (reusa SDL2 + mapping existente)
  ↓
Fase E — Game hooks + DX9 + frame sync (24-31)
  asm_hacks + dll_hacks + dll_process_manager + mem_dump + change_monitor
  + d3d_hook + frame_rate
  ↓
Fase F — Netplay engine (30-35)
  inputs_container + rollback_manager + rollback_addresses
  + netplay_manager + spectator_manager + dll_main (refatorado)
  ↓
Fase G — Resource ✅ RESOLVIDO
  rollback_addresses.hpp substitui o binary blob
  ↓
Fase H — Integração
  Conectar com IPC receiver existente
  Conectar com config_buffer existente
  Substituir dll_main.cpp atual pelo portado
  Build + test contra MBAA.exe real
```

---

## Status do port (atualizado)

| Fase | Status | Arquivos | LOC | Notas |
|---|---|---|---|---|
| A — Fundação | ✅ Completa | 4 | ~1135 | constants + netplay_states + character_select + messages |
| B — Infra | ✅ Completa | 8 | ~415 | algorithms + string_utils + exceptions + timer + thread + compression + miniz + md5 |
| C — Protocolo | ✅ Completa | 3 | ~200 | protocol dispatcher + rolling_average + statistics |
| D — Input | ✅ Completa + validado | 1 | ~180 | input_reader (reusa mapping.hpp + binder.cpp). **Validado contra MBAA.exe via Wine**: SDL_InitSubSystem(JOYSTICK) acrescentado na DLL, inputs do controle chegam ao jogo. |
| E — Game hooks + DX9 | ✅ Completa + validado | 8 + 3rdparty | ~975 | asm_hacks + frame_rate + dll_process_manager + dll_hacks + mem_dump + change_monitor + MinHook + D3DHook. **Validado**: callback dispara, ASM patches aplicam, frame limiter funciona (via limiter nativo no Wine — hook DX não funciona em Wine, veja ressalva abaixo). |
| F — Netplay engine | 🟡 Foco atual | 4/~6 | ~820/~4500 | inputs_container + rollback_manager + rollback_addresses + **netplay_connector** DONE. Faltam: netplay_manager (FSM), spectator_manager, dll_main refatorado. **Plano detalhado em [phase-f-execution-plan.md](phase-f-execution-plan.md)** — divergências nos arquivos já portados (InputsContainer, NetplayState, RollbackManager, rollback_addresses) documentadas e 7 sub-etapas definidas. |
| G — Resource | ✅ Resolvido | 1 | ~200 | rollback_addresses.hpp substitui o binary blob |
| H — Integração | 🟡 Parcial | — | — | Startup offline (training/versus) **validado contra MBAA.exe**: navegação de menu, force change scene, transição instantânea, injeção de input. Entrega de inputs remotos (netplay transport) implementada, **aguarda teste full-duplex com 2 instâncias**. |

### O que já funciona contra MBAA.exe (validado via Wine)

Estes comportamentos foram verificados rodando `caster.exe --training` com
`hook.dll` injetada em `MBAA.exe` sob Wine, evidência no `debug.log`:

1. **Injeção + callback**: hook.dll injeta sem crash, o hook de main-loop
   dispara `callback()` a cada frame (~60fps confirmado por telemetria).
2. **Skip de configuração**: os patches `0x04A1D42`/`0x04A1D4A` pulam o
   diálogo de config do jogo (aplicados pelo launcher enquanto suspenso).
3. **Force change scene**: `forceGoto` em `0x42B475` é escrito corretamente
   e o jogo chega ao chara-select. **Ressalva**: o patch só executa depois
   que a DLL mash Confirm para navegar pelas telas de Startup/Opening/Title
   até o menu principal — port fiel do `getPreInitialInput` do CCCaster.
4. **Transição instantânea**: `CC_SKIP_FRAMES_ADDR=1` durante a navegação
   de menu faz as telas pré-menu passarem invisíveis em milissegundos.
5. **Frame limiter no Wine**: detectado via `wine_get_version`; quando em
   Wine, a DLL **não** aplica `disableFpsLimit` nem instala o hook de D3D
   Present (que não funciona em Wine), preservando o limiter nativo do jogo.
6. **Injeção de input do controle**: `SDL_InitSubSystem(JOYSTICK)` na DLL
   + `SDL_JoystickOpen` → `read_local_input` → `writeGameInput`. Inputs
   direcionais e de botões chegam ao jogo (confirmado por telemetria).

### Barreira da Fase F (restante)

> **Nota**: Esta seção é mantida como referência histórica do
> diagnóstico inicial. O plano de execução atual e detalhado — com
> divergências linha-a-linha contra o CCCaster e 7 sub-etapas validáveis
> incrementalmente — está em
> [`docs/phase-f-execution-plan.md`](phase-f-execution-plan.md).

O `DllNetplayManager.cpp` (~1268 LOC) e `DllMain.cpp` (~2253 LOC) do CCCaster
são **tão game-specific** que cada função lê endereços de memória do MBAA,
gera inputs sintéticos para cada estado de menu (auto-chara-select, retry
menu navigation, etc.), e orquestra o frame-step loop com rollback.

A fatia de **transporte** já está implementada (`netplay_connector.cpp`:
conecta ENet, recebe/envia `BothInputs`, escreve P2). O que falta é a
**camada de lógica** sobre esse transporte:

1. **`netplay_manager` (FSM)** — o "cérebro". Orquestra os estados
   `NetplayState` (PreInitial → Initial → AutoCharaSelect → CharaSelect →
   Loading → CharaIntro → Skippable → InGame → RetryMenu), gera inputs
   sintéticos para navegação automática de menus (auto-chara-select,
   retry menu), e detecta início de round via `roundStartCounter`.
2. **Rollback** — o `RollbackManager` está completo (`saveState`/`loadState`
   + `rollback_addresses.hpp`), mas **não está plugado** ao frame loop.
   Precisa: chamar `allocateStates()` no início da partida, `saveState()`
   a cada frame in-game, e `loadState()` quando o `InputsContainer` detectar
   que um input remoto divergiu do predito (`getLastChangedFrame`).
3. **Delay de input** — `cfg.delay` é recebido mas ignorado (atualmente
   usa-se delay=0). Aplicar é trivial: offset no `InputsContainer` ao ler
   o input remoto.
4. **`SyncHash` / detecção de desync** — `SyncHash::readFromGame` já está
   implementado em `dll_hacks.cpp`, mas não é trocado com o peer nem
   verificado.
5. **`RngState` sync + `match_seed`** — não aplicados.
6. **`spectator_manager`** — broadcast pra espectadores (~235 LOC).
7. **Reconexão / timeout robusto** — o `netplay_connector` loga
   desconexão mas não reconecta nem trata timeout além do log.

Portar isso corretamente requer **validar contra o jogo real**. A estrutura
está pronta: todos os tipos, constants, messages, process_manager,
rollback_manager, input_reader, e o transporte netplay já estão portados,
compilando, e o offline está validado.

### Pré-requisitos para continuar a Fase F

Os pré-requisitos de build/injeção/input (itens 1-3 abaixo) **já foram
validados** nesta sessão. Falta validar rollback (item 4) e o caminho
netplay full-duplex:

1. ~~**Build no Windows**~~ ✅ Cross-compile MinGW funcionando, binários
   testados contra MBAA.exe via Wine.
2. ~~**Teste de injeção**~~ ✅ ASM patches aplicam, callback dispara,
   frame limiter funciona.
3. ~~**Teste de input**~~ ✅ `input_reader` lê controles e `writeGameInput`
   escreve nos endereços certos.
4. **Teste de rollback** (ainda pendente): validar que `saveState`/`loadState`
   preserva o estado do jogo (save → avança N frames → load → estado
   idêntico). Depende de plugar o `RollbackManager` no frame loop.
5. **Teste netplay full-duplex** (pendente): duas instâncias do caster
   (host + join) na mesma máquina; confirmar que cada lado vê o oponente
   se mover. Requer que o handshake do launcher complete.

Após esses testes, o port do `DllNetplayManager` e `DllMain` pode ser feito
com confiança de que a infraestrutura funciona.

---

## Estimativa de esforço

| Fase | Arquivos | LOC (aprox) | Complexidade | Esforço |
|---|---|---|---|---|
| A — Fundação | 4 | ~1080 | Baixa | 1 dia |
| B — Infra | 8 | ~1300 | Média | 2 dias |
| C — Protocolo (sem socket layer) | 3 | ~765 | Média | 1 dia |
| D — Input (reusa SDL2 + mapping existente) | 1 | ~200 | Baixa | 0.5 dia |
| E — Game hooks + DX9 hook + frame sync | 8 | ~2795 | Muito alta | 4.5 dias |
| F — Netplay engine | 6 | ~4540 | Extrema | 5 dias |
| G — Resource | 1 | — | Alta | 1 dia |
| H — Integração | — | — | Média | 2 dias |
| **Total** | **31** | **~10680** | — | **~17 dias** |

(DX9 hook infrastructure + DllFrameRate + D3DHook adicionados de volta
= +2 arquivos, +465 LOC, +0.5 dia. Sem overlay rendering.)

---

## Riscos e pendências

1. **`binary_res_rollback_bin`**: é um blob binário compilado a partir de
   análise do MBAA.exe. Precisa ser extraído do build do CCCaster ou
   regenerado. Sem isso, rollback não funciona. **É o blocker crítico.**

2. **Protocolo de serialização**: o CCCaster usa `cereal` (header-only).
   Vamos substituir por serialização manual binária (como já fizemos no
   `config_buffer`). Isso exige reescrever todos os `serialize()` methods
   de `Messages.hpp`. ~500 linhas de adaptação.

3. **Compatibilidade de protocolo com CCCaster**: se quisermos que
   ReCaster jogue contra CCCaster, o protocolo de rede precisa ser
   idêntico (message tags, field order, endianness). Se só ReCaster vs
   ReCaster, podemos simplificar.

4. **MinHook no MinGW**: o MinHook do CCCaster é pré-compilado para MinGW.
   Precisa validar que compila com nosso toolchain i686-w64-mingw32.

5. **ASM inline**: `DllAsmHacks` tem inline assembly em sintaxe AT&T
   (`__asm__`). MinGW suporta isso, mas precisa validar.

6. **`callback()` naked hook**: o hook de main-loop usa um `extern "C"
   callback()` que é injetado via `PATCHJUMP`. Precisa de cuidado extra
   pra não corromper o stack do jogo.

---

## Critério de aceite da versão 1

Uma partida online funcional requer:

- [ ] Dois ReCasters em máquinas diferentes conseguem se conectar (direto ou relay)
- [ ] Handshake completa (version + name + ping + config + confirm)
- [ ] Jogo abre nos dois lados com hook.dll injetada
- [ ] Inputs do player 1 aparecem no player 2 e vice-versa
- [ ] Rollback funciona (sem desync em partida estável)
- [ ] Partida termina corretamente (round retry, voltar ao menu)
- [ ] Sem crash em alt-tab / window resize

O que NÃO é necessário na versão 1:
- Overlay dentro do jogo
- Trial mode
- Paletas customizadas
- FPS limiter custom
- Replay save/load
- Spectate
- Hotkeys de debug (F5/F6/F7, Ctrl+0..9)
