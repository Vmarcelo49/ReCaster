# Spectator Mode — Plano de Implementação

## Visão Geral

Spectator mode permite que jogadores assistam partidas em andamento sem
participar. O spectator conecta ao host, recebe inputs de ambos os
players (BothInputs) e replaya a partida em tempo real.

**Modos suportados:**
- `SpectateNetplay` — late-join numa partida em andamento (direto ou relay)

**Modo REMOVIDO:**
- `SpectateBroadcast` — não vamos implementar (ninguém usava no CCCaster)

## Pré-requisito: DLL-side threading

**O spectator mode requer que a DLL (hook.dll) seja multi-threaded.**
O plano de threading está em `docs/threading-migration.md` (Part 2,
Layers 4-6). O spectator é implementado nas Layers 5 e 6, que dependem
da Layer 4 (network thread foundation).

**Por que threading é necessário:**
- O `frameStep()` atual tem um spin-lock que bloqueia a game thread por
  até 10s. Spectators não conseguem conectar durante o spin-lock.
- O broadcast round-robin pra 15 spectators toma tempo do `frameStep()`,
  causando hitching.
- Disconnects do peer só são detectados no início do próximo frame.

Sem threading, spectator só funcionaria com workarounds frágeis (timeout
extending, partial accept, etc.). Com threading, spectator é natural.

**Veja:** `docs/threading-migration.md` → Part 2 → Layer 4 (network
thread foundation) deve ser concluído antes de iniciar qualquer fase
de spectator.

## Diferença arquitetural: CCCaster vs ReCaster

O CCCaster é **multi-threaded event-driven**: tem um `EventManager` num
background thread que processa Timer callbacks e Socket callbacks de forma
assíncrona. O ReCaster DLL é atualmente **single-threaded síncrono**,
mas será migrado para uma arquitetura hybrid (1 network thread + game
thread) conforme o plano em `docs/threading-migration.md` (Part 2).

Após a migração (Layer 4 completa), o port do SpectatorManager será
**MENOR** que o original do CCCaster (~389 LOC), porque não precisa de:
- Timer boilerplate (TimerPtr, timerExpired, _pendingTimerToSocket map)
- Thread safety (mutexes, atomics)
- Socket callback registration (tudo é poll no step())

O que adiciona LOC vs CCCaster:
- Mensagem nova no protocolo (SpectateConfig): +40 LOC
  - BothInputs já existe no protocolo (`MsgType::BothInputs = 1` em
    `messages.hpp`, usado pelo rollback engine)
- Launcher/GUI integration: +150 LOC
- Relay spectate (CCCaster usava TCP direto, não relay): +150 LOC

## Estrutura de Arquivos

```
src/dll/spec/
├── spectator_manager.hpp    # Host-side: gerencia connections de spectators
├── spectator_manager.cpp    # Implementação (broadcast round-robin)
├── spectate_client.hpp      # Client-side: recebe BothInputs e replaya
├── spectate_client.cpp      # Implementação
└── spec_messages.hpp        # Mensagem: SpectateConfig (BothInputs já existe)
```

## Como funciona (fluxo)

### Spectator (client-side)
1. Launcher conecta ao host (direto ou relay)
2. Host envia `SpectateConfig` (delay, rollback, winCount, mode flags)
3. Host envia `InitialGameState` (indexedFrame, netplayState, isTraining)
4. Host envia `RngState` (estado do RNG no ponto de entrada)
5. Host faz broadcast de `BothInputs` (inputs P1+P2 combinados) em round-robin
6. Spectator não envia inputs — só recebe e replaya via `writeGameInput`

### Host (host-side)
1. Accepta conexão de spectator → pending (20s timeout via GetTickCount)
2. Promove pra spectator ativo → envia SpectateConfig + InitialGameState + RngState
3. A cada frame, `stepSpectators()` faz broadcast round-robin:
   - Intervalo baseado no número de spectators
   - Envia `BothInputs` para 1+ spectators por frame via ENet reliable
   - Envia `RngState` e `MenuIndex` uma vez por index change
4. Spectator disconnect → remove do map

### Limites
- `MAX_SPECTATORS = 15` (quando o client IS a spectator)
- `MAX_ROOT_SPECTATORS = 1` (quando o client é host/player)
- Redirect: quando cheio, redireciona novo spectator pra outro spectator

## O que o ReCaster já tem

| Item | Estado |
|---|---|
| `is_spectator` flag em `NetplayConfig` | ✅ Existe em `netplay_config.hpp:17`, nunca setado em runtime |
| `kFlagSpectator` no IPC config | ✅ Existe em `config_buffer.hpp:48` (bit 3), com helper `is_spectator()` |
| `--spec=PEER` CLI flag | ✅ Parseado em `cli_args.hpp`, relay spectate rejeitado |
| `InitialGameState::readFromGame` | ✅ Existe em `lifecycle.cpp:244` |
| `getFullCharaName` | ✅ Existe em `character_tables.hpp:17` |
| `AutoCharaSelect` state | ✅ Existe no FSM (`states.hpp:25`) com transições, `getAutoCharaSelectInput` em `manager.hpp:315` |
| `BothInputs` message | ✅ Existe: `MsgType::BothInputs = 1` + struct com serialize/deserialize (`messages.hpp:30,119-130`) |
| `getBothInputs`/`setBothInputs` | ✅ Existe em `manager.hpp:186,190` — usado pelo rollback engine |
| `SpectateConfig` message | ❌ NÃO existe no protocolo — precisa ser criado |
| `SpectatorManager` (host-side) | ❌ NÃO existe — precisa ser portado do CCCaster |
| `SpectateClient` (client-side) | ❌ NÃO existe — precisa ser criado |

## Fases de Implementação

**Pré-requisito:** `docs/threading-migration.md` Layer 4 (network thread
foundation) deve estar completa. As fases abaixo correspondem às Layers
5-6 do threading plan.

### Fase 1 — Protocolo (sem rede)
**Status: ✅ Completa (2026-07-14)**
**Corresponde a:** parte da Layer 5 do threading plan

1. ~~Adicionar `BothInputs` em `src/dll/protocol/messages.hpp`~~ ✅ Já existe
2. ~~Adicionar `getBothInputs`/`setBothInputs` no `NetplayManager`~~ ✅ Já existe
3. ~~Adicionar `kFlagSpectator` no IPC config~~ ✅ Já existe (`config_buffer.hpp:48`)
4. ✅ Adicionar `SpectateConfig` em `src/dll/protocol/messages.hpp`
   - Struct: delay, rollback, rollbackDelay, winCount, hostPlayer, isTraining, names[2]
   - Wire: `[tag=16][u8 delay][u8 rollback][u8 rollbackDelay][u8 winCount][u8 hostPlayer][u8 isTraining][u8 name0Len][name0][u8 name1Len][name1]`
5. ✅ Adicionar `MsgType::SpectateConfig = 16` no enum + decoder case
6. ✅ Build clean (MinGW cross-compile, zero warnings/errors)

**LOC real: ~50** (struct + serialize/deserialize inline + decoder case)

### Fase 2 — Host-side (receber spectators)
**Status: ✅ Completa (2026-07-14) — classe criada, falta integração com NetworkThread**
**Corresponde a:** Layer 5 do threading plan

6. ✅ Criar `src/dll/spec/spectator_manager.hpp` e `.cpp`
   - Portado de `DllSpectatorManager.cpp` do CCCaster (~235 LOC)
   - Adaptado para Layer 4: ENetPeer* em vez de Socket*, GetTickCount() em
     vez de Timer, sem EventManager boilerplate
   - `mutable std::mutex _outMutex` protege tanto o outbox quanto o
     spectator state (single coarse mutex — adequado para
     MAX_ROOT_SPECTATORS=1, contention zero)
   - Outbox queue: game thread pusha (promotePending, frameStepSpectators,
     newRngState), network thread popa (tryPopOut)
7. ⬜ Integrar `stepSpectators()` no loop da network thread (Fase 2.5)
8. ⬜ Wire up `onSpectatorConnect/Disconnect` no NetworkThread::loop
   ENet event dispatch (Fase 2.5)
9. ✅ Enviar SpectateConfig + InitialGameState + RngState no promotePending
10. ✅ Build clean (MinGW cross-compile, zero warnings/errors)

**LOC real: ~260** (era estimado ~200; um pouco maior por causa dos
comentários de threading e do outbox queue)

### Fase 2.5 — Integração NetworkThread + dll_main
**Status: ✅ Completa (2026-07-14)**
**Corresponde a:** parte da Layer 5 do threading plan

7. ✅ `NetworkThread::loop` chama `spectatorMgr_->step()` a cada iteração
8. ✅ `NetworkThread::loop` CONNECT event: distingue opponent (primeiro
   peer) de spectator (peer subsequente). Delega pro SpectatorManager.
9. ✅ `NetworkThread::loop` DISCONNECT event: delega pro SpectatorManager
   se for spectator.
10. ✅ `NetworkThread::loop` drena `tryPopOut` e chama `enet_peer_send`
    com o peer específico de cada pacote.
11. ✅ `enet_host_create` agora usa peerCapacity=16 quando host (1 player
    + 15 spectators), 2 quando client.
12. ✅ `connector.cpp` ganhou `initSpectatorManager(netMan)` — chamado
    por dll_main depois de `netplay::start()` apenas se host.
13. ✅ `dll_main.cpp frameStep` chama `frameStepSpectators()` se host.
14. ✅ Build clean (MinGW cross-compile, zero warnings/errors).

**LOC real: ~120** (NetworkThread ~80 + connector ~30 + dll_main ~10)

### Fase 3 — Client-side (ser spectator)
**Status: ✅ Completa (2026-07-14)**
**Corresponde a:** Layer 6 do threading plan

15. ✅ Criar `src/dll/spec/spectate_client.hpp` e `.cpp`
    - `onSpectateConfig` → configura NetplayManager (delay, rollback,
      hostPlayer, names, isTraining, mode=SpectateNetplay)
    - `onInitialGameState` → escreve chara/moon/color/stage na game
      memory + força FSM pra AutoCharaSelect
    - `onRngState` → forwarda pra `NetplayManager::setRngState`
    - `onMenuIndex` → forwarda pra `setRetryMenuIndex`
    - `onBothInputs` → forwarda pra `setBothInputs` (replay de inputs)
16. ✅ 3 novos inboxes no NetworkThread: `inboxBothInputs_`,
    `inboxInitialGameState_`, `inboxSpectateConfig_`
17. ✅ `dll_main.cpp drainNetplayInbox` drena os 3 novos inboxes e
    forwarda pro `g_spectateClient` quando `g_isSpectator`
18. ✅ `g_isSpectator` derivado de `cfg.is_spectator()` em `doIpcAndModePatch`
19. ✅ `g_spectateClient` criado quando spectator
20. ✅ Build clean (MinGW cross-compile, zero warnings/errors)

**LOC real: ~200** (spectate_client ~150 + dll_main drain ~30 +
network_thread inboxes ~20)

**Pendências (para validação runtime):**
- ⬜ `promotePending()` não é chamado automaticamente — precisa de UI
  no launcher (Fase 4) pra aceitar spectators
- ⬜ `frameStepRerun` não foi adaptado pra spectator — spectator não
  faz rollback, só replay. Pode ser que funcione como está (spectator
  não tem inputs locais pra prever), mas precisa validação runtime.
- ⬜ `getInput()` dispatcher não tem path SpectateNetplay — retorna 0
  por default, que é o correto pra spectator (não joga). Verificar
  se isso chega ao `writeGameInput` corretamente.

### Fase 4 — Launcher/GUI
**Status: ✅ Completa (2026-07-14)**

13. ✅ `--spec=PEER` funcional (direto)
    - CLI: `start_spectate_async(host, port)` em `cli.cpp`
    - Session: `StartSpectate` command no variant, handler em `apply_command`
    - Seta `is_spectator = true`, `is_netplay = false` no NetplayConfig
14. ✅ GUI: botão "Spectate" na Play page
    - `do_spectate` em `play_page.cpp` agora chama `start_spectate_async`
    - Transiciona pra `WaitingForPeer` (mesma tela de waiting do join)
15. ~~IPC config: flag `kFlagSpectator` no `config_buffer`~~ ✅ Já existia
    - `game_runner.cpp:270-272` já seta `kFlagSpectator` quando `np_cfg.is_spectator`
16. ✅ Auto-promote de pending spectators
    - `SpectatorManager::promoteAllPending()` — snapshot dos pending peers,
      chama `promotePending` pra cada um
    - `dll_main.cpp frameStep` chama `promoteAllPending()` se host e state != PreInitial
17. ✅ Build clean (MinGW cross-compile, zero warnings/errors)

**LOC real: ~180** (session ~60 + cli ~30 + play_page ~40 + spectator_manager ~30 + dll_main ~20)

### Fase 5 — Relay spectate
**Status: ✅ Completa (2026-07-14)**

18. ✅ `start_relay_spectate_async(relay_source, room_code)` no session
    - `StartRelaySpectate` command no variant
    - Handler em `apply_command` — idêntico a `StartRelayJoin` mas com
      `is_spectator = true`
    - Relay trata spectator como client normal (mesmo room code do host)
19. ✅ CLI: `--spec=#room` agora funciona (antes rejeitava)
    - `cli.cpp` Mode::Spectate aceita `#room` e chama `start_relay_spectate_async`
20. ✅ GUI: `do_spectate` em `play_page.cpp` aceita `#room`
    - Chama `start_relay_spectate_async` quando `parsed.type == RoomCode`
21. ✅ Build clean (MinGW cross-compile, zero warnings/errors)

**LOC real: ~120** (session ~80 + cli ~20 + play_page ~20)

**Como funciona sem mudar o relay server:**
- O relay server (Go) não precisa saber que é spectator — trata como client normal
- O spectator pede hole-punch pro mesmo room code do host
- O host recebe a conexão UDP e identifica como spectator porque o oponente
  já está conectado (primeiro peer = oponente, peer subsequente = spectator)
- Isso funciona porque o ENet host do host já aceita até 16 peers (Fase 2.5)
- Limitação: só 1 spectator direto por host (MAX_ROOT_SPECTATORS=1). Para
  mais spectators, precisaria de relay chain (spectator relay pra outro
  spectator) — não implementado, mas a base está pronta.

## Adaptações CCCaster → ReCaster

| CCCaster | ReCaster | Impacto |
|---|---|---|
| `EventManager` (bg thread, callbacks) | Network thread (Layer 4) + `step()` no game thread | -50 LOC (sem callback boilerplate) |
| `Timer` + `TimerPtr` | `GetTickCount()` | -30 LOC (sem timer registration) |
| `Socket*` + `SocketPtr` | ENet peer + reliable packets | -40 LOC (sem socket management) |
| Mutexes / thread safety | Network thread owns spectator state (no shared mutex) | -15 LOC |
| `SpectateBroadcast` mode | REMOVIDO | -50 LOC |
| Relay spectate (novo) | Adaptar relay protocol | +150 LOC |
| Protocolo (SpectateConfig) | BothInputs já existe, só SpectateConfig é novo | +40 LOC |

## Total Estimado (corrigido)

| Fase | LOC | Sessões | Depende de |
|---|---|---|---|
| 1 — Protocolo | ~40 (restante) | 1 | — |
| 2 — Host-side | ~200 | 1 | Threading Layer 4 + Fase 1 |
| 3 — Client-side | ~150 | 1 | Threading Layer 4 + Fase 2 |
| 4 — Launcher/GUI | ~130 | 1 | — |
| 5 — Relay spectate | ~150 | 1 | Fase 2-3 |
| **Total** | **~670** | **4-5** | |

**Pré-requisito de threading:** ~650 LOC (Layers 4-6 do
`threading-migration.md`). Fases 2-3 do spectator correspondem às
Layers 5-6 do threading plan, então o LOC de spectator já está incluído
no total de threading. O LOC adicional de spectator é apenas Fases 1
(protocolo) + 4 (launcher) + 5 (relay) = ~320 LOC.

## Pontos de integração no dll_main.cpp

| Local | Mudança |
|---|---|
| `frameStep()` | Chamar `stepSpectators()` se host com spectators |
| `frameStep()` | Se spectator: não ler input local, não resend, fast-forward |
| `drainNetplayInbox()` | Aceitar SpectateConfig, BothInputs, InitialGameState |
| `getInput()` dispatcher | Path SpectateNetplay: retorna 0 (sem input local) |
| `callback()` | Se spectator + alive flag: não stop |
| `loadMappings()` post-load | Se spectator: pular controller init |

## Ordem recomendada

1. **Threading Layer 4** (network thread foundation) — pré-requisito
   absoluto. Sem isso, spectator não funciona. Veja
   `docs/threading-migration.md` Part 2.
2. **Fase 1** (protocolo) — sem dependências de threading, pode ser
   feito antes ou em paralelo com Layer 4
3. **Fase 2 + 3** juntas (host + client) — correspondem às Layers 5-6
   do threading plan. Uma não funciona sem a outra.
4. **Fase 4** (launcher) — habilita uso real
5. **Fase 5** (relay) — nice-to-have, spectate direto funciona sem
