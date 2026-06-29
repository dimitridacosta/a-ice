# Tâche 06 — Nettoyage correct de l'UI dans `clearMessages()`

**Statut :** ⬜ A faire
**Priorité :** 🟠 Moyenne (bug fonctionnel)
**Temps estimé :** ~15 min

## Description

La méthode `ChatWidget::clearMessages()` vide seulement la liste mémoire mais ne supprime pas les widgets créés dans le layout. Résultat : les "bulles" restent affichées à l'infini et s'empilent, même si leur contenu est devenu invalide (dangling).

## Contexte technique

- **Fichiers concernés :** `src/ChatWidget.h` et `src/ChatWidget.cpp`, méthode `clearMessages()` (ligne 78)

## Critères d'acceptation

- [ ] Supprimer visuellement les widgets du layout après le clear mémoire
- [ ] Vérifier avec un clic sur "Envoyer", puis `clearMessages()`, que plus aucun widget ne reste dans le scroll area
- [ ] Optionnel : vérifier via QSignalSpy ou debug que chaque old bubble est bien supprimé (ou a `setParent(nullptr)`)

## Notes / Implémentation hint

L'itération sur les widgets enfants du layout est la plus simple ici, avec suppression une par une. Alternative plus performante si il y en a beaucoup : recréer le conteneur de messages ou utiliser un QStackedWidget. Mais vu qu'on est à un usage desktop non temps réel critique, l'itération reste acceptable.
