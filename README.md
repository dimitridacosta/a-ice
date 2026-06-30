# A-ICE

Petit widget KDE Plasma — chat minimaliste branché à ton serveur `llama.cpp` local.

## Fonctionnalités

- **Overlay verre (glassmorphism)** : pas de fenêtre classique. Fond
  transparent, blur KWin appliqué uniquement derrière la barre et les
  bulles — identique au rendu du panel/menu Plasma (thème Utterly-Round).
- **Barre de prompt fixe** en bas à droite, juste au-dessus du panel KDE :
  pill translucide avec icône d'envoi épurée.
- **Bulles qui montent** : une bulle par tour (utilisateur à droite, assistant
  à gauche). Les plus récentes s'empilent en bas, les anciennes montent ;
  scroll pour remonter dans l'historique.
- **Réflexion en direct** : le thinking s'affiche au fil de l'eau, puis se
  réduit automatiquement quand la réponse commence à streamer — re-dépliable
  d'un clic sur l'en-tête « 💭 Réflexion ».
- Connexion à un provider OpenAI-compatible (llama.cpp local par défaut).
- Streaming SSE (raisonnement + contenu).

## Build

> ⚠️ **Toutes les commandes de build doivent impérativement être lancées
> dans la distrobox `dev-aice` dédiée à ce projet** (Fedora 44, pour matcher
> Bazzite et avoir KF6/Qt6). Ne pas builder sur l'hôte Bazzite (atomic,
> lecture seule) ni dans une autre box.
>
> ```bash
> distrobox enter --name dev-aice
> ```

Création de la box (une seule fois, sur l'hôte Bazzite) :

```bash
distrobox create --name dev-aice --image fedora:44
distrobox enter --name dev-aice
sudo dnf install -y gcc-c++ make cmake pkgconf-pkg-config \
    qt6-qtbase-devel qt6-qtwayland-devel \
    extra-cmake-modules kf6-kwindowsystem-devel
```

Puis, **dans `dev-aice`** :

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## Install

**Dans `dev-aice`** :

```bash
sudo cmake --install .
# ou
sudo make install
```

le A## Config

A-ICE lit un fichier de config JSON décrivant le provider et le modèle.

- Emplacement par défaut : `~/.config/a-ice/config.json`
- Override : `a-ice --config /chemin/vers/config.json`
- Override de l'URL du provider uniquement : `a-ice --server-url http://localhost:8080/v1`

### Schéma

```json
{
  "provider": {
    "name": "openai_compatible",
    "api_url": "http://localhost:18081/v1",
    "prompt_format": "qwen"
  },
  "model": {
    "name": "qwen36-28b-reap",
    "temperature": 0.7,
    "max_tokens": 65536,
    "stream": false
  }
}
```

Un exemple est fourni dans `config.example.json` (aussi installé dans
`share/a-ice/`). Si le fichier est absent ou invalide, A-ICE retombe sur
ses valeurs par défaut (modèle local `qwen36-28b-reap` sur
`http://localhost:18081/v1`).

Par défaut, A-ICE appelle `${provider.api_url}/chat/completions` (endpoint
OpenAI-compatible).

### SOUL.md — personnalité de l'agent

A-ICE lit en plus un fichier **SOUL.md** (Markdown texte brut) dont le contenu
est injecté en tête des messages comme message `role="system"`. C'est là que
se définit la personnalité de l'agent (toi, ddBot).

- Emplacement principal : `~/.config/a-ice/SOUL.md` (à côté de `config.json`)
- Fallback : `share/a-ice/SOUL.md` (installé à côté de `config.example.json`)
- Si aucun SOUL.md n'est trouvé, aucun message système n'est envoyé.

Un exemple est fourni dans `SOUL.example.md`. Pour le personnaliser :

```bash
cp SOUL.example.md ~/.config/a-ice/SOUL.md
$EDITOR ~/.config/a-ice/SOUL.md
```

## Lancer ton serveur llama.cpp

```bash
./server -m ton-model.gguf -c 4096 --port 8080 --host 0.0.0.0
```

## Stack

- C++17
- Qt6 + KF6::WindowSystem (blur KWin via le protocole `ext_background_effect_v1`)
- HTTP client natif Qt pour les requêtes au provider OpenAI-compatible

## Raccourcis

- `Entrée` : envoyer
- `Échap` : masquer la fenêtre
- `Ctrl+Q` : quitter

## Roadmap

- [x] Streaming mode (server-sent events)
- [x] Markdown rendering (user / assistant / thinking)
- [x] SOUL.md system prompt
- [x] Function calling (tools) — terminal, brave_search, fetch_url
- [ ] Configuration GUI (adresse serveur, température, tokens max)
- [ ] Support multi-modèle
- [x] Icône personnalisée
- [ ] Gestion du contexte (conversation longue)

## Tools (function calling)

A-Ice peut agir comme un agent : le modèle reçoit une liste d'outils
(function-calling OpenAI-compatible) et peut les invoquer pendant sa
réponse. Les résultats sont réinjectés en `role="tool"` et le modèle
continue jusqu'à produire une réponse finale (max 8 itérations).

Activation : `"tools": { "enabled": true }` dans `config.json`.

### Outils fournis

- **`terminal`** — exécute `bash -c <command>` dans le home de l'utilisateur
  (ou `terminal_workdir` si défini). Timeout 30s par défaut (clamp 1s–120s).
  Output combiné stdout+stderr tronqué à 20000 chars.
- **`brave_search`** — recherche web via l'API Brave Search. Nécessite une
  clé API : env var `BRAVE_API_KEY` (prioritaire) ou `tools.brave_api_key`
  dans `config.json`. Retourne jusqu'à 20 résultats (title/url/snippet).
- **`fetch_url`** — fetch une URL HTTP(S), convertit le HTML en markdown
  via `QTextDocument` (zéro dépendance externe). Troncature 30000 chars.

### Cycle function-calling

```
user → LLM → tool_calls → execute → role="tool" → LLM → réponse (ou re-tool)
```

Les appels sont affichés en direct dans la zone de réflexion (🔧 name(args)
puis ✓ result-preview).
