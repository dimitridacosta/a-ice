# A-ICE

Petit widget KDE Plasma — chat minimaliste branché à ton serveur `llama.cpp` local.

## Fonctionnalités

- Interface chat ultra-minimaliste, style KDE natif
- Connexion à ton serveur llama.cpp local (localhost:8080 par défaut)
- Support streaming (à venir)
- Thème sombre intégré

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## Install

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

## Lancer ton serveur llama.cpp

```bash
./server -m ton-model.gguf -c 4096 --port 8080 --host 0.0.0.0
```

## Stack

- C++17
- Qt5 + Plasma SDK (KF5)
- HTTP client natif Qt pour les requêtes au serveur llama.cpp

## Roadmap

- [ ] Streaming mode (server-sent events)
- [ ] Configuration GUI (adresse serveur, température, tokens max)
- [ ] Support multi-modèle
- [ ] Icône personnalisée
- [ ] Gestion du contexte (conversation longue)
