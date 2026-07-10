# Status do porte da DLL

Porte da `hook.dll` do CCCaster para o ReCaster, focado em **partidas
online funcionais** (v1). Features extras (trial, paletas, overlay, replay
editor) ficam de fora.

## Resumo

| Fase | Status | Descrição |
|---|---|---|
| A — Fundação | ✅ Completa | addresses + states + character_tables + messages |
| B — Infra | ✅ Completa | algorithms + string_utils + exceptions + timer + thread + hash (xxHash128) |
| C — Protocolo | ✅ Completa | decoder + rolling_average + statistics |
| D — Input | ✅ Completa + validada | input_reader (reusa `common/controller/mapping`) |
| E — Game hooks + DX9 | ✅ Completa + validada | asm_patches + frame_limiter + game_io + lifecycle + mem_dump + MinHook + D3DHook |
| F — Netplay engine | 🟡 Implementada, faltam 7 pontos | inputs_container + rollback_manager + manager (FSM) + connector. Ver blockers abaixo. |
| G — Resource | ✅ Resolvida | `rollback_addresses.hpp` substitui o `rollback.bin` blob |
| H — Integração | 🟡 Parcial | Offline validado. Full-duplex netplay aguarda os 7 blockers. |

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

## Blockers da Fase F (7 pontos críticos)

Identificados no teste de sanidade pós-implementação (F.1–F.7). Os 3
primeiros são **blockers de rollback** — sem eles, rollback não dispara
ou causa re-execução de intros. Os demais são robustez/UX.

| # | Ponto | Severidade | Estimativa |
|---|---|---|---|
| 1 | `isRemoteInputReady` gate no `frameStep` | **CRÍTICO** | ~10 LOC |
| 2 | Portar `hijackIntroState` ASM hack | Alto | ~15 LOC |
| 3 | `CC_INTRO_STATE_ADDR = 0` durante rollback rerun | Alto | ~5 LOC |
| 4 | `resendTimer` / `waitInputsTimer` (timeout de peer sumido) | Médio | ~40 LOC |
| 5 | Setar `CC_DAMAGE_LEVEL`/`CC_TIMER_SPEED`/`CC_WIN_COUNT_VS` | Médio | ~5 LOC |
| 6 | `CC_STAGE_ANIMATION_OFF_ADDR = 1` em rollback | Baixo | ~3 LOC |
| 7 | `initialTimer` (60s connect timeout) | Baixo | ~15 LOC |

**Total: ~90 LOC, ~0.5 dia.**

### Detalhes dos blockers

**#1 — `isRemoteInputReady` gate** (CRÍTICO)
O `frameStep` do ReCaster não chama `netMan.isRemoteInputReady()` antes
de avançar o frame. O CCCaster bloqueia o frameStep até
`isRemoteInputReady() && isRngStateReady()`. Sem o gate, o jogo processa
frames sem input remoto; a predição é "confirmada" pelo avanço do frame
e o rollback nunca dispara → desync permanente.
**Correção**: gate no `entry/dll_main.cpp::frameStep()`, retornar early
quando `g_isNetplay && !g_netMan.isRemoteInputReady()`.

**#2 — `hijackIntroState`** (Alto)
ASM hack do CCCaster (`DllAsmHacks.cpp:503`, endereço `0x45C1F2`, NOP 7
bytes) que dá controle manual sobre `CC_INTRO_STATE_ADDR`. Sem ele, o
jogo controla o intro state automaticamente e conflita com o rollback
(ponto #3). **Correção**: portar para `hooks/asm_patches.cpp`, aplicar
quando `netMan.getRollback()` é habilitado.

**#3 — `CC_INTRO_STATE_ADDR = 0` no rerun** (Alto)
CCCaster (`DllMain.cpp:975-976`) força `*CC_INTRO_STATE_ADDR = 0` durante
rollback rerun quando `frame > CC_PRE_GAME_INTRO_FRAMES` (224). Sem isso,
o jogo re-executa a intro cinematic durante o rerun. **Correção**: no
`frameStep`, antes do path de rerun. Depende do #2.

**#4 — Timeout de peer sumido** (Médio)
Se o peer parar de mandar inputs, o ReCaster não tem timeout. Com o gate
#1, o frameStep ficaria bloqueado para sempre; sem o gate, avança com
predição permanente até desync. CCCaster reenvia o último `PlayerInputs`
a cada 100ms (`RESEND_INPUTS_INTERVAL`) e faz `delayedStop("Timed out!")`
após 10s (`MAX_WAIT_INPUTS_INTERVAL`). **Correção**: contador no
`netplay/connector` ou no `entry/dll_main.cpp`.

**#5 — Defaults de gameplay** (Médio)
CCCaster seta explicitamente `*CC_DAMAGE_LEVEL_ADDR = 2`,
`*CC_TIMER_SPEED_ADDR = 2`, `*CC_WIN_COUNT_VS_ADDR = config.winCount ? : 2`
em `doIpcAndModePatch`. Sem isso, se os dois lados tiverem configs
diferentes (editaram `_App.ini`), gameplay diverge. **Correção**: no
`doIpcAndModePatch` após popular `g_netMan.config`.

**#6 — Stage animations off** (Baixo)
CCCaster seta `*CC_STAGE_ANIMATION_OFF_ADDR = 1` em rollback. Stage
animations são stateful e podem divergir entre host/client durante
rollback. Cosmético, não causa desync de gameplay. **Correção**: no
`doIpcAndModePatch` quando `g_netMan.getRollback()` é true.

**#7 — Connect timeout** (Baixo)
CCCaster tem timer de 60s (`INITIAL_CONNECT_TIMEOUT`) que faz
`delayedStop("Disconnected!")` se o peer nunca conectar. Sem isso, se o
peer nunca conectar (firewall, endereço errado), o ReCaster espera para
sempre. **Correção**: contador de frames desde `netplay::start()`; se
exceder 3600 frames (60s × 60fps) sem `netplay::connected()`,
`delayedStop`.

## Critérios de aceite da v1

- [ ] Dois ReCasters em máquinas diferentes conseguem se conectar (direto ou relay)
- [ ] Handshake completa (version + name + ping + config + confirm)
- [x] Jogo abre nos dois lados com `hook.dll` injetada
- [ ] Inputs do P1 aparecem no P2 e vice-versa (depende do blocker #1)
- [ ] Rollback funciona sem desync em partida estável (depende dos blockers #1, #2, #3)
- [ ] Partida termina corretamente (round retry, voltar ao menu)
- [ ] Sem crash em alt-tab / window resize (não testado; DX9 hook não funciona em Wine)

**Fora de escopo v1**: overlay dentro do jogo, trial mode, paletas
customizadas, FPS limiter custom (no Wine), replay save/load, spectate,
hotkeys de debug (F5/F6/F7, Ctrl+0..9).

## Próximos passos

1. Implementar os 3 blockers (#1, #2, #3) — sem eles, qualquer teste
   contra MBAA.exe vai parecer que a Fase F inteira está quebrada.
2. Testar two-instance full-duplex (host + join em `127.0.0.1`) contra
   MBAA.exe real via Wine.
3. Implementar #4–#7 em paralelo ou após testes iniciais.
