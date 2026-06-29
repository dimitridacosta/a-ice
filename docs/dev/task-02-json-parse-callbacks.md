# Tâche 02 — Parsing JSON & callbacks jamais appelés (`handleResponse`)

**Statut :** ⬜ A faire
**Priorité :** 🟠 Moyenne (bug critique)
**Temps estimé :** ~15 min

## Description

La méthode `LlamaClient::handleResponse()` a **trois bugs empilés** qui rendent l'UI mort :

1. **Silencieux sur erreur** — retour sans appeler `onError` quand la réponse n'est pas OK
2. **Construction invalide du JSON** — `QJsonObject jsonResponse = response;` ne parse rien
3. **Variables mortes** — le contenu est extrait mais jamais passé à `onSuccess`

## Contexte technique

- **Fichiers concernés :** `src/llama_client.cpp`, méthode `handleResponse()` (lignes ~76-98)

## Critères d'acceptation

- [ ] Sur erreur HTTP, appeler `onError(reply->errorString())` au lieu de retourner silencieusement
- [ ] Parser correctement : utiliser `QJsonDocument::fromJson(reply->readAll())`
- [ ] Extraire `choices[0].message.content` et appeler **soit** `onSuccess(content)` (succès) **soit** `onError(...)` (erreur JSON)
- [ ] Pas de variable locale inutile

## Notes / Implémentation hint

Code de référence :

```cpp
void LlamaClient::handleResponse(QNetworkReply *reply) {
    m_isRequesting = false;
    
    if (!reply->error() || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
        // Succès → parser le JSON et appeler onSuccess
        QByteArray body = reply->readAll();
        QJsonObject json = QJsonDocument::fromJson(body).object();
        QString content = json["choices"]
            .toArray()[0]
            .toObject()["message"]
            .toObject()["content"]
            .toString();
        if (onSuccess) onSuccess(content);
    } else {
        // Erreur → appeler onError avec le message d'erreur
        if (onError) onError(reply->errorString());
    }
    
    reply->deleteLater();  // nettoyage
}
```

Le champ `m_serverUrl` doit bien finir par `/` pour que l'URL soit correcte. Vérifier au besoin dans `setServerUrl`.
