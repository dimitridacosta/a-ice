# A-Ice vs Hermes — Analyse comparative des approches

> Source : `~/.hermes/hermes-agent/` (Nova/Hermes, agent Python d'Anthropic internal-grade).
> Périmètre : ce que **A-Ice** a déjà implémenté (tool calling natif + inline fallback,
> boucle agentique, thinking streamé) et **comment le durcir** à la lumière d'Hermes.

## TL;DR

Hermes est un agent de production massif (~300 fichiers, ~120 000 lignes). A-Ice est
un overlay KDE minimaliste (~12 fichiers C++). On ne vise évidemment pas la parité,
mais Hermes résout des problèmes qu'on rencontre **déjà** et qu'on a traités à minima.
Les trois domaines où Hermes nous apporte le plus :

1. **Boucle agentique** — séparation stricte tour/model/outil, validation des tool_calls
   avant exécution, gestion des retries (noms invalides, JSON cassé, tronqué).
2. **Sécurité du terminal** — patterns dangereux (hardline + dangerous), session allowlist,
   smart-approve via LLM auxiliaire, hook `pre_tool_call`.
3. **Extraction inline** — nettoyage des blocs `<tool_call>`/`<function>`/`<think>` qui
   fuitent dans le `content`/`reasoning_content` des modèles open (Qwen3 inclus).

Ce qu'on a **déjà** et qui tient la route : le fallback inline (reasoning + content),
le reset de `m_isRequesting` avant les signaux, la limite anti-boucle (8 itérations),
le feedback visuel 🔧/✓ dans la thinking.

---

## 1. Architecture générale comparée

### Hermes — monolithe modulaire Python

```
conversation_loop.py  (6008 l.)   ← la boucle principale (while max_iterations)
├─ tool_executor.py              ← exécution séquentielle OU concurrente
├─ model_tools.py   (1231 l.)    ← handle_function_call (dispatcher)
├─ agent_runtime_helpers.py      ← _strip_think_blocks, inline extraction
├─ message_sanitization.py       ← _repair_tool_call_arguments
├─ tools/approval.py (1812 l.)   ← dangerous command detection + approval
├─ tools/terminal_tool.py        ← le tool terminal (sandbox/docker/modal)
└─ agent/tool_guardrails.py      ← before_call policy
```

La boucle est **une grosse fonction `run_agent`** avec un `while` qui :
- build `api_messages` (avec repair des tool_calls précédents)
- appelle le modèle
- valide les tool_calls (nom existe ? JSON valide ? tronqué ?)
- exécute (séquentiel ou concurrent) avec hooks `pre_tool_call` / `post_tool_call`
- append les `role="tool"` results
- re-boucle tant que le modèle émet des tool_calls ou qu'on a un final content

Points clés qu'on n'a pas :
- **Repair des tool_calls précédents** avant chaque requête (`_sanitize_tool_call_arguments`).
  Le modèle peut produire du JSON cassé dans l'historique → on le répare avant de le
  renvoyer, sinon l'API rejette.
- **Détection de troncation** : si `finish_reason="tool_calls"` mais les arguments ne
  finissent pas par `}` ou `]`, c'est tronqué → on ne tente pas d'exécuter, on retourne
  `partial`.
- **Budget d'itérations** (`iteration_budget`) avec un `_budget_grace_call`.
- **Interrupt propre** : on check `_interrupt_requested` **avant** chaque tool, et on
  injecte des `role="tool"` "cancelled" pour les skipped (sinon l'API rejette :
  chaque `tool_call_id` doit avoir une réponse `role="tool"`).

### A-Ice — boucle dans ChatWidget (signaux Qt)

```
ChatWidget::onSendMessage ─→ LlamaClient::sendMessages
   ← thinkingChunk / contentChunk (stream)
   ← toolCallsReady(calls, assistantContent)
ChatWidget::onToolCallsReady
   ├─ append assistant (avec tool_calls) à m_messages
   ├─ executeToolCalls(calls, 0)
   │     ├─ registry->execute(name, args, cb)
   │     ├─ cb → append role="tool" → executeToolCalls(calls, index+1)
   │     └─ si dernier → m_client->sendMessages(m_messages)  ← re-boucle
   └─ m_toolIterations++ (limite 8)
```

C'est **équivalent fonctionnellement** mais :
- **Pas de validation** des tool_calls avant exécution (nom inconnu → le registry
  répond `unknown tool`, mais on ne le signale pas au modèle proprement comme un
  `role="tool"` d'erreur pour qu'il s'auto-corrige).
- **Pas de repair** de l'historique (si un tool_call précédent a un JSON cassé, on le
  renvoie tel quel → l'API peut rejeter).
- **Pas de détection de troncation**.
- **Pas d'interrupt propre** pendant l'exécution des tools (le bouton stop interrompt
  le stream, mais pas un `QProcess` en cours dans `TerminalTool`).
- **L'anti-boucle compte mais ne reset pas** `m_toolIterations` entre messages user —
  attention : on l'incrémente à chaque `onToolCallsReady`. Vérifier qu'il est reset au
  `onSendMessage`.

---

## 2. Le point crucial : chaque `tool_call_id` doit avoir une réponse `role="tool"`

Hermes est **obsédé** par ça (lignes 558-604, 4358-4378). Règle de l'API OpenAI :

> Toute `assistant` message avec `tool_calls` doit être suivie d'exactement une
> `role="tool"` par `tool_call_id`, dans l'ordre. Sinon → HTTP 400
> "messages must follow tool calls" ou "tool_call_id not found".

A-Ice le fait correctement dans `executeToolCalls` (on append un `role="tool"` par
tool_call, puis on relance). **MAIS** attention aux cas d'erreur :

- **Tool inconnu** : `ToolRegistry::execute` répond `cb(false, "unknown tool")` → on
  append quand même un `role="tool"` → OK. Mais le message d'erreur est cryptique pour
  le modèle, pas formaté comme une erreur récupérable.
- **Tool qui crash avant `cb`** (exception non catchée dans le tool) → `cb` jamais
  appelé → pas de `role="tool"` appendé → boucle cassée, l'API rejettera au prochain
  tour. **Risque réel.** Il faut garantir que `cb` est toujours appelé (wrap try/catch
  dans `ToolRegistry::execute`).

### Recommandation A-Ice

Dans `ToolRegistry::execute`, wrapper l'appel au tool dans un guard qui garantit que
`cb` est invoqué exactement une fois même si le tool throw :

```cpp
void ToolRegistry::execute(const QString &name, const QJsonObject &args,
                           std::function<void(bool,QString)> cb) {
    auto *tool = find(name);
    if (!tool) { cb(false, "unknown tool: " + name); return; }
    // Guard : ensure cb called exactly once even on exception.
    auto guard = std::make_shared<std::once_flag>();
    auto safeCb = [guard, cb](bool ok, QString r) {
        std::call_once(*guard, cb, ok, std::move(r));
    };
    try { tool->execute(args, safeCb); }
    catch (...) { safeCb(false, "tool crashed"); }
}
```

Et **surtout** : si un tool ne répond jamais (processus bloqué, callback oublié), il
faut un timeout de garde au niveau de `executeToolCalls` (un `QTimer` qui force un
`role="tool"` d'erreur). Hermes le fait via `future.result(timeout=300)`.

---

## 3. Validation des tool_calls avant exécution

### Hermes (conversation_loop.py:3675-3830)

Pipeline en 4 étapes avant d'exécuter :
1. **Repair des noms** : `_repair_tool_call(tc.function.name)` — fuzzy match si le
   modèle a mal orthographié un tool.
2. **Validation noms** : si un nom n'est pas dans `valid_tool_names` → on n'exécute pas,
   on append un `role="tool"` d'erreur par tool_call + `continue` (re-boucle). Après 3
   invalides → on stoppe en `partial`.
3. **Validation JSON args** : `json.loads` sur chaque `arguments`. Si invalide :
   - Si tronqué (args ne finit pas par `}`/`]`) → stop `partial` (ne pas retry).
   - Sinon → 3 retries, puis injection de `role="tool"` d'erreur de récupération.
4. **Post-call guardrails** : `_cap_delegate_task_calls` (limite les delegations),
   `_deduplicate_tool_calls` (enlève les doublons exacts).

### A-Ice

On a **rien de tout ça**. `tc.parsedArgs()` retourne un objet vide si invalide, et on
exécute quand même le tool avec des args vides. Le modèle ne sait pas que ses args
étaient cassés.

### Recommandation A-Ice

Ajouter dans `onToolCallsReady` (avant `executeToolCalls`) :

1. Pour chaque `ToolCall` : vérifier `name` est dans le registry. Si non → append un
   `role="tool"` d'erreur : `"Tool '<name>' does not exist. Available: ..."` et passer
   au suivant (ne pas exécuter). C'est exactement ce que fait Hermes.
2. Valider `parsedArgs()` : si `arguments` non vide mais JSON invalide → append
   `role="tool"` : `"Invalid JSON arguments: <err>. Retry with valid JSON."`.
3. Détecter troncation : si `arguments` non vide et ne finit pas par `}` ou `]` →
   ne pas exécuter, append `role="tool"` d'erreur et re-lancer le modèle.

C'est **du code de ~30 lignes** dans `onToolCallsReady` qui évite 80% des bugs de
function-calling avec les modèles open.

---

## 4. La flash du JSON inline pendant le stream

### Le problème A-Ice

Pendant le stream, le modèle écrit le tool_call en texte dans `reasoning_content`
ou `content`. On accumule (`m_streamedReasoning`/`m_streamedContent`) et on émet
`thinkingChunk`/`contentChunk` au fur et à mesure → l'utilisateur voit le bloc
`{"name": "terminal", "arguments": {...}}` apparaître en direct dans la bulle, puis
disparaître à la fin du stream quand `extractInlineToolCalls` nettoie et `setThinking`
remplace.

### Comment Hermes gère (agent_runtime_helpers.py:580+)

Hermes **ne tente pas de masquer pendant le stream**. Le raisonnement est streamé
brut, puis **après** le tour, `_strip_think_blocks` nettoie le `content` stocké dans
l'historique avant de le renvoyer au modèle. Côté UI (CLI), le `reasoning_content` est
affiché tel quel dans un panneau séparé, et les blocs tool_call XML sont juste laissés
visible (l'utilisateur voit l'agent "réfléchir à voix haute").

Hermes a aussi un nettoyage massif (`_strip_tool_error`, regex sur
`<tool_call>...