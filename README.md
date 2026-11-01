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

A-ICE supporte un schéma **multi-providers / multi-modèles** : chaque provider
(endpoint, prompt_format) contient ses propres modèles. Chaque modèle a un
**alias** (raccourci pour `/model`) et un **name** (nom réel envoyé à l'API).
La clé du modèle dans l'objet `models` est son alias.

Chaque provider accepte une `api_key` optionnelle (ex: token Ollama Cloud),
injectée en `Authorization: Bearer`. Un override global est possible via la
variable d'environnement `AICE_API_KEY` (prioritaire sur la config).

Chaque modèle accepte un `context_window` optionnel (fenêtre de contexte du
modèle, en tokens) — utilisé par la commande `/context` pour estimer le
reste avant le max. 0 ou absent = inconnu.

```json
{
  "providers": {
    "local": {
      "type": "openai_compatible",
      "api_url": "http://localhost:18081/v1",
      "prompt_format": "qwen",
      "models": {
        "qwen": {
          "name": "qwen36-28b-reap",
          "temperature": 0.7,
          "max_tokens": 65536,
          "stream": true
        },
        "glm": {
          "name": "glm-5.2",
          "temperature": 0.7,
          "max_tokens": 65536,
          "stream": true
        }
      }
    }
  },
  "default_provider": "local",
  "default_model": "qwen"
}
```

Un exemple est fourni dans `config.example.json` (aussi installé dans
`share/a-ice/`). Si le fichier est absent ou invalide, A-ICE retombe sur
ses valeurs par défaut (modèle local `qwen36-28b-reap` sur
`http://localhost:18081/v1`).

Le schéma legacy (single `provider` + `model` au top-level) reste accepté :
il est reconstruit comme un provider `default` avec un seul modèle.

Par défaut, A-ICE appelle `${provider.api_url}/chat/completions` (endpoint
OpenAI-compatible).

### Switch de modèle à la volée

Dans un chat, la commande `/model` change de modèle (et de provider si besoin)
sans relancer l'app :

- `/model` — affiche le modèle courant.
- `/model glm` — bascule vers le modèle d'alias `glm` (recherche par alias en
  priorité, fallback sur le nom complet `glm-5.2`).
- `/models` — liste tous les modèles disponibles (le courant est marqué `*`).

La sélection est repoussée au client (`model` + `api_url`) au prochain envoi.

### Compaction du contexte (`/compact`)

Quand la conversation s'allonge, `/compact` demande au LLM de produire un
**résumé structuré** du contexte (objectifs, décisions/faits clés, derniers
échanges, points en suspens), puis :

1. injecte ce résumé en **system prompt, après le SOUL** (comme contexte
   accumulé) pour les prochains messages ;
2. **remplace l'historique** par le résumé (le contexte est compacté).

Le fil naturel est préservé : le prompt de compaction exige la capture précise
 des derniers échanges et des identifiants exacts (chemins, commandes, config,
 nombres). `/new` repart de zéro et abandonne le résumé compacté.

### Infos contexte (`/context`)

`/context` affiche le modèle en cours + une **estimation** des tokens du
contexte (system prompt + messages, heuristique chars/4) et, si `context_window`
est configuré pour le modèle, la fenêtre max et ce qu'il reste. Pratique avant
une `/compact` pour décider s'il faut compacter.

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

## Refonte v2 (en cours, branche `v2`)

Le plan de la refonte — verre en **Qt Quick**, GUI cliente du hub zcode-acp, moteur
interchangeable — est dans [docs/v2-rewrite/PLAN.md](docs/v2-rewrite/PLAN.md).

M1 et M2 sont déjà testables sans toucher au moteur ni à l'UI actuelle : un binaire
séparé, `a-ice-m1`, qui rejoue l'identité visuelle d'a-ice (bulles flottantes
séparées dans leur morceau de verre, barre de prompt attrapable qui déplace toute la
fenêtre) sur une fenêtre de 800 × 800, et qui prompte le hub zcode-acp.

```bash
# dans dev-aice, après un make classique
cd build/poc && AICE_HUD=1 ./a-ice-m1 --size 800        # verre, bulles, barre, hub branché
./a-ice-m1 --size 800 --prompt "dis bonjour"             # un tour envoyé au hub
AICE_STRESS=1 AICE_PUMP=1 ./a-ice-m1 --size 800          # 2 000 bulles, frame time
```

Comment l'exécuter, ce qu'il mesure, et ce qu'on en a tiré :
[docs/v2-rewrite/M1-poc-ui.md](docs/v2-rewrite/M1-poc-ui.md) (le verre) et
[docs/v2-rewrite/M2-client-hub.md](docs/v2-rewrite/M2-client-hub.md) (le client du hub).

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
- [x] Support multi-modèle (commande /model)
- [x] Compaction du contexte (commande /compact)
- [x] Infos contexte (commande /context)
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
