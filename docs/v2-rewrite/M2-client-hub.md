# M2 — Le corps branché sur le hub zcode-acp

*Brief du 2026-10-31. Branche `v2`, même binaire que M1 (`a-ice-m1`) : le moteur
agentique n'est pas dans a-ice, il est au hub. Le plan : [PLAN.md](PLAN.md),
l'état du verre : [M1-poc-ui.md](M1-poc-ui.md).*

## Ce que M2 devait prouver

Les quatre points du plan, sans toucher au moteur : connexion au hub, création de
session, stream des chunks, `request_permission` remontée en boutons.

**Verdict : les quatre passent**, mesurés sur le hub réel de la machine
(`http://127.0.0.1:8377`, instance incubée pour le workspace du projet).

## Le code

```
poc/HubClient.h/.cpp  le client : HTTP pour les instances, websocket pour le pont ACP
poc/qml/Main.qml      carte du hub (état) + carte d'autorisation (les boutons)
poc/main.cpp          --prompt, AICE_TURN_QUIT, diagnostics du hub sur stderr
poc/CMakeLists.txt    Qt6::Network + Qt6::WebSockets (qt6-qtwebsockets-devel dans la box)
```

### Les étapes de la jonction

1. `GET /api/projects` — rattacher notre chemin à celui que le hub connaît (voir les pièges).
2. `GET /api/instances` — réutiliser l'instance live du workspace ; sinon
   `POST /api/instances {workspacePath}`, c'est-à-dire l'incuber (comme l'outil émissaire).
3. `ws://…/acp?instance=<id>` avec `Authorization: Bearer <jeton>`, puis `initialize`.
4. `session/new {cwd, mcpServers: []}`, puis **garde** : si le pont nous rend une
   session déjà live sur l'instance, on en mint une neuve ; si elle colle encore, on
   refuse (`instance occupée`) plutôt que de parler à la place d'un autre maitre.
5. `session/prompt {sessionId, prompt:[{type:"text", text}]}` — un tour.
6. `session/update` **filtré sur `params.sessionId`** : `agent_message_chunk` (le texte
   qui coule), `tool_call`/`tool_call_update` (la ligne « outil » de la carte du hub).
   Sans le filtre, on verrait le texte des autres sessions de l'instance.
7. Réponse du prompt → `turnEnded(stopReason`).

Tout est asynchrone : rien ne bloque le rendu, et c'est le fond du sujet — le lag de
la v1 venait de ce que le dessin et l'agent se marchaient dessus.

### Les permissions

`session/request_permission` arrive comme une **requête** du pont (un `id` **et** une
`method`), pas comme une notification. Elle devient une carte de verre au-dessus de
la barre : « le moteur demande <outil> », le `rawInput` tronçonné à 400 caractères,
et **une option = un bouton** (`optionId` renvoyé au pont ; `outcome: cancelled` si
on ne laisse pas faire).

La demande est **diffusée à toutes les connexions** de l'instance : le premier qui
répond gagne. Si une autre main a déjà tranché, le pont émet `$/zcode/ask_settled` —
on retire notre carte sans répondre, Z Code garde son pouvoir.

### Le texte du tour

Côté QML, `send()` insère la bulle user tout de suite puis `hub.send(text)`. La carte
agent **n'existe qu'au premier chunk** et grandit avec lui ; le verre suit la hauteur
(`onHeightChanged → scheduleRefresh`), donc le flou et la région de clic collent à la
bulle qui coule. Un tour sans texte ne laisse pas de bulle vide.

Attention : `--prompt` prompte depuis le C++ — il n'insère pas de bulle user.

## Lancer

| Variable / option | Effet |
| --- | --- |
| `--prompt "<texte>"` | envoie un tour dès la session ouverte (le test de M2) |
| `AICE_TURN_QUIT=1` | ferme la fenêtre dès que ce tour est terminé (mesure non interactive) |
| `AICE_HUB=<http://hote:port>` | écrase l'adresse du hub |
| `AICE_TOKEN=<jeton>` | écrase le jeton |
| `AICE_WORKSPACE=<chemin>` | écrase le workspace visé |
| `AICE_SHOT_MS=<ms>` | décale la capture `AICE_SHOT` : sert à capturer **pendant** un tour |

Sans écrasement, le client lit le bloc `hub` de `~/.config/a-ice/config.json` s'il
existe, sinon `remote` de `~/.config/zcode-acp/config.json` (`hubHost`, `hubPort`,
`token`) — c'est exactement ce que lisent loom et l'outil émissaire.

## Ce qui a été mesuré

- Jonction complète sur le hub réel : `instances du hub → incubation → pont ACP →
  instance 815997 · session …` → un tour `end_turn` avec ses chunks (1 chunk,
  7 caractères pour « BONJOUR » ; 2 chunks, 236 caractères pour une vraie phrase).
- **Incubation la première fois, réutilisation la seconde** : le second lancement a
  retrouvé l'instance live du workspace au lieu d'en incuber une autre.
- **Garde anti-détournement vérifiée** (`AICE_DEBUG=1`) : le premier `session/new`
  rendait `cc47bcf0…`, déjà déclarée live sur l'instance → mint de `cd44d880…` puis
  `4267bdc7…`, et le tour part sur la session neuve.
- Un vrai tour rendu à l'écran, capturé en pleine réponse (`AICE_SHOT_MS=12000`) :
  carte du hub en haut à gauche (`hub tour en cours… · couper`), carte agent pleine
  largeur avec la réponse du moteur, barre sous le verre.
- Tests v1 inchangés : 10/10, 17/17, config, 25/25.

## Pièges (ne pas les refaire)

- **`/home` n'est pas `/var home`** : la box de build voit `/home/dimitri/…`, la
  machine — donc le hub — voit `/var/home/dimitri/…`. Un `endsWith("/" + chemin)`
  double le slash au milieu et ne matche jamais : on incubait une instance **à chaque
  lancement** (cinq installées avant de comprendre). D'où `sameTail()` : on compare
  une queue de chemin complète, pas une concaténation.
- Les identifiants du hub sont **numériques côté JSON, chaines côté URL** : un
  `jsonId()` qui sait faire les deux.
- **`QWebSocket` n'a pas `send()`** : `sendTextMessage()` (réflexe JS à casser).
- Une requête du pont se reconnaît à `method` **et** `id` ; une réponse à `id` seul.
  Se fier au seul `id` ferait « avaler » au client une demande du pont comme si
  c'était l'acquit de sa propre requête.
- **`session/new` peut adopter la session déjà liée** à l'instance (boot-resume du
  hub) : la vérification dans `GET /api/instances` n'est pas du décor.
- **Une permission est diffusée à toutes les connexions** : ne pas répondre à une
  demande que quelqu'un d'autre a déjà tranchée (`$/zcode/ask_settled`).
- Le container n'a pas de GPU : aucune conclusion de rendu hors écran (règle de M1).

## Ce que M2 ne fait pas encore (M3)

- **Reprendre la conversation** : la GUI mint une session neuve à chaque lancement,
  l'historique reste en mémoire. Le rechargeage (`session/load`) est ce qui permet
  au pont de renvoyer à ce client la demande en cours — à brancher avec l'historique.
- Markdown parsé hors du thread de l'UI (`QTextDocument`).
- Multi-sessions, multi-instances, choix du modèle (`session/set_config_option`).
- Le bloc `hub` dans `~/.config/a-ice/config.json` est lu s'il existe, mais la source
  réelle du hub aujourd'hui est `~/.config/zcode-acp/config.json` : à trancher dans
  M3 (une seule source de vérité).
- Le badge de mesures et `AICE_*` restent des outils du POC : à ne pas garder tels
  quels dans la vraie GUI.
