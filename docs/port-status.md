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
| F — Netplay engine | ✅ Implementada + validada end-to-end | inputs_container + rollback_manager + manager (FSM) + connector. Spin-lock gate, rollback, RNG sync, SyncHash/desync todos wired. **Partida completa validada** (round 1 + round 2 + match end, rollback 1653+ disparos, 60fps, sem crash, sem desync). |
| G — Resource | ✅ Resolvida | `rollback_addresses.hpp` substitui o `rollback.bin` blob |
| H — Integração | ✅ Validada | Netplay full-duplex validado entre duas máquinas reais (PC1 + PC2 em redes diferentes via Wine). |
| Cleanup | ✅ Concluído | Código morto removido (~625 LOC). Logs diagnósticos removidos. Ver `docs/stubs.md`. |

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
8. **Netplay full-duplex entre máquinas distintas**: PC1 (host) e PC2
   (joiner, rede diferente) conectaram via ENet direto, jogaram partida
   completa sem crash. Inputs cruzam nos dois sentidos.
9. **Rollback netplay funcional**: durante a partida validada, o
   `RollbackManager` disparou **1653+ rollbacks** sem desync, mantendo
   60fps estáveis. O `saveState`/`loadState` agora é estável em todas as
   transições de round graças ao fix do dangling parent pointer em
   `MemDumpPtr` (commit `c78f938`).
10. **Transição round 1 → round 2 → match end**: o crash de round 2
    estava resolvido pelo fix `c78f938`. Partida completa (todos os
    rounds + vitória final) confirmada em PC2 (Wine 10.0).
11. **Spin-lock latency otimizada**: primeira iteração de poll usa
    `Sleep(1)` em vez de `Sleep(3)`, reduzindo o tempo de spin-lock
    por frame de 12-13ms para ~1ms quando o input remoto já está no
    buffer ENet.
12. **Logger estruturado**: `caster::dll::netplay_debug` escreve em
    `caster/host_debug.log` / `caster/join_debug.log` separadamente,
    com throttle adaptativo (60 frames em InGame estável, every-frame
    durante rollback/rerun). Logs de diagnóstico temporários foram
    removidos pós-validação.

## Motor de netplay — implementação completa e validada

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
| `saveState` cada frame InGame | `dll_main.cpp:1289-1291` | DllMain.cpp:207 |
| Rollback trigger (`loadState` + rerun) | `dll_main.cpp:1293-1325` | DllMain.cpp:591-621 |
| `writeGameInput` ambos players | `dll_main.cpp:1150-1151` | DllMain.cpp:988-989 |
| SyncHash generate + send | `dll_main.cpp:1235-1252` | DllMain.cpp:775-790 |
| SyncHash compare + desync | `dll_main.cpp:1259-1294` | DllMain.cpp:792-831 |
| `CC_DAMAGE_LEVEL`/`TIMER_SPEED`/`WIN_COUNT_VS` | `dll_main.cpp:425-427` | DllMain.cpp:1889-1891 |
| `CC_STAGE_ANIMATION_OFF_ADDR=1` | `dll_main.cpp:412` | DllMain.cpp:1896-1907 |
| 60s initial connect timeout | `dll_main.cpp:762-775` | DllMain.cpp:1843-1844 |
| ENet DISCONNECT → `delayedStop` | `connector.cpp:188` + `dll_main.cpp:579,965` | DllMain.cpp:1348-1365 |
| `clearLastChangedFrame()` antes do drain | `dll_main.cpp` (pré-spin-lock) | DllMain.cpp:537-538 |
| `isRemoteInputReady` predição no InGame | `manager.cpp` | DllMain.cpp:540-581 |

Os 7 pontos anteriormente listados como "blockers" (teste de sanidade
pós-F.1–F.7) estão **todos implementados** e foram validados em partida
real.

## Bugs críticos resolvidos durante a validação

| Commit | Bug | Sintoma | Root cause |
|---|---|---|---|
| `6c83816` | `clearLastChangedFrame` ordering | Rollback nunca disparava | `clear()` era chamado depois do `drainNetplayInbox()`, apagando a divergência antes do check |
| `6c83816` | `isRemoteInputReady` deadlock InGame | Ambos peers travavam no spin-lock na transição simultânea para InGame | Remote não tinha frames ainda para o novo index; agora permite avanço via predição |
| `6c83816` | saveState access violation | Crash nos primeiros frames InGame | Pointers em heap do jogo não inicializados; mitigado inicialmente pulando saveState nos primeiros 10 frames |
| `aa92eaa` | Spin-lock latency | ~3fps slowdown em CharaSelect | `Sleep(3)` na primeira iteração; trocado para `Sleep(1)` |
| `c78f938` | **Dangling parent pointer em `MemDumpPtr`** | **Crash no round 2 (Wine 10.0)** — virtual call através de ponteiro destruído | Re-parenting recursivo quebrado pela mudança de children by-value para `shared_ptr` (workaround de limitação Clang/LLVM-MinGW). Levels 2-3 mantinham parent apontando para temporários destruídos da initializer-list. **Fix**: remover o campo `parent` armazenado e passar `parentAddr` como parâmetro explícito em `getAddr`/`saveDump`/`loadDump` |
| `80af1ac` | (Diagnóstico) | Confirmou que round 2 já estava resolvido por `c78f938` | Logging temporário das 3 primeiras `saveState` por session — removido no cleanup atual |

## Gaps remanescentes (4 itens de robustez)

Nenhum é blocker para uma partida online funcionar. São diferenças
menores vs CCCaster, todas de robustez/cosmético.

| # | Gap | Severidade | Estimativa | Onde corrigir |
|---|---|---|---|---|
| 1 | `delayedStop` notificar launcher via IPC | UX | ~20 LOC | `entry/dll_main.cpp:239` — hoje só loga; launcher não vê o motivo da parada |
| 2 | `callback()` checar `CC_ALIVE_FLAG_ADDR` | Robustez | ~8 LOC | `entry/dll_main.cpp:1304` — detectar game-close e desconectar sockets limpo (constante já existe em `addresses.hpp:87`) |
| 3 | `clearInputs()` no início do frame | Low | ~3 LOC | `entry/dll_main.cpp:779` — CCCaster escreve `0,0` nos addrs de input antes do ChangeMonitor (defensivo) |
| 4 | SyncHash schedule off-by-one | Cosmético | ~1 LOC | `entry/dll_main.cpp:1243` — usa `%150==0`, CCCaster usa `%150==149` (diferença de 1 frame a cada 2.5s) |

**Total: ~32 LOC, <0.5 dia.** Não são blockers — podem ser feitos antes ou
depois das próximas validações.

> **Nota arquitetural (atualizada 2026-07-15)**: CCCaster é event-driven/multi-threaded
> (background `EventManager` + `Timer` callbacks). ReCaster **também é
> multi-threaded agora** após a migração Layer 4 (commit `027d9ee`,
> 2026-07-14): a DLL tem um network jthread dedicado (dono do `ENetHost*`)
> + game thread (D3D9/game memory/SDL2/rollback). O `netplay::poll()` no
> game thread foi removido — virou no-op, o network thread drena ENet
> internamente. O launcher também é multi-threaded (Layers 0-3:
> NetplaySession worker + GameRunner worker, cada com jthread + BlockingQueue
> + snapshot). Ver `docs/threading-migration.md` para o plano completo.

## Issue conhecida — Rematch / RetryMenu

Após o match end, o jogo entra no `RetryMenu`. O auto-input atual
continua mandando o padrão de InGame (A + direções), mas o `RetryMenu`
provavelmente espera **CONFIRM** em vez de A para acionar o rematch.
Sintoma observado: o jogo não faz rematch automaticamente, ou cai
de volta para o character select em vez de iniciar um novo match.

**Investigação em andamento** (próxima etapa):
- Mapear o estado `RetryMenu` no `NetplayManager` (já existe na FSM?)
- Verificar qual botão o jogo espera nesse estado (provável: CONFIRM
  em vez de A)
- Ajustar o auto-input em `dll_main.cpp` para gerar CONFIRM quando
  `getState() == RetryMenu`
- Validar que ambos peers fazem rematch sincronizado

## Cleanup concluído

Esta etapa removeu código morto e logs diagnósticos temporários:

- **625 LOC removidos** entre arquivos deletados e símbolos mortos
  limpos de headers/shared cpps.
- **6 arquivos deletados** (todos header-only ou não compilados):
  `rolling_average.hpp`, `thread.hpp`, `timer.hpp`, `statistics.hpp`,
  `change_monitor.hpp`, `dll_stubs.cpp`.
- **~14 símbolos mortos removidos** de `algorithms.hpp`, `string_utils`,
  `character_tables`, `hash`.
- **Logs diagnósticos removidos**: bloco `saveState: idx=X frm=Y
  allocated=Z` em `dll_main.cpp` e getter `isAllocated()` em
  `rollback_manager.hpp` (commit `80af1ac` era só diagnóstico; o fix
  real foi o `c78f938`).
- **Logger estruturado permanente** (`debug_log.hpp`) mantido — é a
  solução de logging de produção, não diagnóstico.
- Ver `docs/stubs.md` para o detalhamento arquivo-a-arquivo.

## Critérios de aceite da v1

- [x] Dois ReCasters em máquinas diferentes conseguem se conectar (direto ou relay)
- [x] Handshake completa (version + name + ping + config + confirm)
- [x] Jogo abre nos dois lados com `hook.dll` injetada
- [x] Inputs do P1 aparecem no P2 e vice-versa
- [x] Rollback funciona sem desync em partida estável (1653+ rollbacks, 60fps)
- [x] Partida termina corretamente (round 1 → round 2 → match end)
- [ ] **Rematch automático no RetryMenu** (issue conhecida — ver acima)
- [ ] Sem crash em alt-tab / window resize (não testado; DX9 hook não funciona em Wine)

**Fora de escopo v1**: trial mode, paletas customizadas, FPS limiter custom
(no Wine), replay save/load, spectate, hotkeys de debug (F5/F6/F7, Ctrl+0..9).

## DX9 Overlay (Fase 1 — em progresso)

Porte do overlay DX9 do CCCaster (`DllOverlayUi*.{hpp,cpp}` +
`DllOverlayPrimitives.hpp`), categoria (C) no inventário. A Fase 1 cobre
o porte fiel do text overlay 3-colunas com state machine e animação de
altura. Trial mode (categoria D) e ImGui debug window ficam para fases
posteriores.

### Arquivos novos

| Arquivo | Origem (CCCaster) | Descrição |
|---|---|---|
| `src/dll/overlay/overlay_ui.hpp` | `DllOverlayUi.hpp` | API pública: init/enable/disable/toggle, updateText, showMessage, presentFrameBegin, invalidateDeviceObjects |
| `src/dll/overlay/overlay_ui.cpp` | `DllOverlayUi.cpp` + `DllOverlayUiText.cpp` | State machine (Disabled→Enabling→Enabled→Disabling) + lazy DX init + renderização 3-col + mensagens temporárias. Skip: trial mode, ImGui |
| `src/dll/overlay/primitives.hpp` | `DllOverlayPrimitives.hpp` | Helpers D3D9: drawRectangle, drawBox, drawCircle, drawText, textCalcRect |

### Arquivos modificados

| Arquivo | Mudança |
|---|---|
| `src/dll/util/algorithms.hpp` | Adicionado `clamped()` (não existia no ReCaster; necessário para a animação de altura) |
| `src/dll/entry/dll_main.cpp` | `PresentFrameBegin` e `InvalidateDeviceObjects` stubs agora chamam o overlay. Callback per-frame chama `overlay::updateText()`/`updateMessage()` para animar a altura e fazer tick no timeout |
| `src/dll/entry/lifecycle.cpp` | Após `HookDirectX()` com sucesso: `overlay::init()` (arma lazy init) + `overlay::updateText({placeholder})` + `overlay::enable()` (boot ligado) |
| `CMakeLists.txt` | Adicionado `src/dll/overlay/overlay_ui.cpp` ao target `hook` |

### Decisões de design (Fase 1)

- **Estado inicial**: ligado no boot (`overlay::enable()` chamado em
  `initializePostLoad`). Mostra placeholder "ReCaster | DX9 Overlay v0.1"
  até que o netplay wiring seja conectado.
- **Hotkey**: nenhuma por enquanto — só API programática
  (`enable`/`disable`/`toggle`). Hotkey (provável F1) vem quando
  integrarmos com o input reader.
- **g_desiredText**: introduzido para desacoplar "texto desejado pelo
  caller" de "texto sendo renderizado". O state machine do CCCaster
  original só armazena o texto em certos estados (Enabled/Enabling-done),
  o que quebra quando o caller seta o texto antes do `enable()`. O
  `g_desiredText` persiste across state transitions.
- **Sem Wine (atualizado)**: o overlay **funciona no Wine** após a
  mudança para vtable swap. Ver seção "Mudança arquitetural" abaixo. O
  `frame_rate::enable()` (custom FPS limiter) ainda é desativado no Wine
  porque depende do hook de Present que desabilita o limiter nativo do
  jogo — mas o overlay em si não tem essa dependência.

### Validação

- [x] Syntax-check passou (g++-14 -fsyntax-only com stubs mínimos de
      windows.h/d3d9.h/d3dx9.h)
- [x] Cross-compile MinGW32 (mingw-w64 16.1.0 + cmake 4.4.0 no Arch Linux)
- [x] **Runtime no Wine 11.13** — overlay visível dentro do jogo, log confirma:
      `DX9 hook installed (Wine)` → `overlay armed` → `overlay: DirectX resources initialized (font + background VB)`
- [x] Sem crash, sem erros, saída limpa (`rc=0`)
- [ ] Teste no Windows nativo (DX9 hook via vtable swap deve funcionar
      igual; code-overwrite original não era necessário)
- [ ] Wire com NetplayManager para mostrar ping/FPS/estado real

### Mudança arquitetural: vtable swap (necessária para Wine)

Durante a validação no Wine, descobrimos que o mecanismo original de
hooking do `D3DHook.cc` (**code-overwrite** via `CHookJump` — escreve
um `JMP rel32` no início da função) **não funciona no Wine**. O Wine
implementa D3D9 sobre OpenGL/Vulkan, e as funções da vtable vivem em
`wined3d.dll` — o `VirtualProtect` + write no código da função não
intercepta as chamadas.

A solução foi reescrever `D3DHook.cc` para usar **vtable swap**: em vez
de patchear o código da função, swapamos o ponteiro dentro da vtable do
`IDirect3DDevice9`. Isso funciona em Windows nativo E no Wine porque
ambos implementam vtables COM da mesma forma. Como a vtable está em
página read-only (`.rdata`), usamos `VirtualProtect` para torná-la
temporariamente writable antes do swap.

A mudança afeta 4 funções em `3rdparty/d3dhook/D3DHook.cc`:
- `InitDirectX` — agora salva ponteiros diretos da vtable (não offsets
  relativos a DLL)
- `HookDirectX` — vtable swap com VirtualProtect (era code-overwrite)
- `UnhookDirectX` — restaura entradas da vtable (era RemoveHook)
- `DX9_HooksInit` / `DX9_HooksVerify` — adicionado VirtualProtect para
  AddRef/Release (sem isso, crash no Wine por page fault em write)

A mudança em `lifecycle.cpp` separa o `frame_rate::enable()` (que
desabilita o limiter nativo do jogo — ainda não funciona no Wine) do
`HookDirectX()` + overlay (que agora funcionam no Wine).

### Próximas fases do overlay

2. **Wire com NetplayManager** — `frameStep()` ou `callback()` constrói
   o array de 3 strings com info real (ping, FPS, NetplayState,
   rollback count) e chama `overlay::updateText(text)`.
3. **Hotkey F1** — adicionar tratamento no `WindowProcHook` do
   `lifecycle.cpp` para toggle do overlay.
4. **ImGui debug window** — portar `DllOverlayUiImGui.cpp` para builds
   de debug (janela demo do ImGui sobreposta ao jogo).
5. **Selector + mapping overlay** — portar `DllControllerManager` para
   o modo de mapeamento de controle dentro do jogo.

## Próximos passos

1. **Investigar e corrigir o rematch no `RetryMenu`** — issue conhecida
   mais relevante. Provável ajuste: auto-input manda CONFIRM em vez de A
   quando `getState() == RetryMenu`. Validar que ambos peers sincronizam
   o rematch.
2. **Implementar os 4 gaps** de robustez (~32 LOC) — podem vir antes ou
   depois do fix de rematch.
3. **Validar alt-tab / window resize** no Windows nativo (não-Wine) —
   o DX9 hook só funciona fora do Wine.
4. **Habilitar spectator mode** — o `DllSpectatorManager.cpp` do CCCaster
   JÁ foi portado (`src/dll/spec/spectator_manager.{hpp,cpp}`) e está wired
   end-to-end, mas está DESABILITADO por 2 guards em `network_thread.cpp`
   (CONNECT handler + peerCapacity=2) que causaram regressões no Wine.
   Próximo passo: reescrever o CONNECT handler para distinguir oponente
   de spectator. Ver `docs/spectator-plan.md` → "Estado atual" e
   `docs/threading-migration.md` → "Layer 5 disablers".
