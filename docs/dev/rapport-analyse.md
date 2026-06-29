# Rapport d'analyse — A-ICE

## 🧊 Vue d'ensemble

**A-ICE** est un petit widget KDE Plasma chat branché sur un serveur `llama.cpp` local. Deux modes de lancement : **standalone** (fenêtre Qt classique) ou **applet Plasma**. Stack C++17 + Qt5 + KF5 Plasma SDK.

### Architecture

```
main.cpp
  ├── standalone  → fenêtre Qt (dev/test)
  └── --applet    → Plasma quickapplet

AiceApplet        → controller top-level, gère la fenête + le chat
ChatWidget        → UI chat (bulles, input, scroll)
LlamaClient       → HTTP client → llama.cpp /v1/chat/completions
```

### Fichiers principaux

| Fichier | Rôle |
|---|---|
| `main.cpp` | Entrée, dual mode standalone/applet |
| `AiceApplet.{h,cpp}` | Fenêtre + orchestration du widget chat |
| `ChatWidget.{h,cpp}` | UI chat — bulles, input, typewriter indicator |
| `llama_client.{h,cpp}` | Client HTTP pour llama.cpp (API OpenAI-compatible) |
| `metadata.xml` / `aice.desktop` | Install Plasma (applet + lanceur) |

---

## ⚠️ Problèmes rencontrés

### 🔴 Critique — Le code ne compile pas

**`QHttpRequest` n'existe pas dans Qt5.** La ligne `#include <QHttpRequest>` et l'utilisation dans `llama_client.cpp` (lignes 3-65) vont rendre la compilation impossible. Il faut utiliser `QNetworkAccessManager::post()` à la place.

### 🟠 Bug critique — Callbacks jamais appelés sur erreur

Dans `handleResponse` :
```cpp
if (!reply->isOk()) {           // → return silencieux
    ...                          // sans appeler onError !
    return;
}

QString response = reply->body();  // → jsonResponse construit mais JAMAIS utilisé
QString content = choice["message"]["content"].toString();  // → variable locale morte
```

Trois bugs empilés :
1. **Silencieux sur erreur** — ni `onSuccess` ni `onError` appelés, donc l'UI reste bloqué dans un état "en train d'écrire" à l'infini
2. **Construction invalide** — `QJsonObject jsonResponse = response;` ne parse pas du tout le JSON (il faut `QJsonDocument::fromJson`)
3. **Variables mortes** — `content` est construite et jamais utilisée pour appeler `onSuccess(content)`

### 🟡 Bugs importants

- **`clearMessages()` est vide** : vide la liste mémoire mais ne nettoie pas les widgets de l'UI. Fuites visuelles garanties
- **Pas de gestion du flag CLI** — Le README promet `--server-url`, il n'existe pas dans le code (`main.cpp` ignore ce flag)
- **`m_currentReply` écrasé sans `deleteLater()`** : si un nouveau message part avant la réponse précédente, fuite mémoire + comportement indéterminé

### 🟡 Style / Architecture

- **Layout sur QMainWindow** : le `setLayout()` dans `setupUI()` est redondant avec l'ajout direct de widgets au layout (QMainWindow gère déjà son propre layout interne)
- **Pas de `eventFilter` implémenté** : `ChatWidget::installEventFilter(this)` est appelé mais aucune méthode `eventFilter()` n'est définie — le Enter pour envoyer ne fonctionnera probablement pas comme attendu
- **Mode applet plasma incomplet** : `AiceApplet` hérite de `QMainWindow`, pas de `PlasmaQuickApplet`. Le mode `--applet` va juste lancer une fenêtre Qt classique, pas un vrai applet Plasma

---

## 📋 Roadmap — Priorités techniques

1. **Corriger le client HTTP** (remplacer `QHttpRequest` par `QNetworkAccessManager::post()`, fixer la réponse)
2. **Ajouter le support CLI pour l'URL du serveur** (`--server-url`)
3. **Implémenter `eventFilter`** dans ChatWidget pour gérer Enter/Shift+Enter
4. **Vider correctement l'UI** dans `clearMessages()`

Les features de la roadmap sont solides (streaming, config GUI, multi-modeles, contexte conversationnel). Le streaming notamment est le gros upgrade UX — les SSE events de llama.cpp seraient bien plus fluides qu'un appel one-shot.
