# Teste de Sanidade — Fase F (pós-implementação)

Este documento é o resultado do teste de sanidade feito após completar
as 7 etapas da Fase F (commits `4040091` → `13989a0`). O objetivo é
identificar pontos críticos que podem ter sido esquecidos durante o
port, antes de partir para os testes contra MBAA.exe via Wine.

A revisão foi feita comparando o `frameStep()` do ReCaster
(`src/dll/dll_main.cpp`) contra o `frameStep()` + `frameStepNormal()` do
CCCaster (`targets/DllMain.cpp:957-919`), linha por linha, mais a
verificação dos critérios de aceite do `dll-port-plan.md`.

---

## Resumo executivo

**7 pontos críticos identificados**, ordenados por severidade:

| # | Ponto | Severidade | Status |
|---|---|---|---|
| 1 | `isRemoteInputReady` gate | **CRÍTICO** | Faltante |
| 2 | `hijackIntroState` ASM hack | Alto | Faltante |
| 3 | `CC_INTRO_STATE_ADDR = 0` durante rollback | Alto | Faltante |
| 4 | `resendTimer` / `waitInputsTimer` timeout | Médio | Faltante |
| 5 | `CC_DAMAGE_LEVEL` / `CC_TIMER_SPEED` / `CC_WIN_COUNT_VS` | Médio | Faltante |
| 6 | `CC_STAGE_ANIMATION_OFF_ADDR = 1` | Baixo | Faltante |
| 7 | `initialTimer` (60s connect timeout) | Baixo | Faltante |

Os 3 primeiros são **blockers** para o rollback funcionar — sem eles, o
rollback nunca dispara corretamente ou causa re-execução de intros
durante o rerun. Os demais são robustez/corretude de gameplay.

---

## Detalhamento

### 1. `isRemoteInputReady` gate — CRÍTICO

**Onde**: `src/dll/dll_main.cpp::frameStep()`

**Problema**: O frameStep do ReCaster NÃO chama `netMan.isRemoteInputReady()`
antes de avançar o frame. O CCCaster (DllMain.cpp:540-581) tem um loop
poll que bloqueia o frameStep até `isRemoteInputReady() && isRngStateReady()`
retornarem true.

**Impacto**: Sem esse gate, o jogo processa frames sem ter o input
remoto. O `InputsContainer::get()` retorna `lastInputBefore` (predição),
mas o `setInputs(remotePlayer, ...)` que chegar depois nunca dispara
`_lastChangedFrame` (porque a predição já foi "confirmada" pelo avanço
do frame). Resultado: **rollback nunca dispara**, e qualquer divergência
real entre predição e input remoto fica permanente → desync.

**Correção**: Adicionar gate no frameStep (após ChangeMonitor, antes do
step 4). Retornar early (sem escrever inputs) quando `g_isNetplay &&
!g_netMan.isRemoteInputReady()`.

**Estimativa**: ~10 LOC.

### 2. `hijackIntroState` ASM hack — Alto

**Onde**: `src/dll/asm_hacks.{hpp,cpp}` (não portado)

**Problema**: O CCCaster tem `hijackIntroState` (AsmHacks em
DllAsmHacks.cpp:503) que dá controle manual sobre `CC_INTRO_STATE_ADDR`.
Sem ele, o jogo controla o intro state automaticamente, o que conflita
com a tentativa do rollback de forçar `*CC_INTRO_STATE_ADDR = 0` durante
o rerun (ponto #3 abaixo).

**Impacto**: Durante rollback rerun, se o jogo está num estado de intro
(pós-round-start), o jogo pode re-executar a cinematic de intro em vez
de pular direto pro gameplay. Isso causa atraso visual e potencial
desync de estado.

**Correção**: Portar `hijackIntroState` de CCCaster (endereço 0x45C1F2,
NOP 7 bytes). Aplicar quando `netMan.getRollback()` é habilitado (em
`doIpcAndModePatch` ou `netplayStateChanged` ao entrar InGame).

**Estimativa**: ~15 LOC (Asm definition + write no lifecycle).

### 3. `CC_INTRO_STATE_ADDR = 0` durante rollback — Alto

**Onde**: `src/dll/dll_main.cpp::frameStep()` (ausente)

**Problema**: CCCaster (DllMain.cpp:975-976) faz:
```cpp
if (netMan.isInRollback() && netMan.getFrame() > CC_PRE_GAME_INTRO_FRAMES
    && *CC_INTRO_STATE_ADDR)
    *CC_INTRO_STATE_ADDR = 0;
```

Isso força o intro state pra 0 (in-game) durante o rerun do rollback
quando já passamos dos frames de pre-game intro (224). Sem isso, o jogo
pode re-executar a intro cinematic durante o rerun.

**Impacto**: Rollback rerun pode re-executar intros, causando atraso
visual e potencial desync.

**Correção**: Adicionar no frameStep, antes do step 2b (rerun path).
Depende do ponto #2 (`hijackIntroState`) pra funcionar corretamente.

**Estimativa**: ~5 LOC.

### 4. `resendTimer` / `waitInputsTimer` timeout — Médio

**Onde**: `src/dll/dll_main.cpp` (ausente) + `src/dll/netplay_connector.hpp`

**Problema**: Se o peer parar de mandar inputs (lag spike, packet loss,
ou crash silencioso), o ReCaster não tem timeout. Com o gate do ponto
#1 implementado, o frameStep ficaria bloqueado pra sempre esperando
input remoto. Sem o gate, o jogo avança com predição permanente até
um desync compelar.

CCCaster (DllMain.cpp:574-579, 1941-1946) reenvia o último
`PlayerInputs` a cada 100ms (`RESEND_INPUTS_INTERVAL`) e faz
`delayedStop("Timed out!")` após 10s (`MAX_WAIT_INPUTS_INTERVAL`).

**Impacto**: Partida pode travar indefinidamente ou desyncar se o peer
sumir silenciosamente.

**Correção**: Implementar contador de frames sem input novo no
`netplay_connector` ou no `dll_main`. Quando exceder, reenviar último
`PlayerInputs`. Após N reenvios sem resposta, `delayedStop("Timed out!")`.

**Estimativa**: ~40 LOC.

### 5. `CC_DAMAGE_LEVEL` / `CC_TIMER_SPEED` / `CC_WIN_COUNT_VS` — Médio

**Onde**: `src/dll/dll_main.cpp::doIpcAndModePatch()` (ausente)

**Problema**: CCCaster (DllMain.cpp:1889-1891) seta explicitamente:
```cpp
*CC_DAMAGE_LEVEL_ADDR = 2;
*CC_TIMER_SPEED_ADDR = 2;
*CC_WIN_COUNT_VS_ADDR = (uint32_t)(netMan.config.winCount ? netMan.config.winCount : 2);
```

Esses são os defaults do jogo (damage level 2, timer speed 2, best-of-2),
mas o CCCaster seta explicitamente pra garantir que os dois lados têm
os mesmos valores. Sem isso, se um lado tiver valores diferentes (por
exemplo, editou o `_App.ini` do jogo), os dois lados terão gameplay
diferente.

**Impacto**: Potencial desync de gameplay se os dois lados tiverem
configurações de jogo diferentes. O `winCount` em particular afeta
quantos rounds são necessários pra vitória.

**Correção**: Adicionar no `doIpcAndModePatch` após popular
`g_netMan.config`, antes do `forceGoto`. Ler `winCount` de
`g_netMan.config.winCount`.

**Estimativa**: ~5 LOC.

### 6. `CC_STAGE_ANIMATION_OFF_ADDR = 1` — Baixo

**Onde**: `src/dll/dll_main.cpp` (ausente)

**Problema**: CCCaster (DllMain.cpp:1903-1906) seta
`*CC_STAGE_ANIMATION_OFF_ADDR = 1` quando rollback está habilitado
e a option `StageAnimations` está ligada. Stage animations (animais
no fundo do stage, etc.) são stateful e podem divergir entre host e
client durante rollback.

**Impacto**: Desync visual em stages com animações. Provavelmente não
causa desync de gameplay, mas é cosmético.

**Correção**: Adicionar no `doIpcAndModePatch` quando `g_netMan.getRollback()`
é true. Sem option (sempre ligado em v1).

**Estimativa**: ~3 LOC.

### 7. `initialTimer` (60s connect timeout) — Baixo

**Onde**: `src/dll/dll_main.cpp` (ausente) + `src/dll/netplay_connector`

**Problema**: CCCaster (DllMain.cpp:1843-1844, 1948-1952) tem um timer
de 60s (`INITIAL_CONNECT_TIMEOUT`) que faz `delayedStop("Disconnected!")`
se o peer nunca conectar. Sem isso, se o peer nunca conectar (firewall,
endereço errado, etc.), o ReCaster fica esperando pra sempre.

**Impacto**: UX ruim — usuário não sabe que a conexão falhou. Não é
desync, mas é necessário pra usabilidade.

**Correção**: Adicionar contador de frames desde `netplay::start()`.
Se exceder 60s * 60fps = 3600 frames sem `netplay::connected()`, fazer
`delayedStop("Initial connect timeout")`.

**Estimativa**: ~15 LOC.

---

## Critérios de aceite — status

Do `dll-port-plan.md`:

| Critério | Status | Notas |
|---|---|---|
| Dois ReCasters conseguem se conectar (direto ou relay) | 🟡 Provável | Handshake do launcher já funciona (Fase H). Falta testar full-duplex. |
| Handshake completa (version + name + ping + config + confirm) | ✅ Implementado | `session.cpp` já faz isso desde Fase H. |
| Jogo abre nos dois lados com hook.dll injetada | ✅ Validado | Fase E + H, validado contra MBAA.exe via Wine. |
| Inputs do P1 aparecem no P2 e vice-versa | 🟡 Depende do #1 | Sem `isRemoteInputReady` gate, inputs podem não sincronizar corretamente. |
| Rollback funciona (sem desync em partida estável) | 🔴 Blockers #1, #2, #3 | Sem esses 3, rollback não dispara ou causa re-execução de intros. |
| Partida termina corretamente (round retry, voltar ao menu) | 🟡 F.6 implementou | RetryMenu flow completo, mas depende do rollback funcionar. |
| Sem crash em alt-tab / window resize | ❓ Não testado | DX9 hook não funciona em Wine (ressalva da Fase E). |

---

## Recomendação

Implementar os 3 blockers (#1, #2, #3) **antes de qualquer teste contra
MBAA.exe** — sem eles, o rollback não vai funcionar e os testes vão
parecer que a Fase F inteira está quebrada, quando na verdade faltam
peças pequenas e localizadas.

Os pontos #4-#7 podem ser implementados em paralelo ou depois dos
testes iniciais — são robustez/UX, não blockers de funcionalidade.

Estimativa total pra implementar os 7 pontos: ~90 LOC, ~0.5 dia.

Após implementar, a Fase F estará pronta para testes contra MBAA.exe
via Wine com two-instance full-duplex (dois casters na mesma máquina,
host + join, confirmar que cada lado vê o oponente se mover).
