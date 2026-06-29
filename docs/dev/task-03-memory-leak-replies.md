# Tâche 03 — Fuite mémoire sur `m_currentReply` (réponses écrasées)

**Statut :** ⬜ A faire
**Priorité :** 🟠 Moyenne (bug critique)
**Temps estimé :** ~20 min

## Description

Quand une nouvelle requête est lancée alors qu'une précédente n'a pas encore terminé, l'ancien `QNetworkReply` est écrasé dans le champ `m_currentReply`. Le vieux reply se retrouve **sans cleanup**, créant des fuites mémoire et des comportements indéterminés.

## Contexte technique

- **Fichiers concernés :**
  - `src/llama_client.h` — déclaration de `QNetworkReply *m_currentReply;`
  - `src/llama_client.cpp` — méthode `sendMessages()` (appel à `makeRequest()`)

## Critères d'acceptation

- [ ] Avant d'écraser `m_currentReply`, effectuer un `deleteLater()` dessus
- [ ] Idéalement, désactiver la nouvelle requête si une ancienne est en cours (option `preventConcurrentRequests`)
- [ ] Pas de valgrind warning à l'issue
- [ ] Cas testé : clic rapide multiple sur "Envoyer" → pas d'accident

## Notes / Implémentation hint

Ajouter au début de `makeRequest()` ou dans `sendMessages()` avant l'appel à `makeRequest()` :

```cpp
// Cleanup de la requête précédente
if (m_currentReply) {
    m_currentReply->deleteLater();
    m_currentReply = nullptr;
}
```

Alternative propre : désactiver le bouton d'envoi pendant un round-trip (déjà partiellement implémenté via `m_isRequesting` dans `ChatWidget`). Ajouter simplement `enabled = false` sur le sendButton.
