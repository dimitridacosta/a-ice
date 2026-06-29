# Tâche 07 — Supprimer `setLayout()` redondant sur `QMainWindow`

**Statut :** ⬜ A faire
**Priorité :** 🟡 Basse (qualité / bonnes pratiques)
**Temps estimé :** ~5 min

## Description

Dans `AiceApplet::setupUI()`, le code ajoute un QVBoxLayout avec des widgets à `m_window.get()` **et** appelle `m_window->setLayout(layout)` ensuite. C'est redondant — QMainWindow gère déjà son propre layout interne pour les widgets central et les children.

## Contexte technique

- **Fichiers concernés :** `src/AiceApplet.cpp`, méthode `setupUI()` (ligne 36)

## Critères d'acceptation

- [ ] Supprimer la ligne `m_window->setLayout(layout);`
- [ ] Vérifier que l'affichage ne change pas (la fenêtre doit être identique)
- [ ] Le code compile sans warning

## Notes / Implémentation hint

Le layout ajouté à `m_window` est automatiquement le widget central via le mécanisme interne de QMainWindow. Pas besoin d'appel explicite au setLayout sur MainWindow quand on fait un `layout->addWidget(widget)` sur la fenêtre directement ou en tant que child du layout. Vérifier toutefois qu'il n'y a pas d'ancrage de widgets (QDockWidget, etc.) dans les futures fonctionnalités qui pourraient dépendre du setLayout explicite.
