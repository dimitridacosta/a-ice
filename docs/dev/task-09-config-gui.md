# Tâche 09 — Configuration GUI (adresse serveur, température, tokens max)

**Statut :** ⬜ A faire
**Priorité :** 🔵 Basse (feature roadmap)
**Temps estimé :** ~6 h

## Description

Permettre à l'utilisateur de configurer l'adresse du serveur llama.cpp, la température et le nombre max de tokens via une interface graphique intégrée dans A-ICE.

## Contexte technique

- **Fichiers concernés :** `src/` (nouvelle UI panel / dialog), fichiers de config JSON/ini persistants

## Critères d'acceptation

- [ ] Ouverture du panneau de configuration (menu → Configuration ou bouton dédié)
- [ ] L'utilisateur peut éditer : serveur, température (float 0.1–2.0), tokens max (int 1–8192)
- [ ] Persistance des paramètres (JSON/ini/QtSettings) pour qu'ils survivent au redémarrage
- [ ] Changements appliqués après clic "Appliquer" ou sauvegardés à la volée
- [ ] Validation côté UI (pas de valeur hors limites, pas d'URL vide)

## Notes / Implémentation hint

Utiliser `QSettings` pour une config simple et portable. Le panneau pourrait être un dialog modal ou un QDockWidget latéral selon l'esthétique choisie (KDE-native : dialog modal avec QFormLayout semble le plus propre).
