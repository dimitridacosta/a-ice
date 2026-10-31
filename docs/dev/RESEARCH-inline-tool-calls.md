# Extraction des tool_calls inline — Analyse et durcissement

> Source : `~/.hermes/hermes-agent/agent/agent_runtime_helpers.py` (fonction
> `_strip_think_blocks`, lignes ~500-580), `agent/chat_completion_helpers.py`,
> `model_tools.py`.
> Objectif : comprendre comment les modèles open (Qwen3 inclus) émettent des
> tool_calls en texte dans `reasoning_content` ou `content`, et comment A-Ice
> peut extraire ces appels proprement sans flash visuelle.

## Le phénomène

Qwen3 (et la plupart des modèles fine-tunés pour le function-calling) **n'utilisent
pas toujours** le champ structuré `delta.tool_calls` (format OpenAI natif). Ils
écrivent souvent le tool_call en **texte** dans :

1. `reasoning_content` (le thinking) — "je vais utiliser terminal pour lister les
   fichiers…" puis un bloc JSON.
2. `content` (la réponse) — directement le bloc JSON dans la réponse.

Variantes de format qu'Hermes documente et gère (tags XML de tool-calling) :
- `tool_call`, `tool_calls`, `tool_result`, `function_call`, `function_calls`
  (balises ouvrantes/fermantes)
- `function` avec attribut `name="..."` (style Gemma)
- blocs fenced avec triple-backtick + langue (json/xml/...)
- JSON inline nu (objet avec clé `name`)
- CDATA

## Approche Hermes

`_strip_think_blocks` (`agent_runtime_helpers.py`) — **post-hoc**, appliqué après
le tour (pas pendant le stream). C'est un nettoyage par regex en 4 passes :

1. **Closed tag pairs** (case-insensitive) : thinking, reasoning, thought, etc.
2. **Tool-call XML blocks** : tool_calls, function_call, function_calls, etc.
   Le variant `function` avec attribut `name` est boundary-gated (ne strip que si le
   tag est en début de ligne ou après ponctuation + porte l'attribut name), pour
   ne pas casser le prose "Use function in JavaScript".
3. **Unterminated reasoning block** : tag ouvert sans fermeture → strip de l'open
   tag jusqu'à la fin du string. Fix pour MiniMax qui leak.
4. **Stray orphan tags** : closers orphelins.

Points clés de la philosophie Hermes :
- **Pas de masquage pendant le stream**. Le thinking est streamé brut, l'utilisateur
  voit le modèle réfléchir (tool_calls inclus = "thinking à voix haute"). Le
  nettoyage n'affecte que le content **stocké dans l'historique** avant de le
  renvoyer au modèle (pour pas qu'il relise ses propres tool_calls en XML).
- **Asymétrie intentionnelle** : on strip les closers orphelins mais **pas** les
  openers orphelins (tag non-terminé), car une fin tronquée pendant le stream peut
  encore être utile à l'utilisateur.
- **Defense-in-depth sur les tool errors** : `_sanitize_tool_error` strip les
  framing tokens (XML role tags, CDATA, code fences) des messages d'erreur tool
  avant de les injecter, pour éviter qu'un tool retourne du contenu qui ressemble à
  un tool_call et confonde le modèle (injection adversariale).

## Approche A-Ice actuelle

`LlamaClient::extractInlineToolCalls` (`llama_client.cpp:483-533`) — cherche les
blocs JSON ouvrants par `{"name":` avec un mini-parser d'accolades équilibrées
(`findJsonObjectEnd`) qui gère les strings et escapes. C'est **plus précis** que
les regex d'Hermes pour le JSON nu (Hermes ne parse pas le JSON inline, il strip
juste les tags XML).

Pipeline A-Ice :
1. Pendant le stream, on accumule `m_streamedReasoning` et `m_streamedContent`, et
   on émet `thinkingChunk` / `contentChunk` au fur et à mesure (l'utilisateur voit le
   JSON apparaître en direct — **flash**).
2. À la fin du stream, si `m_pendingToolCalls` (delta.tool_calls natifs) non vide :
   - On nettoie `m_streamedReasoning` et `m_streamedContent` via
     `extractInlineToolCalls` (au cas où le modèle a aussi écrit le tool_call en
     texte, en plus du champ natif).
   - On émet `thinkingUpdated(cleanedThinking)` + `toolCallsReady(calls, cleaned)`.
3. Si pas de tool_calls natifs : on tente `extractInlineToolCalls` sur le reasoning
   ET le content. Si on trouve des calls → `toolCallsReady`. Sinon → `responseComplete`.

### Forces de l'approche A-Ice

- **Parser d'accolades équilibrées** (`findJsonObjectEnd`) — gère le JSON imbriqué
  dans `arguments` (objets dans objets), ce que les regex ne savent pas faire
  proprement. C'est **mieux** que Hermes sur ce point.
- **Détection dans les deux flux** (reasoning + content) — Qwen3 met parfois le
  tool_call dans le thinking, parfois dans la réponse. On couvre les deux.

### Faiblesses

#### a. Flash visuelle du JSON pendant le stream

Le bloc JSON tool_call s'affiche dans la bulle pendant le stream, puis disparaît à
la fin quand `setThinking` remplace. Pas smooth mais fonctionnel. **Hermes a le même
comportement** (il ne masque pas pendant le stream non plus) → ce n'est pas un
retard, c'est un choix d'architecture.

#### b. Pas de gestion des variantes XML

A-Ice ne reconnaît que le JSON nu. Si le modèle émet des balises XML de
tool-calling (variant Gemma ou autres), on ne l'extrait pas. Qwen3 utilise plutôt
le JSON nu, donc c'est OK pour notre cas, mais à garder en tête.

#### c. Pas de nettoyage des tags orphelins

Si le modèle stream un tag de thinking ouvert sans fermeture, A-Ice affiche le tag
brut. Hermes le strip. Pas critique (Qwen3 utilise `reasoning_content` structuré,
pas des tags), mais pour d'autres modèles ce serait utile.

#### d. Risque de faux positif

`findJsonObjectEnd` cherche `{"name":` — mais si le modèle écrit en prose un objet
JSON avec une clé `name` dans une discussion (pas un tool_call), on l'extrait par
erreur. Hermes évite ça via le boundary gating (start-of-line + attribut name). A-Ice
n'a pas cette protection.

**Mitigation** : valider que l'objet JSON extrait a `name` dans l'ensemble des tools
enregistrés et `arguments` est un objet. Si `name` n'est pas un tool connu → ignorer
(ne pas extraire, laisser le texte). Ça élimine les faux positifs de prose.

## Recommandations pour A-Ice

### 1. Valider le tool name avant d'extraire

Dans `extractInlineToolCalls`, après `doc.object()`, vérifier que `obj["name"]`
correspond à un tool enregistré. Si non → **ne pas extraire**, laisser le bloc dans
le texte (c'est probablement de la prose, pas un tool_call).

```cpp
const QString name = obj.value("name").toString();
if (!m_knownToolNames.contains(name)) {
    searchFrom = idx + 1;  // skip, pas un tool_call
    continue;
}
```

`m_knownToolNames` : un `QSet<QString>` rempli quand on `setTools` (noms des specs).

### 2. Exiger `arguments` de type objet

Si `obj["arguments"]` existe mais n'est pas un objet (string, nombre, array) →
soit on tente de le re-parse (si c'est un string JSON), soit on ignore. Ne pas
extraire des tool_calls malformés.

### 3. (Optionnel) Strip les tags orphelins

Ajouter une passe regex sur le `m_streamedReasoning` avant émission pour stripper
les tags de thinking/reasoning orphelins. Utile si on change de modèle un jour. Pour
Qwen3, pas nécessaire.

### 4. (Optionnel) Smooth la flash — buffer pending

Pendant le stream, quand on détecte qu'on est potentiellement en train d'écrire un
tool_call (un `{` suivi éventuellement de `"name"`), retenir ces chunks dans un
buffer `m_pendingInline` et ne les émettre dans `thinkingChunk` que si on confirme
que ce n'est pas un tool_call (le bloc se ferme sans `name` valide, ou on atteint
un point où c'est clairement du prose).

C'est un état-machine sur le stream. Complexité modérée. **Mon avis : ne pas faire
pour l'instant** — Hermes ne le fait pas, la flash est mineure, et l'implémentation
introduirait de la latence et des bugs de buffer. On a le nettoyage final via
`thinkingUpdated` + `setContent`, c'est suffisant.

### 5. Defense-in-depth sur les tool results

Comme Hermes (`_sanitize_tool_error`), nettoyer les résultats de tools avant de
les injecter en `role="tool"` : stripper les framing tokens (XML role tags, code
fences, CDATA) pour éviter qu'un tool retourne du contenu qui ressemble à un
tool_call et confonde le modèle au prochain tour.

```cpp
QString sanitizeToolResult(const QString &result) {
    QString s = result;
    // Strip XML role-like tags
    QRegularExpression roleRe("</?(?:tool_call|function_call|result|response|"
                             "output|input|system|assistant|user)>",
                             QRegularExpression::CaseInsensitiveOption);
    s.remove(roleRe);
    // Strip code fences
    QRegularExpression openRe("^\\s*```(?:json|xml|html|markdown)?\\s*",
                             QRegularExpression::MultilineOption);
    s.remove(openRe);
    QRegularExpression closeRe("\\s*```\\s*$",
                              QRegularExpression::MultilineOption);
    s.remove(closeRe);
    if (s.size() > 2000) s = s.left(1997) + "...";
    return "[TOOL] " + s;
}
```

À appeler dans `executeToolCalls` avant d'append le `role="tool"`. Surtout utile
pour `fetch_url` qui peut retourner du HTML/JS contenant des patterns tool_call.

## Comparatif final

| Aspect | Hermes | A-Ice | Verdict |
|---|---|---|---|
| JSON nu inline | ne parse pas (strip XML seulement) | parse (accolades équilibrées) | A-Ice **mieux** |
| Tags XML tool-calling | strip par regex | non géré | Hermes mieux (mais Qwen3 = JSON nu) |
| Tags orphelins thinking | strip | non géré | Hermes mieux (pas critique pour Qwen3) |
| Faux positif prose | boundary gating | non protégé | A-Ice à durcir (valider name ∈ tools) |
| Flash pendant stream | acceptée | acceptée | Égal (choix d'architecture) |
| Sanitize tool results | oui | non | A-Ice à ajouter (fetch_url) |
| Boundary gating | oui | non | À considérer |

## Code final recommandé pour `extractInlineToolCalls`

Voir `llama_client.cpp:483-533` existant. Ajouter après `doc.object()` :

```cpp
const QJsonObject obj = doc.object();
const QString name = obj.value("name").toString();
if (!m_knownToolNames.contains(name)) {
    // Pas un tool_call valide (prose), ne pas extraire.
    searchFrom = idx + 1;
    continue;
}
const QJsonValue args = obj.value("arguments");
if (args.isString()) {
    // arguments en string JSON — re-parse
    QJsonDocument argDoc = QJsonDocument::fromJson(args.toString().toUtf8());
    if (!argDoc.isObject()) {
        searchFrom = idx + 1;
        continue;
    }
}
// … suite identique (construire ToolCall, retirer le bloc du texte)
```

Et initialiser `m_knownToolNames` dans `setTools` :

```cpp
void LlamaClient::setTools(const QJsonArray &tools) {
    m_tools = tools;
    m_knownToolNames.clear();
    for (const auto &t : tools) {
        const QJsonObject obj = t.toObject();
        const QJsonObject fn = obj.value("function").toObject();
        m_knownToolNames.insert(fn.value("name").toString());
    }
}
```