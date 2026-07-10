# Status do porte da DLL

Porte da `hook.dll` do CCCaster para o ReCaster, focado em **partidas
online funcionais** (v1). Features extras (trial, paletas, overlay, replay
editor) ficam de fora.

## Resumo

| Fase | Status | Descrição |
|---|---|---|
| A — Fundação | ✅ Completa | addresses + states + character_tables + messages |
| B — Infra | ✅ Completa | algorithms + string_utils + exceptions + hash (xxHash128) |
| C — Protocolo | ✅ Completa | decoder + messages |
| D — Input | ✅ Completa + validada | input_reader (reusa `common/controller/mapping`) |
| E — Game hooks + DX9 | ✅ Completa + validada | asm_patches + frame_limiter + game_io + lifecycle + mem_dump + MinHook + D3DHook |
| F — Netplay engine | ✅ Implementada | inputs_container + rollback_manager + manager (FSM) + connector. Spin-lock gate, rollback, RNG sync, SyncHash/desync todos wired. Faltam 4 gaps menores de robustez (ver abaixo). |
| G — Resource | ✅ Resolvida | `rollback_addresses.hpp` substitui o `rollback.bin` blob |
| H — Integração | 🟡 Parcial | Offline validado. Full-duplex netplay aguarda teste real contra MBAA.exe. |

## O que já funciona (validado contra MBAA.exe via Wine)

1. **Injeção + callback**: `hook.dll` injeta sem crash; o hook de main-loop
   dispara `callback()` a cada frame (~60fps).
2. **Skip de configuração**: patches `0x04A1D42`/`0x04A1D4A` pulam o
   diálogo de config do jogo (aplicados pelo launcher enquanto suspenso).
3. **Force change scene**: `forceGoto` em `0x42B475` leva ao chara-select
   após mash de Confirm pelas telas pré-menu.
4. **Transição instantânea**: `CC_SKIP_FRAMES_ADDR=1` durante navegação
   de menu faz as telas pré-menu passarem invisíveis.
5. **Frame limiter no Wine**: detectado via `wine_get_version`; no Wine a
   DLL não aplica `disableFpsLimit` nem instala o hook de D3D Present
   (que não funciona em Wine), preservando o limiter nativo.
6. **Injeção de input do controle**: `SDL_InitSubSystem(JOYSTICK)` na DLL
   + `SDL_JoystickOpen` → `read_local_input` → `writeGameInput`. Inputs
   direcionais e de botões chegam ao jogo.
7. **Handshake do launcher**: `session.cpp` faz version + name + ping +
   config + confirm desde a Fase H.
8. **Runtime test (pós-reorg)**: duas instâncias `caster.exe` (host +
   join em `127.0.0.1:12345`) ficaram ativas por 30s sem crash.

## Motor de netplay — implementação completa

O `frameStep()` em `entry/dll_main.cpp` cobre o ciclo completo de
rollback netplay estilo GGPO, comparável ao `DllMain.cpp` do CCCaster:

| Step | Onde (ReCaster) | Onde (CCCaster) |
|---|---|---|
| ChangeMonitor (gameMode/gameState/roundStart) | `dll_main.cpp:809-850` | DllMain.cpp:964 |
| `updateFrame()` | `dll_main.cpp:779` | DllMain.cpp:960 |
| `checkRoundOver()` | `dll_main.cpp:863-865` | DllMain.cpp:971 |
| `CC_INTRO_STATE_ADDR=0` no rerun | `dll_main.cpp:873-877` | DllMain.cpp:974-976 |
| `hijackIntroState` ASM hack | `asm_patches.cpp:263` + `dll_main.cpp:411` | DllAsmHacks.cpp:503 |
| Read local controller + `setInput` | `dll_main.cpp:943-956` | DllMain.cpp:477 |
| `sendPlayerInputs` to peer | `dll_main.cpp:974-978` | DllMain.cpp:507 |
| Host: generate + send RngState | `dll_main.cpp:993-1000` | DllMain.cpp:514-527 |
| **Spin-lock `isRemoteInputReady && isRngStateReady`** | `dll_main.cpp:1034-1084` | DllMain.cpp:540-581 |
| Resend inputs a cada 100ms | `dll_main.cpp:1064-1072` | DllMain.cpp:568-579 |
| Timeout 10s → `delayedStop("Timed out!")` | `dll_main.cpp:1075-1078` | DllMain.cpp:1945-1946 |
| Apply RngState (`setRngState`) | `dll_main.cpp:1096-1110` | DllMain.cpp:623-646 |
| rollbackTimer countdown | `dll_main.cpp:1121-1125` | DllMain.cpp:583-589 |
| `saveState` cada frame InGame | `dll_main.cpp:1176-1184` | DllMain.cpp:207 |
| Rollback trigger (`loadState` + rerun) | `dll_main.cpp:1186-1220` | DllMain.cpp:591-621 |
| `writeGameInput` ambos players | `dll_main.cpp:1150-1151` | DllMain.cpp:988-989 |
| SyncHash generate + send | `dll_main.cpp:1235-1252` | DllMain.cpp:775-790 |
| SyncHash compare + desync | `dll_main.cpp:1259-1294` | DllMain.cpp:792-831 |
| `CC_DAMAGE_LEVEL`/`TIMER_SPEED`/`WIN_COUNT_VS` | `dll_main.cpp:425-427` | DllMain.cpp:1889-1891 |
| `CC_STAGE_ANIMATION_OFF_ADDR=1` | `dll_main.cpp:412` | DllMain.cpp:1896-1907 |
| 60s initial connect timeout | `dll_main.cpp:762-775` | DllMain.cpp:1843-1844 |
| ENet DISCONNECT → `delayedStop` | `connector.cpp:188` + `dll_main.cpp:579,965` | DllMain.cpp:1348-1365 |

Os 7 pontos anteriormente listados como "blockers" (teste de sanidade
pós-F.1–F.7) estão **todos implementados** — foram verificados por grep
contra o código atual.

## Gaps remanescentes (4 itens de robustez)

Nenhum é blocker para uma partida online funcionar. São diferenças
menores vs CCCaster, todas de robustez/cosmético.

| # | Gap | Severidade | Estimativa | Onde corrigir |
|---|---|---|---|---|
| 1 | `delayedStop` notificar launcher via IPC | UX | ~20 LOC | `entry/dll_main.cpp:239` — hoje só loga; launcher não vê o motivo da parada |
| 2 | `callback()` checar `CC_ALIVE_FLAG_ADDR` | Robustez | ~8 LOC | `entry/dll_main.cpp:1304` — detectar game-close e desconectar sockets limpo (constante já existe em `addresses.hpp:87`) |
| 3 | `clearInputs()` no início do frame | Low | ~3 LOC | `entry/dll_main.cpp:779` — CCCaster escreve `0,0` nos addrs de input antes do ChangeMonitor (defensivo) |
| 4 | SyncHash schedule off-by-one | Cosmético | ~1 LOC | `entry/dll_main.cpp:1243` — usa `%150==0`, CCCaster usa `%150==149` (diferença de 1 frame a cada 2.5s) |

**Total: ~32 LOC, <0.5 dia.** Todos podem vir antes ou depois do teste
real contra MBAA.exe — não são blockers.

> **Nota arquitetural**: CCCaster é event-driven/multi-threaded
> (background `EventManager` + `Timer` callbacks). ReCaster é
> single-threaded/synchronous (`netplay::poll()` no game thread +
> `GetTickCount` wall-clock). Ambos são corretos e funcionalmente
> equivalentes para v1; o do ReCaster é mais simples.

## Critérios de aceite da v1

- [ ] Dois ReCasters em máquinas diferentes conseguem se conectar (direto ou relay)
- [x] Handshake completa (version + name + ping + config + confirm)
- [x] Jogo abre nos dois lados com `hook.dll` injetada
- [ ] Inputs do P1 aparecem no P2 e vice-versa (código pronto, aguarda teste real)
- [ ] Rollback funciona sem desync em partida estável (código pronto, aguarda teste real)
- [ ] Partida termina corretamente (round retry, voltar ao menu)
- [ ] Sem crash em alt-tab / window resize (não testado; DX9 hook não funciona em Wine)

**Fora de escopo v1**: overlay dentro do jogo, trial mode, paletas
customizadas, FPS limiter custom (no Wine), replay save/load, spectate,
hotkeys de debug (F5/F6/F7, Ctrl+0..9).

## Próximos passos

1. **Teste two-instance full-duplex contra MBAA.exe real via Wine** —
   o código está pronto. Injetar `hook.dll` em duas instâncias do
   MBAA.exe, host + join em `127.0.0.1`, confirmar que cada lado vê o
   oponente se mover e que rollback dispara em caso de lag artificial.
2. **Implementar os 4 gaps** de robustez (~32 LOC) — podem vir antes ou
   depois do teste, conforme o que ele revelar.
3. **Cleanup de código morto** — ver `docs/stubs.md` seção "Módulos
   portados mas não integrados". ~400 LOC candidatas a deleção, manter
   apenas a API surface de spectator (post-v1).
