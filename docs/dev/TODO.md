# A-ICE — TODO

Tableau de bord des tâches du projet **A-ICE**.
Chaque ligne correspond à un fichier dédié dans le même dossier (`task-*.md`).

## 🔴 Critique — Build & Stabilité (blockant)

| # | Tâche | Fichier | Statut | Est. |
|---|-------|---------|--------|------|
| ⬜ B1 | **Build : fixes incompatibilité Qt5** — `QGuiAppFactories` (Qt6), `QJsonReader` inexistant, `ChatMessage` non déclaré dans llama_client.h, `reply->body()` → `readAll()`, `QNetworkReply::cancel()` → `deleteLater()`, `toUtf8()` sur QJsonObject, `setTabPolicy` inexistant sur QTextEdit, `addWidget(HBoxLayout*)` invalide, `QFrame::setAlignment` / `setHtml`, `replace()` const-correctness | — (affecte **tous les fichiers**) | ⬜ A faire | ~1 h |
| 01 | Client HTTP : remplacer `QHttpRequest` par `QNetworkAccessManager::post()` | [task-01-http-client.md](task-01-http-client.md) | ⬜ A faire | ~30 min |
| 02 | Parsing JSON & callbacks jamais appelés (`handleResponse`) | [task-02-json-parse-callbacks.md](task-02-json-parse-callbacks.md) | ⬜ A faire | ~15 min |
| 03 | Fuite mémoire sur `m_currentReply` (réponses écrasées sans cleanup) | [task-03-memory-leak-replies.md](task-03-memory-leak-replies.md) | ⬜ A faire | ~20 min |

> **Ces 3 tâches sont bloquantes** : le code ne compile pas et ne s'exécute même pas correctement. On les fait en premier.

## 🟠 Fonctionnel — Bugs corrigés

| # | Tâche | Fichier | Statut | Est. |
|---|-------|---------|--------|------|
| 04 | Flag CLI `--server-url` (promis dans README, non implémenté) | [task-04-cli-server-url.md](task-04-cli-server-url.md) | ⬜ A faire | ~15 min |
| 05 | Implémenter `eventFilter` pour Enter / Shift+Enter dans l'input | [task-05-eventfilter-enter.md](task-05-eventfilter-enter.md) | ⬜ A faire | ~20 min |
| 06 | Nettoyage correct de l'UI dans `clearMessages()` (fuites widgets) | [task-06-clear-ui.md](task-06-clear-ui.md) | ⬜ A faire | ~15 min |

## 🟡 Qualité / Bonnes pratiques

| # | Tâche | Fichier | Statut | Est. |
|---|-------|---------|--------|------|
| 07 | Supprimer `setLayout()` redondant sur `QMainWindow` dans `AiceApplet::setupUI()` | [task-07-layout-qmainwindow.md](task-07-layout-qmainwindow.md) | ⬜ A faire | ~5 min |

## 🔵 Roadmap — Features à venir (non bloquantes)

| # | Tâche | Fichier | Statut | Est. |
|---|-------|---------|--------|------|
| 08 | Mode streaming (SSE / server-sent events) | [task-08-streaming.md](task-08-streaming.md) | ⬜ A faire | ~4 h |
| 09 | Configuration GUI (adresse serveur, température, tokens max) | [task-09-config-gui.md](task-09-config-gui.md) | ⬜ A faire | ~6 h |
| 10 | Support multi-modèles | [task-10-multi-models.md](task-10-multi-models.md) | ⬜ A faire | ~3 h |

---

## Statut global

```
🔴 Critique  : ░░░░░░░░░░ (0 / 4 terminés)
🟠 Fonctionnel: ░░░░░░░░░░ (0 / 6 terminés)
🟡 Qualité   : ░░░░░░░░░░ (0 / 1 terminés)
🔵 Roadmap  : ░░░░░░░░░░ (0 / 3 terminés)
```

---

## Méthode de développement incrémentale (dev distrobox)

Nous travaillons dans un conteneur `dev` Ubuntu 24.04 via **distrobox**. Voici notre boucle de travail :

### 🔁 Boucle standard

```
1. Modifier un fichier (.h / .cpp / CMakeLists.txt)
   ↓
2. Compiler : DBX_CONTAINER_MANAGER=docker distrobox enter --name dev -- fish -lc 'cd ~/projects/ai/projects/a-ice/build && make'
   ↓ (exit 0 = succès, exit 2 = erreurs)
3. Examiner les erreurs
   ↓ (erreurs ?)
4. Corriger — une seule famille d'erreurs à la fois
   ↓
5. Relancer le build
```

### 📋 Règles de bouclage

- **Une famille d'erreurs par itération** : on regroupe les erreurs du même type (Qt5, typing, etc.) et on les corrige toutes avant de recompiler.
- **CMake en premier** : si le configure échoue (`cmake ..`), on ne va jamais plus loin. On règle ça d'abord (variables cache corrompues, modules manquants, CMAKE_PREFIX_PATH).
- **Compiler → lire les erreurs → corriger** : chaque famille d'erreurs est identifiée dans l'output `make`, puis corrigée fichier par fichier.
- **Validation minimale par tâche** : avant de passer à la suivante, on s'assure que le projet compile au moins partiellement (`make -j$(nproc)` passe sans erreur).

### 🔑 Connaissance acquise (distrobox / KDE)

| Élément | Solution |
|---------|----------|
| CMake `KF5Config.cmake` | Installer `extra-cmake-modules`, utiliser `-DCMAKE_PREFIX_PATH="/usr/lib/x86_64-linux-gnu/cmake" -DCMAKE_MODULE_PATH="/usr/share/ECM/find-modules"` |
| `QGuiAppFactories::createApplication()` | Header Qt6, inutilisable en Qt5 — remplacer par `QApplication(argc, argv)` direct |
| `QJsonReader` | N'existe pas dans Qt — utiliser `QJsonDocument::fromJson(data, &error)` |
| `QNetworkReply::body()` / `cancel()` | Inexistants — utiliser `readAll()` et `deleteLater()` |
| `QTextEdit::TabPerIndent` | Non existant en Qt5 — utiliser `setTabChangesFocus(true)` ou gestion manuelle |
| `addWidget(QLayout*)` | invalide — créer un QWidget conteneur avec le layout imbriqué |
| Sudo dans la distrobox | Demande mot de passe (ksshaskpass indisponible) — privilégier les installations `apt` sur l'hôte |

### 💡 Astuce utile pour CMake / KDE

La variable d'environnement `KDEDIRS` ou `-DCMAKE_PREFIX_PATH` doit pointer vers `/usr/lib/x86_64-linux-gnu/cmake` (Ubuntu) plutôt que vers un chemin racine. Vérifier avec `dpkg -L extra-cmake-modules | grep cmake`. La commande `find /usr -name "KF5Config.cmake"` permet de localiser rapidement les fichiers de config KDE.

### 🎯 Ordre d'attaque recommandé

1. **Tâche B1 (Build fixes Qt5)** — c'est la base : sans compilation, tout le reste est mort.
2. **Tâches 01-03** — corriger les bugs critiques du client HTTP / JSON / mémoire (liés aux erreurs de build identifiées hier).
3. **Tâche 04 CLI** — rendre `--server-url` opérationnel, testable avec `make && ./a-ice --server-url http://localhost:8080`.
4. Le reste suit dans l'ordre du tableau.

---
