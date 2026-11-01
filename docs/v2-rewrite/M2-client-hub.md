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

- Markdown parsé hors du thread de l'UI (`QTextDocument`).
- Multi-sessions, multi-instances, choix du modèle (`session/set_config_option`).
- Le bloc `hub` dans `~/.config/a-ice/config.json` est lu s'il existe, mais la source
  réelle du hub aujourd'hui est `~/.config/zcode-acp/config.json` : à trancher dans
  M3 (une seule source de vérité).
- Le badge de mesures et `AICE_*` restent des outils du POC : à ne pas garder tels
  quels dans la vraie GUI.

---

## Séance du 2026-11-01 — la gestion de session (le corps est branché sur un moteur)

C'est la première tranche de M3 : le corps ne ment plus une session neuve à chaque
lancement, il reprend celle du workspace et il survit aux sessions périmées du hub.

### La session vit dans QSettings, pas dans la fenetre

Cle `hub/session-<16 premiers octets du MD5 du chemin du workspace>` (le chemin tel
que le hub le connait, donc la meme cle de la box ou de l'hote). On ne l'écrit
qu'après un tour qui a répondu : une session stockée est une session qui a prouvé
qu'elle est vivante. Au lancement suivant, après `initialize`, le client appelle
`session/load` sur cette session plutôt que `session/new`.

Le pont **rejoue l'historique** pendant la requête `session/load`, avant même d'y
répondre : des `session/update` en `user_message_chunk` et `agent_message_chunk`,
chacun portant un `messageId`. Le corps n'a donc aucun historique à stocker lui-meme.
Mesuré sur la session `889055bb…` : sept messages rejoués, et un tour posé après la
reprise qui répond « la dernière chose demandée » — le moteur a bien sa mémoire.

### Le premier `session/new` rend un cadavre

Le hub relie la connexion au dernier fil qu'il tient sur l'instance, même quand ce
fil n'existe plus côté backend. Le premier `session/new` nous a rendu `444aced8…`
lancé après lancé, et le prompt répond : `code -32603`, `data.details` : « session
444aced8… no longer exists in the backend — this thread cannot be recovered ; start
a new thread ». Un second `session/new`, lui, ment une session vivante.

Le client n'en meurt plus : sur ce motif précis il mint une session neuve et relance
le tour (deux fois par tour au plus). Mesuré : échec sur la session reprise →
session neuve `4aa5c0fd…` → `end_turn` avec ses chunks. Le détail est la : le pont
met son vrai motif dans `error.data` (chaine ou `{ details }`), pas dans
`error.message`, qui ne dit que « Internal error ».

### La fin de tour vient du `turnState` du pont

La réponse JSON-RPC de `session/prompt` peut ne jamais parvenir au client : un client
tué en plein tour (SIGTERM) laisse le pont muet sur cette requête, et le corps reste
alors « generating » pour toujours — la barre ne peut plus prompter. Le hub, lui, a
bien fini : `/api/instances` passe la session de `running` à `idle`. Mesuré sur les
runs 4 à 9 : le prompt n'a jamais été acquitté.

Le pont signale l'état de tour par `$/zcode`/`_zcode/turnState` (l'agent le déclare
dans `agentCapabilities._meta.zcode.turnState`) : `{ sessionId, running, usage }`.
Le corps close le tour dès `running: false` ; la réponse JSON-RPC, quand elle vient,
passe avant elle dans les faits et ne fait que gagner la course. Mesuré : « tour
termine par le pont » → `end_turn`, 17 chunks, 486 caractères.

### Cote bulles

- **Un `messageId` = une bulle** : un chunk qui s'ajoute au même message ne rouvre
  pas une carte, et le replay de `session/load` ouvre une bulle par message reprise
  (dans l'ordre du plus vieux au plus récent, le modèle étant tenu du plus récent au
  plus vieux).
- Le `user_message_chunk` est traité comme l'`agent_message_chunk` : notre propre
  message, déjà posé par `send()`, n'est pas dupliqué quand le pont nous le renvoie.
- La carte du hub porte un bouton « conversation neuve » : il vide le modèle et
  oublie la session stockée (sinon on resterait bloqué sur une conversation usée).

### Diagnostic ajouté (GlassHelper)

`AICE_DEBUG=1` affiche « verre N carte(s) sur M candidate(s), K excluse(s) » et la
capture entoure les cartes avec leurs dimensions. Sans le compte de **candidats**,
une carte écartée pour sa taille, son clip ou son alpha ne laisse aucune trace.

**Piège de mesure retrouvé** : un run lancé depuis le terminal de l'agent ne swap
**aucune frame** (les lignes `frame time` n'apparaissent pas) — donc le verre ne se
recalcule pas après l'apparition d'une bulle, et la capture ne montre que le badge,
la carte du hub et la barre comme cartes de verre. Ce n'est pas l'état réel : le
même run en mode `AICE_STRESS` compte bien huit cartes de bulles dans le verre,
ancrées en bas. Aucune conclusion sur le verre ne doit être tirée d'un run sans
frames — à vérifier à l'œil sur une vraie fenêtre.
