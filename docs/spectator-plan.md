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
**Status: Parcialmente feito — BothInputs e getters já existem**
**Corresponde a:** parte da Layer 5 do threading plan

1. ~~Adicionar `BothInputs` em `src/dll/protocol/messages.hpp`~~ ✅ Já existe
2. ~~Adicionar `getBothInputs`/`setBothInputs` no `NetplayManager`~~ ✅ Já existe
3. ~~Adicionar `kFlagSpectator` no IPC config~~ ✅ Já existe (`config_buffer.hpp:48`)
4. Adicionar `SpectateConfig` em `src/dll/protocol/messages.hpp`
   - Struct: delay, rollback, winCount, hostPlayer, names[2], isTraining
5. Adicionar `MsgType::SpectateConfig` no enum + decoder case

**Estimativa restante: ~40 LOC** (apenas SpectateConfig + decoder)

### Fase 2 — Host-side (receber spectators)
**Status: Pendente — requer Layer 4 (network thread)**
**Corresponde a:** Layer 5 do threading plan

6. Criar `src/dll/spec/spectator_manager.hpp` e `.cpp`
   - Portar `DllSpectatorManager.cpp` do CCCaster
   - Roda na **network thread** (aceita + broadcast sem bloquear game)
   - Sem Timer/EventManager — usar `GetTickCount()`
   - Sem Socket* — usar ENet peer IDs
   - Sem mutexes para spectator state (network thread owns it)
7. Integrar `stepSpectators()` no loop da network thread
8. Accept de conexões de spectator (ENet connect event na network thread)
9. Enviar SpectateConfig + InitialGameState + RngState no accept

**Estimativa: ~200 LOC** (CCCaster tem 389, mas sem Timer/Socket/mutex boilerplate)

### Fase 3 — Client-side (ser spectator)
**Status: Pendente — requer Layer 4 + Fase 2**
**Corresponde a:** Layer 6 do threading plan

10. Criar `src/dll/spec/spectate_client.hpp` e `.cpp`
    - Receive BothInputs → push to game thread queue
    - Receive RngState → push to game thread queue
    - Receive MenuIndex → push to game thread queue
11. Integrar no FSM: path `SpectateNetplay` em `getInput()`
    - Não lê input local (spectator não joga)
    - Não resend inputs
    - Auto-select characters se entrar depois do CharaSelect
12. Fast-forward/hard-sync controls (toggle com teclas)
    - Fast-forward: pula frames pra alcançar o presente
    - Hard-sync: espera sincronizar antes de continuar

**Estimativa: ~150 LOC**

### Fase 4 — Launcher/GUI
**Status: Pendente**

13. `--spec=PEER` funcional (direto + relay)
    - CLI: já parseado, só wire up
    - Session: `start_spectate()` (similar a `start_join` mas com flag spectator)
14. GUI: botão "Spectate" na Play page
    - Input field para host:port ou #room
    - Não precisa de "Start Match" (spectator não confirma)
15. ~~IPC config: flag `kFlagSpectator` no `config_buffer`~~ ✅ Já existe
    - DLL já lê a flag via `cfg.is_spectator()` — só precisa ativar o modo

**Estimativa: ~130 LOC** (flag já existe, menos trabalho)

### Fase 5 — Relay spectate
**Status: Pendente**

16. Relay server: aceitar spectators como terceiro tipo de conexão
    - HostRegister (host), ClientJoin (player), SpectateJoin (spectator)
    - Spectator não consome room slot
17. Host redireciona spectators cheios pra outros spectators
    - `getRandomSpectatorAddress()` → redirect
    - Spectator vira relay pra outro spectator

**Estimativa: ~150 LOC**

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
