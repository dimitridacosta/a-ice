# Tâche 01 — Client HTTP : remplacer `QHttpRequest` par `QNetworkAccessManager::post()`

**Statut :** ⬜ A faire
**Priorité :** 🔴 Haute (blockant)
**Temps estimé :** ~30 min

## Description

Le fichier `src/llama_client.cpp` utilise `QHttpRequest`, qui **n'existe pas dans Qt5**. Cela rend la compilation impossible. Il faut le remplacer par `QNetworkAccessManager::post()` pour effectuer les requêtes HTTP asynchrones.

## Contexte technique

- **Fichiers concernés :**
  - `src/llama_client.h` (déclarations)
  - `src/llama_client.cpp` (implémentation, ligne ~64-73)

## Critères d'acceptation

- [ ] Supprimer l'inclusion de `<QHttpRequest>` et son utilisation
- [ ] Remplacer par `QNetworkAccessManager::post()` avec body JSON sérialisé
- [ ] Connecter le signal `finished(QNetworkReply*)` à la méthode de traitement existante
- [ ] Le code compile sans erreur
- [ ] Une requête `curl` simple vers un serveur dummy ne retourne plus d'erreur d'inclusion

## Notes / Implémentation hint

L'exemple typique QNAM :

```cpp
QNetworkAccessManager *nam = new QNetworkAccessManager(this);
auto reply = nam->post(requestUrl, jsonBody);
connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    handleResponse(reply);  // déjà existant dans LlamaClient
});
```

Astuce : garder la même signature de méthode `handleResponse(QNetworkReply*)` pour minimiser les impacts ailleurs. Il faudra peut-être stocker le reply sur l'instance ou passer via un lambda capturant correctement.
