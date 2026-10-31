# Boucle agentique — Architecture et durcissement

> Source : `~/.hermes/hermes-agent/agent/conversation_loop.py` (6008 lignes),
> `agent/tool_executor.py`, `model_tools.py`.
> Objectif : structurer la boucle de tool-calling d'A-Ice pour qu'elle soit
> robuste multi-tours (think → tool → message → think → tool …).

## La boucle Hermes (anatomie)

Le cœur est un `while` dans `run_agent` :

```python
while (api_call_count < max_iterations
       and iteration_budget.remaining > 0) or grace_call:
    # 1. Build api_messages (repair tool_calls précédents)
    repaired = _sanitize_tool_call_arguments(api_messages)
    # 2. Inject steer (message user injecté pendant la génération)
    # 3. API call (avec retry sur erreurs transitoires)
    assistant_message = call_model(api_messages)
    # 4. Si pas de tool_calls → final response, break
    # 5. Validation tool_calls :
    #    - repair noms (fuzzy)
    #    - noms inconnus → role="tool" erreur + continue (3 max)
    #    - JSON args invalide :
    #       * tronqué (args ne finit pas par }/]) → stop partial
    #       * sinon → 3 retries, puis role="tool" recovery
    # 6. Post-call guardrails :
    #    - _cap_delegate_task_calls (limite delegations)
    #    - _deduplicate_tool_calls (doublons)
    # 7. Append assistant_msg à messages
    # 8. _execute_tool_calls (sequential OU concurrent) :
    #    - check interrupt avant chaque tool
    #    - pre_tool_call hook (plugins / approval)
    #    - execute → role="tool" result
    #    - post_tool_call hook
    # 9. Re-boucle (api_call_count++)
```

### Points structurants qu'on n'a pas

#### a. Repair de l'historique avant chaque requête

`_sanitize_tool_call_arguments` (`conversation_loop.py:644`) :
```python
repaired_tool_calls = agent._sanitize_tool_call_arguments(api_msg, model=agent.model)
```
Le modèle peut produire des `arguments` JSON invalides dans un tool_call précédent.
Quand on renvoie cet historique au modèle, l'API **rejette** (HTTP 400 "invalid
tool_call arguments"). Hermes répare : re-parse, re-serialize, ou injecte `"{}"`.

**A-Ice** : `ChatMessage::toJson` sérialise `arguments` tel quel (string brut).
Si le modèle a produit `"arguments": "ls -la"` (sans JSON), on renvoie ça →
l'API peut rejeter au tour suivant. **Risque réel sur conversations longues.**

#### b. Détection de troncation

`conversation_loop.py:1645+` : si `finish_reason="tool_calls"` mais un `arguments`
ne finit pas par `}` ou `]` → c'est tronqué (le modèle a manqué de tokens). On ne
**tente pas** d'exécuter (args cassés), on stoppe en `partial` plutôt que de boucler.

**A-Ice** : on ne regarde pas `finish_reason`. On essaie `parsedArgs()` qui
retourne `{}` si invalide → on exécute avec args vides → le tool fait n'importe quoi.

#### c. Budget d'itérations dual

`max_iterations` (dur, ex: 50) + `iteration_budget` (souple, ex: token-based).
+ `_budget_grace_call` : autorise un dernier appel même si budget épuisé, pour
que le modèle puisse produire une réponse finale propre au lieu de couper net.

**A-Ice** : `m_toolIterations > 8` → `onRequestError`. Brutal, pas de grace call,
pas de reset vérifié entre messages user.

#### d. Interrupt propre

`tool_executor.py:784` :
```python
if agent._interrupt_requested:
    # Skip tous les tools restants
    for skipped_tc in remaining_calls:
        messages.append({
            "role": "tool", "content": "cancelled — skipped due to user interrupt",
            "tool_call_id": skipped_tc.id,
        })
    break
```
On check l'interrupt **avant** chaque tool. Les skipped tools reçoivent quand même
un `role="tool"` (sinon l'API rejette au prochain tour).

**A-Ice** : `onStopClicked` fait `m_client->cancel()` (interrompt le stream réseau)
mais **ne touche pas** à un `QProcess` en cours dans `TerminalTool`. Et on n'injecte
pas de `role="tool"` cancelled pour le tool_call en cours → si on arrête pendant
l'exécution d'un tool, l'historique est dans un état incohérent.

#### e. Validation nom de tool

`conversation_loop.py:3680-3720` :
- Si `tc.function.name` n'est pas dans `valid_tool_names` → tentative de repair
  (`_repair_tool_call`, fuzzy match).
- Si toujours invalide → append `role="tool"` erreur par tool_call +
  `"Tool '<name>' does not exist. Available: ..."` → `continue` (re-boucle, le
  modèle s'auto-corrige). Max 3 retries.

**A-Ice** : `ToolRegistry::execute` répond `cb(false, "unknown tool")` → on append
un `role="tool"` avec ce message. Mais : pas de fuzzy repair, pas de compteur de
retries, pas de message listant les tools disponibles (le modèle ne sait pas quoi
d'autre appeler).

#### f. Exécution séquentielle vs concurrente

`execute_tool_calls_concurrent` (plusieurs tools en parallèle) vs
`execute_tool_calls_sequential` (un par un, pour tools interactifs).

**A-Ice** : séquentiel uniquement. C'est plus simple et suffisant pour 3 tools.
À garder.

---

## Structure recommandée pour A-Ice

### 1. Reset `m_toolIterations` au `onSendMessage`

Vérifier : actuellement on l'incrémente dans `onToolCallsReady` mais on ne le reset
**jamais explicitement** au début d'un nouveau message user. Si une conversation
précédente a consommé 6 itérations, la suivante n'a plus que 2 de budget.

```cpp
void ChatWidget::onSendMessage(const QString &text) {
    m_toolIterations = 0;  // ← AJOUTER
    // …
}
```

### 2. Validation dans `onToolCallsReady` (avant exécution)

```cpp
void ChatWidget::onToolCallsReady(const QList<ToolCall> &calls, const QString &assistantContent) {
    if (++m_toolIterations > 8) {
        // Inject role="tool" d'erreur pour chaque call (sinon API rejette)
        ChatMessage asstErr;
        asstErr.role = "assistant";
        asstErr.toolCalls = calls;
        m_messages.append(asstErr);
        for (const auto &tc : calls) {
            ChatMessage toolMsg;
            toolMsg.role = "tool";
            toolMsg.toolCallId = tc.id;
            toolMsg.toolName = tc.name;
            toolMsg.content = "Tool iteration limit reached (8). Stop calling tools.";
            m_messages.append(toolMsg);
        }
        onRequestError("Too many tool iterations (limit 8).");
        return;
    }

    // Append assistant (avec tool_calls) à l'historique
    ChatMessage asstMsg;
    asstMsg.role = "assistant";
    asstMsg.content = assistantContent;
    asstMsg.toolCalls = calls;
    m_messages.append(asstMsg);

    // Update bulle
    if (m_currentBubble) {
        m_currentContent = assistantContent;
        m_currentBubble->setContent(assistantContent);
        if (m_currentBubble->thinkingExpanded())
            m_currentBubble->collapseThinking();
    }

    // Validation + exécution tool par tool
    executeToolCallsValidated(calls, 0);
}
```

### 3. `executeToolCallsValidated` — validation puis exécution

Nouvelle fonction qui remplace l'appel direct à `executeToolCalls` :

```cpp
void ChatWidget::executeToolCallsValidated(const QList<ToolCall> &calls, int index) {
    if (index >= calls.size()) {
        m_client->sendMessages(m_messages);
        return;
    }
    const ToolCall &tc = calls.at(index);

    // (a) Validation du nom
    if (!m_registry->find(tc.name)) {
        QString avail = m_registry->availableToolsList();  // à ajouter
        ChatMessage toolMsg;
        toolMsg.role = "tool";
        toolMsg.toolCallId = tc.id;
        toolMsg.toolName = tc.name;
        toolMsg.content = QStringLiteral("Tool '%1' does not exist. Available: %2")
                              .arg(tc.name, avail);
        m_messages.append(toolMsg);
        if (m_currentBubble)
            m_currentBubble->appendThinking(
                QStringLiteral("⚠️ unknown tool: ") + tc.name + "\n");
        executeToolCallsValidated(calls, index + 1);
        return;
    }

    // (b) Validation du JSON args
    if (!tc.arguments.isEmpty() && tc.parsedArgs().isEmpty()) {
        // Args non vides mais JSON invalide
        // Détecter troncation : ne finit pas par } ou ]
        bool truncated = !tc.arguments.trimmed().endsWith('}')
                      && !tc.arguments.trimmed().endsWith(']');
        if (truncated) {
            // Ne pas retry, injecter erreur + stopper
            ChatMessage toolMsg;
            toolMsg.role = "tool";
            toolMsg.toolCallId = tc.id;
            toolMsg.toolName = tc.name;
            toolMsg.content = "Tool call arguments truncated. Stop calling tools.";
            m_messages.append(toolMsg);
            onRequestError("Truncated tool call arguments.");
            return;
        }
        ChatMessage toolMsg;
        toolMsg.role = "tool";
        toolMsg.toolCallId = tc.id;
        toolMsg.toolName = tc.name;
        toolMsg.content = QStringLiteral("Invalid JSON arguments: %1. "
                                        "Retry with valid JSON.")
                              .arg(tc.arguments.left(200));
        m_messages.append(toolMsg);
        executeToolCallsValidated(calls, index + 1);
        return;
    }

    // (c) Approval policy (voir RESEARCH-terminal-safety.md)
    // TODO: if tc.name == "terminal" && m_policy.isDangerous(args) → demande UI

    // (d) Exécuter
    m_registry->execute(tc.name, tc.parsedArgs(),
        [this, calls, index, tc](bool ok, QString result) {
            // Feedback bulle
            if (m_currentBubble) {
                QString preview = result.trimmed();
                if (preview.size() > 400) preview = preview.left(400) + "…";
                m_currentBubble->appendThinking(
                    (ok ? "✓ " : "⚠ ") + preview + "\n\n");
            }
            // Append role="tool"
            ChatMessage toolMsg;
            toolMsg.role = "tool";
            toolMsg.toolCallId = tc.id;
            toolMsg.toolName = tc.name;
            toolMsg.content = result;
            m_messages.append(toolMsg);
            scrollToBottom();
            scheduleBlurUpdate();
            executeToolCallsValidated(calls, index + 1);
        });
}
```

### 4. Garantie `cb` invoqué (dans `ToolRegistry::execute`)

```cpp
void ToolRegistry::execute(const QString &name, const QJsonObject &args,
                            std::function<void(bool, QString)> cb) {
    auto *tool = find(name);
    if (!tool) { cb(false, "unknown tool: " + name); return; }
    // Guard : ensure cb called exactly once even on exception / forget
    auto called = std::make_shared<bool>(false);
    auto guard = [cb, called](bool ok, QString r) {
        if (*called) return;
        *called = true;
        cb(ok, std::move(r));
    };
    QTimer::singleShot(130000, this, [guard, called]() {  // 130s safety net
        if (*called) return;
        *called = true;
        guard(false, "tool timed out (no callback after 130s)");
    });
    try { tool->execute(args, guard); }
    catch (...) { guard(false, "tool crashed"); }
}
```

Le `QTimer` est un filet de sécurité : si un tool oublie d'appeler `cb` (QProcess
bloqué, bug), on force une réponse après 130s pour ne pas rester coincé.

### 5. Bulles par tour (pas une seule bulle pour toute la séquence)

Voir `RESEARCH-hermes-comparative.md` §6. Concrètement :

- `onSendMessage` → bulle user + `startAssistantBubble()` (bulle tour 1).
- Pendant le stream → thinking + content dans bulle courante.
- `onToolCallsReady` → finalize bulle courante (collapse thinking, setContent),
  puis **ne pas réutiliser**. Les tool results s'affichent soit dans une mini-bulle
  "tool" soit dans la thinking de la bulle courante (état intermédiaire).
- Après `executeToolCalls` → `m_client->sendMessages` → dans `onContentChunk` /
  `onThinkingChunk`, **créer une nouvelle bulle** si `m_currentBubble == nullptr`
  (ou si on a explicitement reset après tool execution).

C'est un changement de paradigme dans ChatWidget : `m_currentBubble` devient
"bulle du tour en cours" et on en crée une nouvelle à chaque `sendMessages`.
L'historique `m_messages` (API) est découplé des bulles (UI).

### 6. Interrupt propre

```cpp
void ChatWidget::onStopClicked() {
    m_interruptRequested = true;          // flag
    m_client->cancel();                   // stop stream réseau
    m_registry->cancelAll();               // stop QProcess en cours (à ajouter)
    // Si on était en train d'exécuter un tool, injecter role="tool" cancelled
    // pour le tool_call_id en cours (sinon API rejette au prochain sendMessages).
    // TODO : tracker le tool_call_id en cours d'exécution.
    setGenerating(false);
    m_currentBubble = nullptr;
}
```

`ToolRegistry::cancelAll()` parcourt les tools et appelle un `cancel()` virtuel
sur ceux qui le supportent (TerminalTool → terminate ses QProcess).

---

## Résumé des modifications (par fichier)

| Fichier | Changement |
|---|---|
| `ChatWidget.h` | `m_toolIterations` reset vérifié, `m_interruptRequested`, `executeToolCallsValidated`, `m_toolCallInProgress` (pour interrupt) |
| `ChatWidget.cpp` `onSendMessage` | Reset `m_toolIterations = 0` |
| `ChatWidget.cpp` `onToolCallsReady` | Inject role="tool" sur limite atteinte, validation nom + JSON |
| `ChatWidget.cpp` `executeToolCallsValidated` | Nouvelle fonction (remplace `executeToolCalls`) avec validation + approval hook |
| `ChatWidget.cpp` `onStopClicked` | `m_interruptRequested = true`, `m_registry->cancelAll()`, inject role="tool" cancelled |
| `ToolRegistry.{h,cpp}` | `execute` avec guard cb + timeout, `cancelAll()`, `availableToolsList()`, `find()` public |
| `Tool.h` | `virtual void cancel() {}` (default no-op) |
| `TerminalTool.{h,cpp}` | `cancel()` override → terminate QProcess, garder une liste des QProcess actifs |
| `Bubble.{h,cpp}` | État "pending approval" avec boutons (voir RESEARCH-terminal-safety.md) |
| `llama_client.cpp` | (optionnel) exposer `finish_reason` pour détection troncation côté stream |

Ordre d'implémentation suggéré (du plus urgent au moins urgent) :
1. Reset `m_toolIterations` (1 ligne).
2. Guard `cb` + timeout dans `ToolRegistry::execute` (anti-blocage).
3. Validation nom + JSON dans `onToolCallsReady`.
4. Interrupt propre (`cancelAll` + role="tool" cancelled).
5. Bulles par tour.
6. Approval policy + hardline/dangerous patterns.