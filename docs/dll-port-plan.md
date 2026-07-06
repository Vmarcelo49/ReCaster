# Plano: Port da DLL (hook.dll) — Versão 1: Partidas Online

Este documento descreve o plano para portar a DLL do CCCaster para o ReCaster,
focando exclusivamente no que é necessário para **partidas online funcionarem**.
Features adicionais (trial mode, paletas customizadas, overlay de combo,
frame rate limiter, replay editor) ficam de fora desta versão.

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

### Prioridade 3 — Networking (já temos大部分, adaptar/conectar)

| # | Arquivo origem | Arquivo destino | LOC | Descrição |
|---|---|---|---|---|
| 13 | `lib/Protocol.hpp/.cpp` + `lib/ProtocolEnums.hpp` | `src/dll/protocol.{hpp,cpp}` | ~590 | Framework de serialização. **Adaptar**: sem cereal — serialização manual binária (como config_buffer). |
| 14 | `lib/IpAddrPort.hpp/.cpp` | `src/dll/ip_addr_port.{hpp,cpp}` | ~260 | Endereço IP+porta. |
| 15 | `lib/Socket.hpp` + `lib/TcpSocket.hpp/.cpp` + `lib/UdpSocket.hpp/.cpp` + `lib/SocketManager.hpp/.cpp` | `src/dll/socket.{hpp,cpp}` | ~1600 | Camada de sockets. **Adaptar**: podemos reusar nosso ENet transport pra parte disso, mas a DLL do CCCaster usa sockets raw (TCP+UDP com GoBackN). **Decisão**: portar o socket layer do CCCaster direto — ele é game-agnostic. |
| 16 | `lib/GoBackN.hpp/.cpp` | `src/dll/go_back_n.{hpp,cpp}` | ~480 | Reliable UDP via Go-Back-N ARQ. |
| 17 | `lib/SmartSocket.hpp/.cpp` | `src/dll/smart_socket.{hpp,cpp}` | ~740 | Socket que tenta TCP/UDP direto + cai pra UDP tunnel. **Adaptar**: nosso relay_client já faz isso — podemos não precisar portar. |
| 18 | `lib/RollingAverage.hpp` | `src/dll/rolling_average.hpp` | ~70 | Média móvel. Template puro. |
| 19 | `lib/Statistics.hpp` | `src/dll/statistics.hpp` | ~105 | Estatísticas online (Welford). |

### Prioridade 4 — Input

| # | Arquivo origem | Arquivo destino | LOC | Descrição |
|---|---|---|---|---|
| 20 | `lib/Controller.hpp/.cpp` | `src/dll/controller.{hpp,cpp}` | ~1100 | Abstrai keyboard + joystick. Mantém mappings, deadzone, SOCD, estado. **Adaptar**: usar SDL2 (já temos). |
| 21 | `lib/ControllerManager.hpp/.cpp` | `src/dll/controller_manager.{hpp,cpp}` | ~990 | Singleton que detecta/atribui controllers, polling thread. **Adaptar**: já temos `mapping.hpp` — estender. |
| 22 | `lib/KeyboardManager.hpp/.cpp` | `src/dll/keyboard_manager.{hpp,cpp}` | ~280 | Hook global de teclado. |
| 23 | `lib/KeyboardState.hpp/.cpp` | `src/dll/keyboard_state.{hpp,cpp}` | ~150 | Estado de teclas (down/pressed/held/released). |
| 24 | `lib/JoystickDetector.hpp/.cpp` | `src/dll/joystick_detector.{hpp,cpp}` | ~250 | Detecta attach/detach de joysticks. **Adaptar**: SDL já faz isso. |
| 25 | `lib/KeyboardVKeyNames.hpp` | `src/dll/vkey_names.hpp` | ~35 | VK codes → nomes. |
| 26 | `targets/DllControllerUtils.hpp` | `src/dll/controller_utils.hpp` | ~110 | `filterSimulDirState()` (SOCD) + `convertInputState()` (bitmask → numpad+buttons). |
| 27 | `targets/DllControllerManager.hpp/.cpp` | `src/dll/dll_controller_manager.{hpp,cpp}` | ~1150 | Gerencia 2 players, hotkeys (F3/F4/F5), input injection. **Adaptar**: REMOVER trial menu overlay + mapping editor overlay. |

### Prioridade 5 — Game hooks (o coração da DLL)

| # | Arquivo origem | Arquivo destino | LOC | Descrição |
|---|---|---|---|---|
| 28 | `targets/DllAsmHacks.hpp` | `src/dll/asm_hacks.hpp` | ~605 | Catálogo de patches. **Adaptar**: REMOVER patches de palette, trial, SFX filter, screenshot. Manter: hookMainLoop, hijackControls, hijackMenu, detectRoundStart, multiWindow, hijackEscapeKey, enableDisabledStages. |
| 29 | `targets/DllAsmHacks.cpp` | `src/dll/asm_hacks.cpp` | ~615 | Implementação dos patches + callbacks. **Adaptar**: REMOVER palette/PNG/trial callbacks. Manter: `Asm::write/revert`, `hookMainLoop`, `hijackControls`, `hijackMenu`, `detectRoundStart`. |
| 30 | `targets/DllHacks.hpp/.cpp` | `src/dll/dll_hacks.{hpp,cpp}` | ~335 | Lifecycle: `initializePreLoad()`, `initializePostLoad()`, `deinitialize()`. WindowProc hook via MinHook. **Adaptar**: REMOVER DX9 hook (sem overlay nesta versão). Manter: WindowProc hook (keyboard/mouse/device-change), `InitialGameState` ctor, `SyncHash` ctor. |
| 31 | `targets/DllProcessManager.cpp` | `src/dll/dll_process_manager.cpp` | ~120 | `writeGameInput()`, `getRngState()`, `setRngState()`, `connectPipe()`. **Adaptar**: já temos `ipc_receiver` — estender. |
| 32 | `lib/MemDump.hpp/.cpp` | `src/dll/mem_dump.{hpp,cpp}` | ~510 | Save/restore de ranges de memória para rollback. |
| 33 | `lib/ChangeMonitor.hpp/.cpp` | `src/dll/change_monitor.{hpp,cpp}` | ~210 | Monitora mudanças em endereços de memória (dispara callbacks quando values mudam). |

### Prioridade 6 — Netplay engine (o cérebro)

| # | Arquivo origem | Arquivo destino | LOC | Descrição |
|---|---|---|---|---|
| 34 | `targets/DllNetplayManager.hpp` | `src/dll/netplay_manager.hpp` | ~240 | Classe `NetplayManager`: inputs containers, RNG states, indexed frame, state transitions. |
| 35 | `targets/DllNetplayManager.cpp` | `src/dll/netplay_manager.cpp` | ~1270 | FSM lógica: `frameStepNormal()`, `getCharaSelectInput()`, `getInGameInput()`, etc. **Adaptar**: REMOVER UDP debug logger (porta 17474). |
| 36 | `netplay/InputsContainer.hpp` | `src/dll/inputs_container.hpp` | ~195 | Template `InputsContainer<T>` — mapa indexado frame→input. |
| 37 | `targets/DllRollbackManager.hpp/.cpp` | `src/dll/rollback_manager.{hpp,cpp}` | ~350 | Save/load de estado via memory pool. **Adaptar**: REMOVER SFX history (mute-during-reroll). Manter: `saveState()`, `loadState()`, `RepInputContainer` fixup. **Depende**: `binary_res_rollback_bin` (precisa ser extraído do CCCaster ou regenerado). |
| 38 | `targets/DllSpectatorManager.cpp` | `src/dll/spectator_manager.cpp` | ~235 | Broadcast de inputs para espectadores. **Pode ser cortado** se spectate não for prioridade — mas é genérico o suficiente pra portar direto. |
| 39 | `targets/DllMain.cpp` | `src/dll/dll_main.cpp` (refatorar) | ~2255 | Orquestrador central: `frameStepNormal()`, `frameStepRollback()`, `frameStepSpectator()`, callbacks de socket, `callback()` (hook de main-loop). **Adaptar**: REMOVER trial manager, replay manager, frame-step hotkeys (F5/F6/F7), delay/rollback hotkeys (Ctrl+0..9), broadcast mode. Manter: netplay FSM principal, socket callbacks, IPC. |

### Prioridade 7 — Resource binário (rollback states)

| # | Item | Descrição |
|---|---|---|
| 40 | `res/rollback.o` (binary blob) | Lista de (address, size) pairs que o RollbackManager salva/restaura. **Precisa ser extraído do build do CCCaster ou regenerado** a partir de análise do MBAA.exe. Sem isso, rollback não funciona. É um array de structs `{void* addr; size_t size;}` compilado como resource. |

---

## O que NÃO VAMOS portar (versão 1)

### Features adicionais (fora do escopo de partidas online)

| Arquivo origem | LOC | Razão da exclusão |
|---|---|---|
| `targets/DllTrialManager.hpp/.cpp` | ~2100 | Trial mode (combo training). Feature adicional, não necessária pra partidas online. |
| `targets/DllPaletteManager.cpp` | ~50 | Paletas customizadas via PNG. Feature cosmética. |
| `targets/DllFrameRate.hpp/.cpp` | ~165 | FPS limiter custom. O jogo já tem seu próprio limitador. |
| `targets/DllOverlayUi.hpp/.cpp` | ~157 | Overlay de texto (3 colunas). Sem overlay nesta versão. |
| `targets/DllOverlayUiImGui.cpp` | ~116 | Overlay ImGui de debug. |
| `targets/DllOverlayUiText.cpp` | ~535 | Overlay de texto + trial combo text. |
| `targets/DllOverlayPrimitives.hpp` | ~103 | Primitivas de desenho D3D9. |
| `targets/oCallDraw.c` + `targets/CallDraw.s` | ~215 | Wrappers ASM pra funções de draw internas do jogo. Só usado por trial. |
| `netplay/ReplayCreator.hpp/.cpp` | ~770 | Criação de replay. Legacy, não referenciado pelo pipeline ativo. |
| `netplay/PaletteManager.hpp/.cpp` | ~340 | Gerenciamento de paletas. Feature cosmética. |

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

### Decisão de arquitetura: ENet vs sockets raw

O CCCaster usa uma camada própria de sockets (`lib/Socket.hpp` + TCP/UDP +
GoBackN + SmartSocket). O ReCaster já tem ENet integrado e um `relay_client`
com hole-punching.

**Opção A**: Portar o socket layer inteiro do CCCaster (~3000 linhas) e
usar na DLL. Vantagem: compatibilidade de protocolo com o CCCaster.
Desvantagem: duplicação de código.

**Opção B**: Adaptar a DLL pra usar ENet (que já temos). Vantagem: menos
código. Desvantagem: incompatível com o protocolo do CCCaster — ReCaster
só joga com ReCaster.

**Recomendação**: **Opção A** pra versão 1 (copiar o socket layer direto).
Garante que o protocolo é idêntico ao CCCaster e podemos testar contra
relay servers existentes. Migração pra ENet pode acontecer depois.

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
Fase C — Networking (13-19)
  protocol + ip_addr_port + socket + go_back_n + smart_socket
  + rolling_average + statistics
  ↓
Fase D — Input (20-27)
  controller + controller_manager + keyboard_manager + keyboard_state
  + joystick_detector + vkey_names + controller_utils + dll_controller_manager
  ↓
Fase E — Game hooks (28-33)
  asm_hacks + dll_hacks + dll_process_manager + mem_dump + change_monitor
  ↓
Fase F — Netplay engine (34-39)
  netplay_manager + inputs_container + rollback_manager
  + spectator_manager + dll_main (refatorado)
  ↓
Fase G — Resource (40)
  Extrair/regenerar rollback_bin
  ↓
Fase H — Integração
  Conectar com IPC receiver existente
  Conectar com config_buffer existente
  Substituir dll_main.cpp atual pelo portado
  Build + test
```

---

## Estimativa de esforço

| Fase | Arquivos | LOC (aprox) | Complexidade | Esforço |
|---|---|---|---|---|
| A — Fundação | 4 | ~1080 | Baixa | 1 dia |
| B — Infra | 8 | ~1300 | Média | 2 dias |
| C — Networking | 7 | ~3780 | Alta | 3 dias |
| D — Input | 8 | ~3995 | Média | 3 dias |
| E — Game hooks | 6 | ~2330 | Muito alta | 4 dias |
| F — Netplay engine | 6 | ~4540 | Extrema | 5 dias |
| G — Resource | 1 | — | Alta | 1 dia |
| H — Integração | — | — | Média | 2 dias |
| **Total** | **40** | **~17025** | — | **~21 dias** |

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
