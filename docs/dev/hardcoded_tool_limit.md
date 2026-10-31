---
date: 2025-07-20
type: dev-instruction
priority: medium
status: resolved
tags: [refactor, config, hardcode, tool-limit]
---

# Remplacer le 8 codé en dur par un paramètre de config

## Résolution (2026-07-20)

Implémenté tel que décrit ci-dessous : `tools.max_tool_iterations` dans
`config.json` (défaut 8), lu dans `Config::load()`, exposé via
`Config::Tools::maxToolIterations`, et consommé par `ChatWidget` (nouveau
membre `m_maxToolIterations`, initialisé dans `setupTools()`) à la place du
`8` en dur dans la boucle anti-tool-loop. Build + tests validés dans
`dev-aice`.

## Résumé

La limite de tool calls par message (actuellement **8**) est codée en dur dans `ChatWidget.cpp`. Elle doit devenir un paramètre lisible depuis `config.json`.

## Fichier concerné

`/var/home/dimitri/projects/ai/projects/a-ice/src/ChatWidget.cpp` (ligne 690)

### Code actuel

```cpp
// 3. Limite anti-boucle : on tolère 8 tours de tools par message utilisateur.
if (++m_toolIterations > 8) {
```

## Ce qu'il faut faire

### 1. Ajouter le paramètre dans `config.json`

```json
{
  "provider": { ... },
  "model": { ... },
  "tools": {
    "enabled": true,
    "brave_api_key": "...",
    "terminal_workdir": "",
    "max_tool_iterations": 8
  }
}
```

### 2. Lire le paramètre dans la config

Dans `Config.cpp`, ajouter le parsing de `max_tool_iterations` dans la section `tools` :

```cpp
// Dans la section de chargement de la config (après le parsing de "brave_api_key")
if (tools.contains(QStringLiteral("max_tool_iterations"))) {
    m_tools.maxToolIterations = tools.value(QStringLiteral("max_tool_iterations")).toInt(8);
}
```

### 3. Exposer la valeur dans la classe Config

Dans `Config.h`, ajouter un membre :

```cpp
struct Tools {
    bool enabled = true;
    QString braveApiKey;
    QString terminalWorkdir;
    int maxToolIterations = 8;  // ← NOUVEAU
};

// Dans la classe Config principale :
const Tools& tools() const { return m_tools; }
```

### 4. Remplacer le 8 dans ChatWidget.cpp

```cpp
// Avant :
if (++m_toolIterations > 8) {

// Après :
if (++m_toolIterations > m_config.tools().maxToolIterations) {
```

(Vérifier comment `ChatWidget` accède à la config — probablement via un pointer ou un reference passé en constructeur.)

### 5. Ajouter un fallback par défaut

Si le paramètre est absent dans `config.json`, la valeur par défaut doit rester **8** pour ne pas casser les configs existantes.

## Bonus : logging

Ajouter un log au démarrage pour confirmer la valeur chargée :

```cpp
qDebug() << "Max tool iterations:" << m_config.tools().maxToolIterations;
```

## Vérification

Après modification :

1. Lancer a-ice sans `max_tool_iterations` dans `config.json` → doit fonctionner avec la valeur par défaut (8)
2. Lancer a-ice avec `"max_tool_iterations": 15` → la limite doit être de 15
3. Vérifier que le debug du Brave Search fonctionne correctement avec la nouvelle limite

## Fichiers à modifier

1. `src/Config.h` — ajouter le membre `maxToolIterations`
2. `src/Config.cpp` — parser le paramètre
3. `src/ChatWidget.cpp` — remplacer le 8 par `m_config.tools().maxToolIterations`
4. `config.json` — ajouter `max_tool_iterations` (optionnel, la valeur par défaut suffit)
