# Tâche 08 — Mode streaming (SSE / Server-Sent Events)

**Statut :** ⬜ A faire
**Priorité :** 🔵 Basse (feature roadmap)
**Temps estimé :** ~4 h

## Description

Remplacer le mode "one-shot" (`stream=false`) par le mode SSE de llama.cpp pour un effet typing progressif. Améliore fortement l'UX en affichant les réponses mot par mot au lieu d'un bloc à la fin.

## Contexte technique

- **Fichiers concernés :**
  - `src/llama_client.h` — ajouter une méthode/streaming callback, gestion SSE parsing
  - `src/llama_client.cpp` — utiliser `stream=true`, parser chunk par chunk
  - `src/ChatWidget.{h,cpp}` — exposer `appendToBubble()` pour l'effet streaming

## Critères d'acceptation

- [ ] `--stream` (ou toujours activé, via flag config)
- [ ] Les tokens sont affichés progressivement dans la bulle en cours (`m_currentBubble`)
- [ ] L'indicateur "en train d'écrire" reste visible pendant le streaming
- [ ] La fin de stream est bien détectée (chunk avec `finish_reason`)

## Notes / Implémentation hint

LLama.cpp supporte les SSE :
```json
{"choices":[{"delta":{"content":"token"},"index":0}]}
```

Un parsing chunk-by-chunk sur le body brut suffit pour extraire chaque token. Garder une copie du QNetworkReply et consommer `readyRead()` ou faire un polling régulier via `QTimer` ou via des données partielles dans handleResponse avec streaming activé.
