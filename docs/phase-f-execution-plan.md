# Plano de Execução — Fase F (Netplay Engine)

> **STATUS**: F.1–F.7 implementadas (commits `4040091` → `13989a0`).
> Teste de sanidade pós-implementação em
> [`phase-f-sanity-check.md`](phase-f-sanity-check.md) identificou 7
> pontos críticos faltantes (3 blockers de rollback + 4 de
> robustez/UX). Esses pontos devem ser endereçados antes dos testes
> contra MBAA.exe.

Este documento é o **foco atual** do projeto ReCaster. Ele consolida a análise
linha-a-linha entre o CCCaster original (`targets/DllNetplayManager.{hpp,cpp}`,
`targets/DllRollbackManager.{hpp,cpp}`, `targets/DllMain.cpp`,
`targets/DllSpectatorManager.cpp`, `netplay/InputsContainer.hpp`,
`netplay/NetplayStates.hpp`) e o estado atual do ReCaster, e estabelece o
plano de execução para completar a Fase F do port.

A Fase F é a parte mais crítica de todo o projeto — sem ela, não há partidas
online funcionais. Tudo o que foi feito nas Fases A–E é infraestrutura que
somente se realiza quando a FSM de netplay e o rollback estão plugados ao
frame loop. Cada divergência silenciosa aqui se manifesta como desync,
travamento de menu, ou rollback que não restaura estado corretamente — bugs
extremamente difíceis de diagnosticar contra o jogo real.

O princípio orientador é o do `dll-port-plan.md`: **copiar o que funciona,
adaptando para C++23**. Em outras palavras, divergências estruturais do
CCCaster só são aceitáveis quando explicitamente justificadas (por exemplo,
remoção de trial mode, remoção de UDP debug logger, substituição de cereal
por serialização manual). Divergências acidentais — onde o ReCaster faz algo
diferente do CCCaster sem justificativa documentada — são tratadas como bugs
a corrigir.

---

## 1. Estado atual — inventário

A tabela abaixo compara cada arquivo da Fase F do CCCaster contra o ReCaster.
As LOC do CCCaster são as contadas no clone de referência
(`https://github.com/Rhekar/CCCaster`, clonado em `/home/z/my-project/CCCaster`).

| Item | CCCaster LOC | ReCaster status | Notas |
|---|---|---|---|
| `InputsContainer<T>` | 192 | Portado (118 LOC) | Divergências sérias — ver §2.1 |
| `NetplayState` enum | 35 | Portado (84 LOC) | Divergências sérias — ver §2.2 |
| `RollbackManager` | 349 | Portado (177 LOC) | Divergências sérias — ver §2.3 |
| `rollback_addresses` | Generator.cpp ~360 | Portado (201 LOC) | Divergência séria — ver §2.4 |
| `NetplayManager` (FSM) | 1508 | Inexistente | O coração da Fase F. Sem isso nada funciona. |
| `DllSpectatorManager` | 235 | Inexistente | Cortado da v1 (decisão mantida). |
| `dll_main.cpp` refatorado | 2253 | Stub rudimentar (336 LOC) | Apenas menu-mash + read-input + send/receive BothInputs. Sem FSM, sem rollback plugado, sem SyncHash, sem RNG sync. |

---

## 2. Divergências críticas entre ReCaster e CCCaster

Cada divergência abaixo é um ponto onde o ReCaster se afasta do CCCaster sem
justificativa documentada no `dll-port-plan.md`. São bugs a corrigir antes
de portar a FSM — caso contrário, a FSM herdará contratos quebrados e será
impossível distinguir bug do port de bug do código subjacente.

### 2.1 `InputsContainer` — semântica diferente do CCCaster

O `InputsContainer` é o tipo mais fundamental da Fase F: ele mapeia
`{índice de transição, frame} → input` e é usado pelo `NetplayManager` para
guardar histórico de inputs de cada player, pelo `RollbackManager` para
detectar divergências entre input predito e input real, e pelo
`SpectatorManager` para enviar janelas de inputs a espectadores. Qualquer
divergência de semântica aqui propaga bugs para toda a camada superior.

**ReCaster** (`src/dll/inputs_container.hpp`):
- `set(index, frame, value)` **sobrescreve** silenciosamente — viola
  contrato do CCCaster onde `set` "CANNOT change existing inputs".
- `get` retorna `T{}` (zero) se faltar índice/frame, ao passo que o
  CCCaster retorna o **último input conhecido antes daquele índice**
  (`lastInputBefore`).
- `_lastChangedFrame` é `unordered_map<uint32_t, uint32_t>` (uma por
  índice), enquanto o CCCaster tem **um único `_lastChangedFrame` do tipo
  `IndexedFrame`** global.
- Faltam: o overload `set(index, frame, value, n)` (preenche N frames com o
  mesmo valor), o overload `get(index, frame, T*, n)` (copia N inputs), e o
  parâmetro `checkStartingFromIndex` que o rollback usa para detectar
  divergência.
- `clearLastChangedFrame` no ReCaster apaga o mapa; no CCCaster reseta para
  `MaxIndexedFrame`.

**Impacto**: rollback quebra. `getLastChangedFrame` é o gatilho do
rollback — se estiver errado, ou o rollback nunca dispara ou dispara toda
hora. E `get` retornar 0 ao invés do último input causa inputs fantasma no
replay (player remoto "solta" todos os botões quando um frame ainda não
chegou).

**Recomendação**: reescrever `InputsContainer` para refletir fielmente a
semântica do CCCaster (incluindo `lastInputBefore` e `MaxIndexedFrame`
sentinel). Não é grande — aproximadamente 80 LOC a mais, mas elimina uma
classe inteira de bugs difíceis de rastrear.

### 2.2 `NetplayState` — tabela de transições diverge do CCCaster

Comparando `isValidNextState` do ReCaster (`src/dll/netplay_states.hpp:51-82`)
vs `isValidNext` do CCCaster (`targets/DllNetplayManager.cpp:1140-1163`):

| De → Para | CCCaster | ReCaster | Problema |
|---|---|---|---|
| Loading → Skippable | aceito | omitido | Round-end com rollback usa Loading→Skippable via `checkRoundOver` em alguns caminhos |
| Loading → InGame | aceito | aceito | treino |
| Loading → CharaIntro | aceito | aceito | versus |
| Skippable → Skippable | rejeitado | aceito | Auto-loop estranho, não está no original |
| Skippable → RetryMenu | aceito | aceito | |
| InGame → RetryMenu | aceito | omitido | Crítico: `checkRoundOver` no CCCaster faz exatamente essa transição |
| RetryMenu → ReplayMenu | aceito | omitido | Save-replay flow |

**Impacto**: transições legítimas serão rejeitadas. No CCCaster isso
dispara `delayedStop("Desync!")`. Para o ReCaster (sem `delayedStop` no
momento) o efeito é a FSM travar ou pular estados silenciosamente.

**Recomendação**: alinhar 1:1 com o CCCaster. São 3 linhas a mudar no
arquivo `netplay_states.hpp`.

### 2.3 `RollbackManager` — 4 divergências importantes

Comparando `rollback_manager.cpp` do ReCaster contra `DllRollbackManager.cpp`
do CCCaster:

**(a) `saveState` — estratégia de eviction diverge.**

O CCCaster (linhas 84-100) tem lógica não-trivial: se `_freeStack` está
vazia, descarta o **mais antigo do meio** quando
`front().indexedFrame.parts.frame <= netMan.getRemoteFrame()` (mantém o
front porque ainda pode ser alvo de rollback futuro), caso contrário
descarta o front. Essa lógica existe porque descartar um state que ainda
está dentro da janela de rollback do peer faz o próximo `loadState` falhar.

O ReCaster (linhas 58-69) sempre descarta o front, sem checar
`getRemoteFrame()`. Isso pode descartar um state que ainda seria alvo de
rollback futuro, fazendo o rollback falhar silenciosamente — o que se
manifesta como desync silencioso (o jogo continua, mas com estado
divergente do peer).

**(b) `loadState` — divergência na busca.**

O CCCaster itera `_statesList.rbegin() → rend()` e carrega o primeiro state
com `indexedFrame <= target`. Em RELEASE, se nenhum bater, força carregar o
`_statesList.front()` (fallback seguro). Esse fallback é crítico: sem ele,
um `target` mais antigo que o state mais antigo salvo faria o rollback
falhar e o jogo seguir corrompido.

O ReCaster itera `_statesList.end() → begin()`, mesma lógica `<= target`,
mas **sem o fallback de front**. Se o target for mais antigo que o state
mais antigo salvo, retorna `false` e o caller precisa lidar com a falha —
o que atualmente nenhum caller faz, porque `loadState` nem é chamado ainda.

**(c) `loadState` — faltam 3 efeitos colaterais críticos.**

O CCCaster, após carregar o state, faz mais três coisas que o ReCaster não
faz:

1. **Atualiza `netMan._state`, `netMan._startWorldTime`,
   `netMan._indexedFrame`** com os valores salvos. O ReCaster só retorna
   via out-params, e nenhum caller aplica esses out-params de volta ao
   `NetplayManager`. Sem isso, após um rollback a FSM continua achando que
   está no frame atual, não no frame restaurado.
2. **Fixup do `RepInputContainer`** — apaga N frames do replay struct
   interno do jogo para cada frame de rollback (linhas 165-203). Sem isso,
   replays internos do jogo ficam inconsistentes e o `exportInputs` quebra
   em runtime com estado corrompido.
3. **SFX mute re-init** — linhas 210-222, marca SFX não-tocados como `0x80`
   para serem mutados depois. ReCaster não tem (combo com
   `saveRerunSounds`/`finishedRerunSounds` que também não existem).

**(d) `saveRerunSounds` / `finishedRerunSounds` ausentes.**

O ReCaster removeu esses métodos completamente (combinado com a remoção de
`sfxFilterArray`/`sfxMuteArray` do `asm_hacks`). Isso causa audio glitch
durante reroll — som já tocado volta a tocar, som ainda não tocado é
silenciado. Aceitável pra v1? É decisão de produto, mas precisa estar
documentada como limitação conhecida. A recomendação é portar — o ASM hack
`filterRepeatedSfx` existe no CCCaster e são aproximadamente 80 LOC de
código C++.

### 2.4 `rollback_addresses.hpp` — faltam pointer chasers

O CCCaster (`tools/Generator.cpp:291-297`) define `firstEffect` com
**pointer chasing aninhado**:

```cpp
static const MemDump firstEffect (CC_EFFECTS_ARRAY_ADDR, CC_EFFECT_ELEMENT_SIZE, {
    MemDumpPtr(0x320, 0x38, 4, {
        MemDumpPtr(0, 0, 4, {
            MemDumpPtr(0, 0, 4)
        })
    })
});
```

E aplica isso a cada um dos 1000 elementos do effects array. Isso segue
ponteiros gravados em offsets `0x320+0x38` para salvar estados de efeitos
que estão em **heap alocado pelo jogo** — sem isso, o estado visual dos
efeitos (hitsparks, partículas, super flashes) é perdido no rollback.

O ReCaster (`src/dll/rollback_addresses.hpp:192-195`):

```cpp
for (int i = 0; i < CC_EFFECTS_ARRAY_COUNT; ++i) {
    allAddrs.append({(void*)(uintptr_t)(CC_EFFECTS_ARRAY_ADDR + i * CC_EFFECT_ELEMENT_SIZE),
                     CC_EFFECT_ELEMENT_SIZE});
}
```

Só faz dump direto dos 0x33C bytes. **Perde todo o estado apontado pelos
ponteiros em 0x320+0x38.**

**Impacto**: rollback vai restaurar um estado parcial dos efeitos visuais.
Provavelmente causa desync visual ou, pior, desync de estado se efeitos
influenciarem gameplay (hitsparks, hitstop, etc.). É difícil afirmar sem
testar, mas é clara divergência do princípio "copiar o que funciona".

O `MemDumpPtr` está implementado no ReCaster (`src/dll/mem_dump.hpp:36-57`)
e funcional. Faltou **usá-lo** no `rollback_addresses.hpp`. Há também os
player-struct `MemDumpPtr`s comentados no `Generator.cpp` (linhas 67-138) —
vários campos com ponteiros aninhados para sub-structs. O ReCaster também
não tem nenhum deles. Esses são os mais arriscados porque provavelmente
afetam estado de animação/hitbox dos personagens.

### 2.5 `dll_main.cpp` — o que falta portar

Comparando `frameStep()` do CCCaster (linhas 957-997) com o ReCaster
(`src/dll/dll_main.cpp:189-272`):

| Componente do CCCaster | ReCaster |
|---|---|
| `netMan.updateFrame()` (atualiza indexed frame do world timer) | Ausente — usa `g_frameCount` local |
| `procMan.clearInputs()` | Ausente |
| `ChangeMonitor::get().check()` (detecta gameMode/roundStart/gameState) | Ausente — sem isso a FSM nunca transiciona |
| `checkRoundOver()` (detecta fim de round → Skippable) | Ausente |
| `frameStepRerun()` (modo fast-forward pós-rollback) | Ausente |
| `frameStepNormal()` com switch em `NetplayState` | Parcial — só tem PreInitial/Initial (via gameMode flag), resto é "leu input → escreveu" |
| `frameStepSpectators()` | Ausente (cortado da v1, ok) |
| `procMan.writeGameInput(localPlayer, netMan.getInput(localPlayer))` | Parcial — usa `read_local_input` direto, sem passar pela FSM |
| `procMan.writeGameInput(remotePlayer, netMan.getInput(remotePlayer))` | Parcial — usa `apply_remote_input` direto |
| `shouldSyncRngState` + `procMan.setRngState` | Ausente |
| `rollMan.saveState(netMan)` a cada frame InGame | Ausente |
| `rollMan.loadState(getLastChangedFrame, netMan)` quando diverge | Ausente |
| SyncHash exchange + desync detect | Ausente (só release builds no CCCaster) |
| `dataSocket->send(netMan.getInputs(localPlayer))` — PlayerInputs, não BothInputs | ReCaster usa BothInputs pra tudo — protocolo diverge do CCCaster |
| `setRemoteIndex` via `TransitionIndex` msg | Ausente |
| `localRetryMenuIndexSent` flow (MenuIndex msg) | Ausente |
| `shouldChangeDelayRollback` flow (ChangeConfig msg) | Ausente |
| `delayedStop` + `lazyDisconnect` | Ausente |

---

## 3. Plano de execução — sequência segura, validável etapa por etapa

O plano é dividido em 7 sub-etapas. Cada uma produz um estado compilável e
testável. **Nada de portar tudo de uma vez** — cada etapa precisa ser
validada contra MBAA.exe via Wine antes de avançar. Isso segue o espírito
do `dll-port-plan.md` (que já estabeleceu essa cadência para as Fases A–E)
e respeita a regra prática de que bugs em rollback são virtualmente
impossíveis de diagnosticar sem isolamento etapa-por-etapa.

### Etapa F.1 — Corrigir `InputsContainer` e `NetplayState` (pré-requisito)

- Reescrever `InputsContainer` com semântica idêntica ao CCCaster
  (`lastInputBefore`, `MaxIndexedFrame`, overloads `set`/`get` com N,
  parâmetro `checkStartingFromIndex`).
- Alinhar `isValidNextState` 1:1 com `isValidNext` do CCCaster.
- **Validação**: compila + smoke test offline (training mode ainda
  funciona). Sem isso, não vale a pena começar F.2 — qualquer bug aqui
  contamina toda a FSM.

### Etapa F.2 — Portar `NetplayManager` (sem rollback ainda)

- Portar `DllNetplayManager.hpp` 1:1 (struct + assinaturas).
- Portar `.cpp` removendo: UDP debug logger, trial hooks
  (`TrialManager::playDemo`, `getDemoInput`), replay code (`exportInputs`,
  `exportResults`, `autoReplaySave`), `splitDelay` (manter simples),
  `getSkippableInput` replay branch.
- Manter: `setState` (com toda a lógica de
  `_startIndex`/`_spectateStartIndex`/`preserveStartIndex`), `updateFrame`,
  `getInput` switch, `setInput`/`assignInput`/`getInputs`/`setInputs`/
  `getBothInputs`/`setBothInputs`, `isRemoteInputReady`, `isRngStateReady`,
  `getRngState`/`setRngState`, `getRetryMenuIndex`/`setRetryMenuIndex`/
  `getLocalRetryMenuIndex`/`setRemoteRetryMenuIndex`,
  `getLastChangedFrame`/`clearLastChangedFrame`, `getRemoteIndex`/
  `setRemoteIndex`.
- Substituir `MsgPtr` (cereal) por `std::shared_ptr<Message>` ou refs
  diretas — adaptar para o `messages.hpp` existente.
- Substituir `LOG()` UDP por `caster::common::logger`.
- **Validação**: compila. Não pluga ainda em `dll_main.cpp`.

### Etapa F.3 — Plugar `NetplayManager` no `dll_main.cpp` (sem rollback)

- Instanciar `NetplayManager netMan` global.
- Trocar o "leu input → escreveu" por `netMan.setInput(localPlayer, input)`
  + `procMan.writeGameInput(p, netMan.getInput(p))`.
- Implementar o `ChangeMonitor` simplificado: a cada `callback()`, ler
  `CC_GAME_MODE_ADDR`, `CC_GAME_STATE_ADDR`, `roundStartCounter` e chamar
  os handlers `gameModeChanged`/`gameStateChanged`/
  `netplayStateChanged(RoundStart)`.
- Implementar `frameStepNormal` (sem spectate, sem overlay, sem hotkeys
  Ctrl+0..9, sem F5/F6/F7, sem replay, sem trial).
- Implementar `netplayStateChanged` lifecycle (allocateStates em InGame,
  deallocate em leave InGame — por enquanto vazio, só placeholder).
- Substituir `netplay_connector::send_local_input` para usar
  `netMan.getInputs(localPlayer)` (PlayerInputs, não BothInputs — voltar
  ao protocolo CCCaster).
- Substituir `apply_remote_input` para usar
  `netMan.setInputs(remotePlayer, ...)` + `netMan.getInput(remotePlayer)`.
- Implementar troca de `TransitionIndex` (setRemoteIndex).
- **Validação**: training mode funciona + duas instâncias host+join passam
  pelo menu juntas (sem jogar ainda, sem rollback).

### Etapa F.4 — SyncHash + RngState sync

- Implementar troca periódica de `SyncHash` (a cada 5*60 frames ou 150
  frames, igual CCCaster).
- Implementar comparação local/remote SyncHash + `delayedStop("Desync!")`
  (criar `delayedStop` simplificado).
- Implementar `shouldSyncRngState` + `procMan.setRngState` (host envia
  RngState no frame 0 de CharaSelect/InGame).
- Implementar `isRngStateReady` gate antes de `frameStepNormal` (client
  espera RngState do host).
- **Validação**: desync detectado em teste manual (forçar divergência).

### Etapa F.5 — Plugar RollbackManager

- Corrigir `rollback_addresses.hpp` primeiro (adicionar `MemDumpPtr` no
  `firstEffect` e nos player-struct pointers comentados do Generator.cpp).
- Corrigir `RollbackManager::saveState` (eviction strategy do CCCaster) e
  `loadState` (fallback front + atualização de
  `netMan._state`/`_startWorldTime`/`_indexedFrame` + `RepInputContainer`
  fixup).
- Em `frameStepNormal` InGame: chamar `rollMan.saveState(netMan)` a cada
  frame.
- Implementar `getLastChangedFrame` check + `rollMan.loadState(
  getLastChangedFrame, netMan)` + `frameStepRerun` (com `fastFwdStopFrame`
  + `CC_SKIP_FRAMES_ADDR=1`).
- Implementar `rollbackTimer`/`minRollbackSpacing` (espaçamento entre
  rollbacks).
- **Decidir**: SFX mute durante reroll — portar `saveRerunSounds`/
  `finishedRerunSounds` + `sfxFilterArray`/`sfxMuteArray` (precisa portar
  `filterRepeatedSfx` ASM hack), ou aceitar glitch de áudio na v1.
  Recomendo portar — é aproximadamente 80 LOC e o ASM hack existe no
  CCCaster.
- Implementar `checkRoundOver` (transição InGame → Skippable via
  `CC_P1_NO_INPUT_FLAG_ADDR` etc.).
- **Validação**: testes de stress — forçar atraso de pacote, confirmar que
  rollback dispara e restaura estado corretamente.

### Etapa F.6 — RetryMenu flow

- Implementar `MenuIndex` exchange (`localRetryMenuIndexSent` +
  `setRemoteRetryMenuIndex`).
- Implementar `getRetryMenuInput` com a lógica de "ambos os lados
  selecionaram → navigate" (`_targetMenuState`/`_targetMenuIndex`).
- Implementar `getMenuNavInput` (state machine 0→1→2→...→39).
- Implementar `lazyDisconnect` (disconnect limpo após retry menu selado em
  caso de socket cair).
- **Validação**: partida completa melhor-de-3, com retry menu funcionando.

### Etapa F.7 — Manual delay (`--delay=N`) + changeConfig mid-match (opcional v1)

- Adicionar `NetplaySession::set_manual_delay` no launcher.
- Implementar `shouldChangeDelayRollback` flow no `dll_main.cpp`
  (ChangeConfig msg → `netMan.setDelay`/`setRollback`).
- Pular se v1 não precisar de mudança mid-match.

---

## 4. Riscos topológicos (não-resolvíveis por código)

Estes são riscos que não podem ser completamente eliminados por código —
precisam ser validados empiricamente contra o MBAA.exe real.

1. **`binary_res_rollback_bin` já foi substituído** por
   `rollback_addresses.hpp`, mas a substituição é **incompleta** (sem
   `MemDumpPtr`). Sem os pointer chasers, o rollback pode não preservar
   estado suficiente para evitar desync sutil — só validável testando
   contra o jogo.
2. **`RepInputContainer` fixup** depende do struct
   `RepRound`/`RepInputContainer`/`RepInputState` — é um hack frágil que
   lê `CC_REPROUND_TBL_ENDPTR_ADDR`. Precisa validar que o offset
   `0x77BF9C` ainda aponta para a tabela correta em MBAA 1.07 Rev.1.4.0.
3. **`checkRoundOver`** lê `CC_P1_PUPPET_STATE_ADDR` +
   `CC_P3_PUPPET_STATE_ADDR` (puppets = tag battles). A lógica "se P1 é
   puppet, checa P3" pode não se aplicar a versão sem puppets. Validar.
4. **Wine compatibility**: o `frameStepRerun` usa `CC_SKIP_FRAMES_ADDR=1`
   para fast-forward. Em Wine o limiter nativo do jogo roda, então o
   fast-forward pode ser muito rápido (sem sync de 60fps). Precisa testar.
5. **Protocolo BothInputs vs PlayerInputs**: ReCaster usa BothInputs pra
   tudo. CCCaster usa PlayerInputs pra netplay host/client e BothInputs só
   pra spectate. Isso significa que ReCaster não é compatível com o
   protocolo CCCaster (mas isso já era esperado pela decisão ENet).
   Internamente, porém, o `setInputs(remotePlayer, PlayerInputs)` do
   CCCaster é mais simples que `setBothInputs` — ReCaster precisa decidir
   se mantém BothInputs em tudo ou volta pra PlayerInputs no path
   host/client. **Recomendo voltar a PlayerInputs** pra aderir ao
   princípio "copiar o que funciona".

---

## 5. Estimativa revisada de esforço

A estimativa abaixo substitui a entrada "F — Netplay engine" do
`dll-port-plan.md` (que originalmente estimava 5 dias para 4540 LOC). A
revisão reflete a realidade dos arquivos já portados e das correções
necessárias nas divergências documentadas em §2.

| Etapa | LOC a portar/corrigir | Complexidade | Esforço |
|---|---|---|---|
| F.1 InputsContainer + NetplayState | ~120 | Média | 0.5 dia |
| F.2 NetplayManager | ~1100 (sem trial/replay/debug) | Alta | 3 dias |
| F.3 Plug NetplayManager + ChangeMonitor | ~400 | Alta | 2 dias |
| F.4 SyncHash + RngState | ~150 | Média | 1 dia |
| F.5 Rollback + checkRoundOver + frameStepRerun | ~400 | Extrema | 3 dias |
| F.6 RetryMenu flow | ~200 | Alta | 1.5 dias |
| F.7 Manual delay (opcional) | ~50 | Baixa | 0.5 dia |
| **Total** | **~2400** | — | **~11.5 dias** |

Mais 1-2 dias de buffer para testes full-duplex com 2 instâncias em Wine.

---

## 6. Recomendação imediata

Antes de começar a portar F.2, validar a F.1 em PR separado —
`InputsContainer` errado propaga bug pra toda a FSM e rollback. É mais
barato corrigir agora do que debugar depois, porque bugs de rollback
manifestam-se como desync silencioso que só aparece após minutos de jogo,
tornando bissecção extremamente difícil.

Cada etapa subsequente deve seguir a mesma cadência: PR separado, build
validado contra MBAA.exe via Wine, e só então avançar para a próxima.
Pular etapas ou acumular mudanças não-validadas é a forma mais comum de
uma Fase F falhar — historicamente, projetos de rollback netplay que
tentam portar tudo de uma vez acabam com bugs residuais que levam meses
para serem isolados.

---

## Referências

- `docs/dll-port-plan.md` — plano original das Fases A–H. Esta Fase F
  substitui a entrada "F — Netplay engine" daquele documento.
- `docs/non-implemented-stubs.md` — inventário de stubs. As entradas #6
  (Rollback) e #7 (FSM de netplay) serão resolvidas pela execução deste
  plano.
- `docs/cccaster-dll-targets.md` — inventário dos arquivos `Dll*` do
  CCCaster com classificação (A/B/C/D).
- CCCaster upstream: `https://github.com/Rhekar/CCCaster` (clonado em
  `/home/z/my-project/CCCaster` para referência linha-a-linha durante o
  port).
