# Spectator Mode — Plano de Implementação

## Visão Geral

Spectator mode permite que jogadores assistam partidas em andamento sem
participar. O spectator conecta ao host, recebe inputs de ambos os
players (BothInputs) e replaya a partida em tempo real.

**Modos suportados:**
- `SpectateNetplay` — late-join numa partida em andamento (direto ou relay)

**Modo REMOVIDO:**
- `SpectateBroadcast` — não vamos implementar (ninguém usava no CCCaster)

## Diferença arquitetural: CCCaster vs ReCaster

O CCCaster é **multi-threaded event-driven**: tem um `EventManager` num
background thread que processa Timer callbacks e Socket callbacks de forma
assíncrona. O ReCaster é **single-threaded síncrono**: tudo roda no game
thread via `callback()` → `step()`, com `GetTickCount()` para timeouts e
ENet `poll()` para rede.

Isso significa que o port do SpectatorManager será **MENOR** que o
original do CCCaster (~389 LOC), porque não precisa de:
- Timer boilerplate (TimerPtr, timerExpired, _pendingTimerToSocket map)
- Thread safety (mutexes, atomics)
- Socket callback registration (tudo é poll no step())

O que adiciona LOC vs CCCaster:
- Mensagens novas no protocolo (SpectateConfig, BothInputs): +80 LOC
- Launcher/GUI integration: +150 LOC
- Relay spectate (CCCaster usava TCP direto, não relay): +150 LOC

## Estrutura de Arquivos

```
src/dll/spec/
├── spectator_manager.hpp    # Host-side: gerencia connections de spectators
├── spectator_manager.cpp    # Implementação (broadcast round-robin)
├── spectate_client.hpp      # Client-side: recebe BothInputs e replaya
├── spectate_client.cpp      # Implementação
└── spec_messages.hpp        # Mensagens: SpectateConfig, BothInputs
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

## O que o ReCaster já tem (stubs)

| Item | Estado |
|---|---|
| `is_spectator` flag em `NetplayConfig` | Existe mas nunca setado |
| `--spec=PEER` CLI flag | Parseado, relay spectate rejeitado |
| `InitialGameState::readFromGame` | Existe em `lifecycle.cpp` |
| `getFullCharaName` | Existe em `character_tables` |
| `AutoCharaSelect` state | Existe no FSM mas sem implementação |
| `getBothInputs`/`setBothInputs` | NÃO existe (mapeado no stubs.md) |
| `SpectateConfig` message | NÃO existe no protocolo |
| `BothInputs` message | NÃO existe no protocolo |

## Fases de Implementação

### Fase 1 — Protocolo (sem rede)
**Status: Pendente**

1. Adicionar `SpectateConfig` e `BothInputs` em `src/dll/protocol/messages.hpp`
2. Adicionar `getBothInputs`/`setBothInputs` no `NetplayManager`
3. Adicionar `kFlagSpectator` no `NetplayConfig`
4. Adicionar decoder cases em `src/dll/protocol/decoder.cpp`

**Estimativa: ~120 LOC**

### Fase 2 — Host-side (receber spectators)
**Status: Pendente**

5. Criar `src/dll/spec/spectator_manager.hpp` e `.cpp`
   - Portar `DllSpectatorManager.cpp` do CCCaster
   - Sem Timer/EventManager — usar GetTickCount + step()
   - Sem Socket* — usar ENet peer IDs
   - Sem mutexes — single-threaded
6. Integrar `stepSpectators()` no `frameStep()` do `dll_main.cpp`
7. Accept de conexões de spectator no `drainNetplayInbox()`
8. Enviar SpectateConfig + InitialGameState + RngState no accept

**Estimativa: ~200 LOC** (CCCaster tem 389, mas sem Timer/Socket/mutex boilerplate)

### Fase 3 — Client-side (ser spectator)
**Status: Pendente**

9. Criar `src/dll/spec/spectate_client.hpp` e `.cpp`
   - Receive BothInputs → unpack → writeGameInput (ambos players)
   - Receive RngState → setRngState
   - Receive MenuIndex → setRetryMenuIndex
10. Integrar no FSM: path `SpectateNetplay` em `getInput()`
    - Não lê input local (spectator não joga)
    - Não resend inputs
    - Auto-select characters se entrar depois do CharaSelect
11. Fast-forward/hard-sync controls (toggle com teclas)
    - Fast-forward: pula frames pra alcançar o presente
    - Hard-sync: espera sincronizar antes de continuar

**Estimativa: ~150 LOC**

### Fase 4 — Launcher/GUI
**Status: Pendente**

12. `--spec=PEER` funcional (direto + relay)
    - CLI: já parseado, só wire up
    - Session: `start_spectate()` (similar a `start_join` mas com flag spectator)
13. GUI: botão "Spectate" na Play page
    - Input field para host:port ou #room
    - Não precisa de "Start Match" (spectator não confirma)
14. IPC config: flag `kFlagSpectator` no `config_buffer`
    - DLL lê a flag e ativa modo spectator

**Estimativa: ~150 LOC**

### Fase 5 — Relay spectate
**Status: Pendente**

15. Relay server: aceitar spectators como terceiro tipo de conexão
    - HostRegister (host), ClientJoin (player), SpectateJoin (spectator)
    - Spectator não consome room slot
16. Host redireciona spectators cheios pra outros spectators
    - `getRandomSpectatorAddress()` → redirect
    - Spectator vira relay pra outro spectator

**Estimativa: ~150 LOC**

## Adaptações CCCaster → ReCaster

| CCCaster | ReCaster | Impacto |
|---|---|---|
| `EventManager` (bg thread, callbacks) | `step()` no game thread | -50 LOC (sem callback boilerplate) |
| `Timer` + `TimerPtr` | `GetTickCount()` | -30 LOC (sem timer registration) |
| `Socket*` + `SocketPtr` | ENet peer + reliable packets | -40 LOC (sem socket management) |
| Mutexes / thread safety | Single-thread, sem mutex | -15 LOC |
| `SpectateBroadcast` mode | REMOVIDO | -50 LOC |
| Relay spectate (novo) | Adaptar relay protocol | +150 LOC |
| Protocolo (SpectateConfig, BothInputs) | Novo no ReCaster | +80 LOC |

## Total Estimado (corrigido)

| Fase | LOC | Sessões |
|---|---|---|
| 1 — Protocolo | ~120 | 1 |
| 2 — Host-side | ~200 | 1 |
| 3 — Client-side | ~150 | 1 |
| 4 — Launcher/GUI | ~150 | 1 |
| 5 — Relay spectate | ~150 | 1 |
| **Total** | **~770** | **4-5** |

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

1. **Fase 1** primeiro (protocolo) — sem dependências, habilita testes
2. **Fase 2 + 3** juntas (host + client) — uma não funciona sem a outra
3. **Fase 4** (launcher) — habilita uso real
4. **Fase 5** (relay) — nice-to-have, spectate direto funciona sem
