# Tâche 04 — Flag CLI `--server-url`

**Statut :** ⬜ A faire
**Priorité :** 🟠 Moyenne (fonctionnel)
**Temps estimé :** ~15 min

## Description

Le README mentionne le flag `a-ice --server-url http://localhost:8081`, mais il n'existe pas dans le code. Il faut l'implémenter.

## Contexte technique

- **Fichiers concernés :**
  - `src/main.cpp` — ajouter le parser de CLI pour le nouveau flag
  - `src/ChatWidget.h/.cpp` ou `AiceApplet` — passer l'URL au widget/client au démarrage

## Critères d'acceptation

- [ ] Flag ajouté via `QCommandLineParser::addOption("server-url", ...)` dans `main.cpp`
- [ ] Si flag fourni, override l'URL par défaut (`http://localhost:8080`) de `LlamaClient` avant `show()` / `init()`
- [ ] Si pas de flag, la valeur par défaut reste valide
- [ ] Vérifier que la validation d'URL est correcte (pas d'URL malformée qui crash le programme)

## Notes / Implémentation hint

Le `LlamaClient` a déjà un setter `setServerUrl(QString)` accessible via `ChatWidget::setServerUrl()` ou directement via l'applet. L'intégration la plus propre : ajouter une méthode dans `AiceApplet` pour configurer le serveur avant tout affichage, ou passer l'URL comme argument constructeur du client HTTP.
