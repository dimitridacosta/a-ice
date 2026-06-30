# Roadmap — Durcissement de la boucle agentique A-Ice

> Synthèse des 3 rapports de recherche (`RESEARCH-hermes-comparative.md`,
> `RESEARCH-agent-loop.md`, `RESEARCH-terminal-safety.md`,
> `RESEARCH-inline-tool-calls.md`) en un plan d'action prioritisé.
> Source d'inspiration : `~/.hermes/hermes-agent/` (Nova/Hermes).

## Priorisation

| # | Sujet | Effort | Gain | Risque si non fait |
|---|---|---|---|---|
| 1 | Reset `m_toolIterations` au `onSendMessage` | XS | Évite fausse limite atteinte | Budget d'outils qui s'épuise sur la 2e question |
| 2 | Guard `cb` + timeout dans `ToolRegistry::execute` | S | Évite boucle bloquée silencieusement | Tool qui oublie cb → app gelée |
| 3 | Validation tool_calls (nom + JSON + troncation) | S | 80% des bugs function-calling open | Tool exécuté avec args vides cassés |
| 4 | Hardline + dangerous patterns terminal | M | Sécurité de base non négociable | `rm -rf /` exécutable par le modèle |
| 5 | Approval manuelle (bulle inline) pour dangerous | M | UX + sécurité | Aucun contrôle utilisateur |
| 6 | Bulles par tour (think → tool → message) | M | Alignement brief Dim | Tout empilé dans 1 bulle |
| 7 | Repair historique (tool_calls précédents cassés) | S | Robustesse multi-tours | API rejette au tour suivant |
| 8 | Interrupt propre pendant exécution tools | M | Bouton stop fonctionne vraiment | Stop ne tue pas le QProcess |
| 9 | Sanitize tool results (fetch_url) | XS | Anti-injection | HTML distant → tool_call fantôme |
| 10 | Valider name ∈ tools avant extraction inline | XS | Anti faux positif | Prose extraite comme tool_call |

## Détail par item

### 1. Reset `m_toolIterations` — 1 ligne

```cpp
// ChatWidget.cpp — onSendMessage
void ChatWidget::onSendMessage(const QString &text) {
    m_toolIterations = 0;   // ← AJOUTER
    // … reste inchangé
}
```

### 2. Guard `cb` + timeout — `ToolRegistry::execute`

```cpp
void ToolRegistry::execute(const QString &name, const QJsonObject &args,
                            std::function<void(bool, QString)> cb) {
    auto *tool = find(name);
    if (!tool) { cb(false, "unknown tool: " + name); return; }
    auto called = std::make_shared<bool>(false);
    auto guard = [cb, called](bool ok, QString r) {
        if (*called) return;
        *called = true;
        cb(ok, std::move(r));
    };
    // Filet de sécurité : si le tool oublie cb (process bloqué, bug)
    QTimer::singleShot(130000, this, [called, guard]() {
        if (*called) return;
        *called = true;
        guard(false, "tool timed out (no callback after 130s)");
    });
    try { tool->execute(args, guard); }
    catch (...) { guard(false, "tool crashed"); }
}
```

### 3. Validation tool_calls — `onToolCallsReady` / `executeToolCallsValidated`

Nouvelle fonction `executeToolCallsValidated` qui remplace `executeToolCalls` :

- (a) Si `!m_registry->find(tc.name)` → append `role="tool"` erreur listant les
  tools dispo + passer au suivant (ne pas exécuter).
- (b) Si `tc.arguments` non vide mais `parsedArgs()` vide (JSON invalide) :
  - Si tronqué (args ne finit pas par `}` ou `]`) → append `role="tool"` "truncated"
    + `onRequestError` + return (stop).
  - Sinon → append `role="tool"` "invalid JSON, retry" + passer au suivant.
- (c) Sinon → exécuter via `m_registry->execute` + append `role="tool"` du résultat.

À la limite `m_toolIterations > 8` : injecter un `role="tool"` "limit reached" pour
chaque call (sinon API rejette au prochain sendMessages) avant de stopper.

### 4. Hardline + dangerous patterns — `TerminalTool::execute` (hardline) +
   `ChatWidget` (dangerous approval)

Hardline = block inconditionnel dans `TerminalTool::execute` avant `proc->start`.
Liste de `QRegularExpression` compilées statiquement (voir RESEARCH-terminal-safety.md
pour la liste complète). Match → ne pas exécuter, retourner erreur au modèle :
`"BLOCKED (unrecoverable): <desc>. Do NOT retry."`

Dangerous = approval manuelle dans `ChatWidget::executeToolCallsValidated` avant
d'appeler le tool. Patterns dans un `ApprovalPolicy` (classe dédiée ou simple
fonction). Si match → afficher bulle d'approbation inline :
```
⚠️ git reset --hard (destroys uncommitted changes)
[Allow once] [Allow session] [Deny]
```
Sur Deny → append `role="tool"` "user denied, do NOT retry".
Sur Allow → exécuter + ajouter pattern à `m_sessionApproved` (QSet).

### 5. Approval UI — état "pending approval" dans `Bubble`

Ajouter à `Bubble` un état avec boutons inline (pas de QDialog modal, garde le
glassmorphisme). Boutons : Allow once / Allow session / Deny. Signale le choix au
`ChatWidget` qui reprend l'exécution. ~50 lignes de UI.

### 6. Bulles par tour — `ChatWidget` restructure

Changer le paradigme : `m_currentBubble` = "bulle du tour en cours", une nouvelle
bulle par `sendMessages`. Concrètement :
- `onSendMessage` → bulle user + `startAssistantBubble()` (bulle tour 1).
- `onToolCallsReady` → finalize bulle courante (collapse thinking, setContent) +
  afficher les tool results (mini-bulle "tool" ou dans la thinking de la bulle
  courante). **Reset `m_currentBubble = nullptr`** après.
- `executeToolCalls` finit → `m_client->sendMessages` → `onThinkingChunk` /
  `onContentChunk` appellent `startAssistantBubble()` si `m_currentBubble == nullptr`
  → **nouvelle bulle** (tour 2).
- L'historique `m_messages` (API) reste découplé des bulles (UI).

### 7. Repair historique — `ChatMessage::toJson` ou pré-sendMessages

Avant chaque `sendMessages`, parcourir `m_messages` et pour chaque assistant avec
`toolCalls`, vérifier que `tc.arguments` est un JSON valide. Si non → le
re-serializer en `"{}"` ou en JSON valide. Empêche l'API de rejeter au tour suivant.

### 8. Interrupt propre — `onStopClicked` + `Tool::cancel`

- Ajouter `virtual void Tool::cancel() {}` (default no-op) dans `Tool.h`.
- `TerminalTool::cancel()` override → garder liste des `QProcess` actifs,
  `terminate()` chacun.
- `ToolRegistry::cancelAll()` → parcourt les tools, appelle `cancel()`.
- `onStopClicked` : `m_interruptRequested = true; m_client->cancel();
  m_registry->cancelAll();` + si un tool était en cours, append un `role="tool"`
  "cancelled" pour le `tool_call_id` en cours (sinon API rejette).
- Tracker `m_toolCallInProgress` (l'index en cours dans `executeToolCallsValidated`).

### 9. Sanitize tool results — helper + appel dans `executeToolCallsValidated`

```cpp
QString sanitizeToolResult(const QString &result) {
    QString s = result;
    s.remove(QRegularExpression("</?(?:tool_call|function_call|result|response|"
                               "output|input|system|assistant|user)>",
                               QRegularExpression::CaseInsensitiveOption));
    s.remove(QRegularExpression("^\\s*```(?:json|xml|html|markdown)?\\s*",
                                QRegularExpression::MultilineOption));
    s.remove(QRegularExpression("\\s*```\\s*$",
                                QRegularExpression::MultilineOption));
    if (s.size() > 2000) s = s.left(1997) + "...";
    return "[TOOL] " + s;
}
```

Appeler avant d'append le `role="tool"`. Surtout pour `fetch_url` (HTML distant).

### 10. Valider name ∈ tools avant extraction inline — `extractInlineToolCalls`

Ajouter `m_knownToolNames` (QSet) rempli dans `setTools`. Dans
`extractInlineToolCalls`, après parse du JSON inline, vérifier que `obj["name"]`
est dans `m_knownToolNames`. Si non → ne pas extraire (prose). Élimine les faux
positifs. Voir `RESEARCH-inline-tool-calls.md` §1-2 pour le code.

---

## Ordre d'implémentation suggéré

Faire par lots, en validant à chaque étape (build + test manuel "liste mes fichiers") :

**Lot 1 — Robustesse (1, 2, 3, 10)** : les 4 plus rapides et plus de gain.
Anti-blocage + anti-faux-positif + validation. ~50 lignes de code.

**Lot 2 — Sécurité (4, 5, 9)** : hardline + dangerous + sanitize. C'est le gros
chantier sécurité. ~100 lignes (patterns) + ~50 lignes (UI approval).

**Lot 3 — UX boucle (6, 7, 8)** : bulles par tour + repair + interrupt. Changement
de paradigme UI. ~80 lignes. Valider que la séquence think → tool → message → think
s'affiche bien en bulles séparées.

## Ce qu'on n'emprunte PAS d'Hermes

- Smart-approve LLM (trop lourd, latence, second endpoint).
- Sandbox docker/modal (hors scope desktop overlay).
- Context compression / trajectory_compressor (pas de problème de contexte long).
- Plugin hooks pre/post tool_call (pas de système de plugins).
- Multi-provider adapters (un seul serveur llama.cpp local).
- Cron mode / gateway mode / yolo mode (contextes qui n'existent pas).

## Validation finale

À la fin de chaque lot, tester :
1. Build dans distrobox `dev-aice` : `make -j$(nproc)` → 0 warnings.
2. Lancer `./a-ice` + question simple ("yooooo") → réponse sans tool_call.
3. Question tool ("liste les fichiers de mon home") → tool_call → résultat →
   réponse finale. Vérifier que la séquence s'affiche bien.
4. Question multi-tours (2 tool_calls d'affilée) → ne boucle pas infiniment.
5. Stop pendant une génération → interrompt vraiment.
6. (Après lot 2) Commande dangereuse simulée ("rm -rf /tmp/test") → blocked.
7. (Après lot 2) `fetch_url` sur une page avec des patterns tool_call → pas
   d'injection.