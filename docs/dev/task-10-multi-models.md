# Tâche 10 — Support multi-modèles

**Statut :** ⬜ A faire
**Priorité :** 🔵 Basse (feature roadmap)
**Temps estimé :** ~3 h

## Description

Permettre à l'utilisateur de sélectionner plusieurs modèles llama.cpp pour basculer entre eux sans redémarrer le serveur. Chaque modèle pourrait avoir ses propres paramètres (template, température, tokens max).

## Contexte technique

- **Fichiers concernés :** UI nouvelle (menu/combobox), fichier de config multi-modèles, client HTTP avec paramètre `"model"` dynamique

## Critères d'acceptation

- [ ] Combobox pour choisir le modèle actif
- [ ] Liste de modèles chargée depuis une config locale ou détection automatique du serveur (`/v1/models` si supporté)
- [ ] Changer de modèle n'affecte pas la conversation en cours (le contexte reste dans le serveur LLM)
- [ ] Les paramètres par modèle sont persistés dans la même config que l'étape 09

## Notes / Implémentation hint

L'API OpenAI-compatible expose `/v1/models` pour lister les modèles disponibles. Si le serveur ne la supporte pas, permettre à l'utilisateur de rentrer manuellement une liste (JSON array dans la config). Stocker chaque modèle avec ses paramètres associés :
```json
[{"name":"llama3","temp":0.7,"max_tokens":512}, {"name":"codex-small","temp":0.3,"max_tokens":1024}]
```
