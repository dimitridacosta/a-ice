---
date: 2025-07-20
type: dev-instruction
priority: high
status: resolved
tags: [bug, brave-search, Qt, JSON]
---

# Bug Brave Search — `invalid JSON: illegal number`

## Résolution (2026-07-20)

Cause confirmée : hypothèse **gzip** (piste 3). Le header `Accept-Encoding: gzip`
était fixé manuellement via `req.setRawHeader(...)`. Or Qt désactive sa
décompression gzip automatique dès que ce header est présent explicitement
dans la requête — `QNetworkAccessManager` renvoie alors le corps gzip brut,
que `QJsonDocument::fromJson` échoue à parser ("illegal number" sur le magic
byte `0x1f`).

Correction appliquée dans `src/tools/BraveSearchTool.cpp` :
- suppression du header `Accept-Encoding: gzip` manuel (laisse Qt gérer la
  décompression automatiquement) ;
- ajout d'un log `qDebug()` (Content-Type, taille du body, preview) en cas
  d'échec de parsing JSON futur, pour faciliter un diagnostic ultérieur.

Build + tests validés dans le container dédié `dev-aice` (ECM + KGlobalAccel
disponibles), binaire redéployé dans `build/`.

## Résumé

L'outil `brave_search` plante systématiquement avec l'erreur :

```
[a-ice] Brave search: invalid JSON: illegal number
```

Les 8 itérations de tool calls sont consommées avant qu'une recherche réussie ne soit effectuée.

## Ce qu'on sait

- **Brave API est fonctionnelle** : un `curl` direct avec la même clé retourne `200 OK` avec du JSON valide
- **L'URL construite est correcte** : `https://api.search.brave.com/res/v1/web/search?q=...&count=...`
- **Le code C++ est basé sur Qt** (`QNetworkAccessManager`)

## Fichier concerné

`/var/home/dimitri/projects/ai/projects/a-ice/src/tools/BraveSearchTool.cpp`

## Analyse

Le bug est probablement **côté client Qt**, pas côté Brave. Les pistes :

1. **Proxy / Firewall** — un proxy d'entreprise ou un firewall pourrait intercepter la requête HTTPS et renvoyer une page HTML avec un HTTP 200 (faux positif). Le parseur JSON de Qt crasherait sur ce HTML.

2. **SSL/TLS** — si le certificat n'est pas reconnu, Qt NetworkAccessManager pourrait renvoyer une erreur HTML au lieu de la vraie réponse.

3. **Compression gzip** — le header `Accept-Encoding: gzip` est ajouté (ligne ~80). Qt gère normalement le gzip automatiquement, mais une interaction bizarre avec le décodeur de Qt pourrait produire un corps corrompu.

4. **Buffer incomplet** — `r->readAll()` peut ne pas tout lire si le flux est interrompu.

## Diagnostic à effectuer

### Étape 1 : Ajouter du debug dans BraveSearchTool.cpp

Juste avant la ligne `QJsonParseError parseErr;` (vers ligne 120), ajouter :

```cpp
qDebug() << "=== BraveSearch debug ===";
qDebug() << "HTTP code:" << httpCode;
qDebug() << "Content-Type:" << r->header(QNetworkRequest::ContentTypeHeader).toString();
qDebug() << "Body size:" << body.size();
qDebug().noquote() << "Body preview:" << QString::fromUtf8(body.left(500));
qDebug() << "=========================";
```

### Étape 2 : Compiler et relancer

```bash
cd /var/home/dimitri/projects/ai/projects/a-ice/build
cmake --build . --target a-ice
```

Puis relancer a-ice et tenter une recherche Brave.

### Étape 3 : Interpréter les résultats

- **Si `Content-Type` n'est pas `application/json`** → un proxy/firewall intercepte la requête
- **Si `Body size` est très petit ou le preview contient `<html>`** → confirmation du proxy
- **Si `Body size` est 0** → problème de lecture du flux (SSL/TLS ?)
- **Si tout semble correct** → le bug est ailleurs (probablement dans le parsing JSON lui-même)

## Pistes de correction

### Si c'est un proxy/firewall

- Configurer Qt pour ignorer le proxy système :
  ```cpp
  QNetworkProxyFactory::setUseSystemConfiguration(false);
  ```
- Ou bypasser via variable d'environnement : `NO_PROXY=api.search.brave.com`

### Si c'est SSL/TLS

- Vérifier que les certificats sont installés
- Ajouter temporairement `sslConfiguration.setPeerVerifyMode(QSslSocket::VerifyNone)` (pas en prod !)

### Si c'est le gzip

- Retirer le header `Accept-Encoding: gzip` dans `BraveSearchTool.cpp` (ligne ~80)
- Laisser Qt gérer la compression automatiquement

### Si c'est le parsing JSON

- Vérifier que `QJsonDocument::fromJson` reçoit bien un QByteArray complet
- Envisager un fallback : si le parse échoue, logger le body complet et retourner une erreur descriptive

## Note sur la limite d'itérations

Actuellement codée en dur à 8 dans `ChatWidget.cpp` (ligne 690) :

```cpp
if (++m_toolIterations > 8) {
```

Cette limite devrait être augmentée (15-20 semble raisonnable) pour éviter de bloquer prématurément pendant le debug.

## Fichiers à modifier

1. `src/tools/BraveSearchTool.cpp` — debug + correction
2. `src/ChatWidget.cpp` — augmenter la limite d'itérations (optionnel)
