# Melhorias Futuras — UX e Features

Documento vivo com ideias de melhorias para o ReCaster, priorizadas por
valor/esforço. Atualizado conforme novas ideias surgem e itens são
concluídos.

---

## UX — Overlays

### 1. Info overlay com dados reais (prioridade alta)
O overlay de info (hotkey `3`) atualmente mostra apenas placeholder
"ReCaster | DX9 Overlay v0.1". Deveria mostrar dados úteis em runtime:
- Ping atual (ms)
- FPS real
- Estado do netplay (CharaSelect / InGame / Rollback / RetryMenu)
- Rollback count (quantos rollbacks nesta partida)
- Delay frames atual

**Dados já disponíveis:** `NetplayManager` tem estado, `connector` tem
ping, `frame_rate` tem FPS. Falta apenas wiring.
**Esforço:** ~80 LOC.

### 2. Ping/delay indicator no playername overlay
Junto ao playername overlay (hotkey `5`), mostrar ping e delay:
```
marcelo  16ms 2f    8ms 1f  Vitor
```

**Esforço:** ~50 LOC (dados já disponíveis).

### 3. Input display overlay (hotkey candidata: `1`)
Mostra os inputs do P1 em tempo real usando numpad notation + botões:
```
P1: 6A  5B  2C  236A  j9AB
```

Útil pra training e streaming. Pode usar o overlay existente.

**Esforço:** ~150 LOC. Precisa ler `CC_P1_DIRECTION_ADDR` e
`CC_P1_BUTTONS_ADDR` a cada frame.

### 4. Rollback counter no overlay
Mostra "ROLLBACK: 42" no canto durante netplay. Dado já disponível
no `RollbackManager`.

**Esforço:** ~30 LOC.

### 5. Help overlay (hotkey candidata: `H` ou `2`)
Overlay que lista todas as hotkeys disponíveis:
```
ReCaster Hotkeys
─────────────────
1 — Input display (future)
2 — (future)
3 — Toggle info overlay
4 — Controller mapper
5 — Toggle player names
H — This help
Alt+F4 — Exit
```

**Esforço:** ~60 LOC.

---

## UX — Keymapper

### 6. Feedback visual durante captura
Quando o usuário aperta Enter pra capturar uma tecla, o overlay deveria
mostrar "capturing..." ou piscar o seletor, em vez de só logar.

**Esforço:** ~20 LOC (mudar texto da linha selecionada).

### 7. Deletar binding individual
Atualmente só é possível re-bindar por cima. Adicionar uma tecla (ex:
Backspace) pra limpar o binding da linha selecionada.

**Esforço:** ~15 LOC.

---

## UX — Launcher / GUI

### 8. Keybinds na launcher
Atalhos de teclado na launcher:
- Tab — navegar entre campos/seções
- Enter — confirmar
- Esc — cancelar/voltar

**Esforço:** ~100 LOC (ImGui suporta via `io.KeyMap`).

### 9. DXVK_HUD toggle nas configs
Adicionar uma opção na config page pra habilitar/desabilitar o DXVK HUD
(Wine only). Atualmente o código está comentado em `main.cpp`.

**Esforço:** ~30 LOC (config field + uncomment + apply).

---

## Hotkeys — reservar funções para 1 e 2

### 10. Hotkey `1` → Input display overlay
Ver item #3 acima.

### 11. Hotkey `2` → Help overlay
Ver item #5 acima. Alternativa: `2` → Rollback counter toggle.

---

## Features — Médio prazo

### 12. Ctrl+0..9 delay hotkeys
Permite ajustar input delay durante o chara-select sem voltar pra
launcher. CCCaster tinha isso.

**Esforço:** ~80 LOC.

### 13. Connection type indicator
Mostra "Wired"/"Wireless" do oponente. Dado já vem no handshake
(`remote_connection_type` no `NetplayConfig`).

**Esforço:** ~50 LOC.

### 14. Replay save/load
Portar `ReplayManager` do CCCaster. Salva inputs de ambos players +
RNG seed, permite replayar a partida exata. Já há `auto_replay_save`
no config mas não implementado.

**Esforço:** ~400 LOC.

### 15. Auto-replay save
Salva replay automaticamente ao fim da partida. Depende de #14.

**Esforço:** ~100 LOC (após #14).

---

## Features — Longo prazo

### 16. DLL-side threading (Layers 4-6)
Plano completo em `docs/threading-migration.md` Part 2.

**Esforço:** ~650 LOC, 3-4 sessões.

### 17. Spectator mode
Plano completo em `docs/spectator-plan.md`. Depende de #16.

**Esforço:** ~670 LOC (incluindo threading), 4-5 sessões.

### 18. Discord Rich Presence
Mostra "Playing MBAACC vs X" no Discord.

**Esforço:** ~200 LOC + discord SDK.

### 19. 2v2 support
4 players simultâneos. Mudança significativa na arquitetura.

**Esforço:** ~1000+ LOC.

### 20. Palette customization
Categoria D, pulada no port. Paletas customizadas via PNG.

**Esforço:** ~500 LOC.

### 21. Trial mode
Categoria D, pulada. Combo trials com demo inputs.

**Esforço:** ~2000 LOC.

### 22. Match history/stats
Trackea W/L, personagens usados, etc.

**Esforço:** ~300 LOC + storage.

---

## Roadmap sugerido (prioridade por valor/esforço)

| # | Feature | LOC | Prioridade |
|---|---|---|---|
| 1 | Info overlay com dados reais | ~80 | Alta — imediato |
| 2 | Ping/delay no playername overlay | ~50 | Alta — imediato |
| 3 | Rollback counter no overlay | ~30 | Alta — imediato |
| 4 | Input display overlay (hotkey 1) | ~150 | Média — 1 sessão |
| 5 | Help overlay (hotkey 2) | ~60 | Média — meia sessão |
| 6 | Feedback visual no keymapper | ~20 | Baixa |
| 7 | Deletar binding individual | ~15 | Baixa |
| 8 | DXVK_HUD toggle nas configs | ~30 | Baixa |
| 9 | Keybinds na launcher | ~100 | Baixa |
| 10 | Ctrl+0..9 delay hotkeys | ~80 | Média |
| 11 | Connection type indicator | ~50 | Média |
| 12 | Threading Layer 4 | ~300 | Alta (pré-requisito spectator) |
| 13 | Spectator mode | ~670 | Média (depende de #12) |
| 14 | Replay save/load | ~400 | Média |
| 15 | Auto-replay save | ~100 | Baixa (depende de #14) |
| 16 | Discord Rich Presence | ~200 | Baixa |
