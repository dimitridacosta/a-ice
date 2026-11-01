# M3 — Markdown, permissions, questions à choix

*Note de recherche du 2026-11-01, avant développement. Branche `v2`. Tout ce qui est
marqué « mesuré » vient d'une sonde sur le hub réel (`127.0.0.1:8377`, ZCode 0.76.3),
pas d'une supposition. Le plan général : [PLAN.md](PLAN.md) ; la jonction ACP :
[M2-client-hub.md](M2-client-hub.md).*

## 1. Où on en est vraiment

| Ingrédient | État dans le POC | Ce qui manque |
| --- | --- | --- |
| Markdown | **pas porté** : la carte affiche du texte brut (`Text.PlainText`) | le rendu, et le parsing hors du thread UI |
| Permissions (`session/request_permission`) | **partiel** : une carte, un bouton par option, `ask_settled` géré | groupement par `kind`, `rawInput`/sortie d'outil, file d'attente, clavier |
| Questions à choix (`AskUserQuestion`) | **pas porté** | `elicitation/create` : le formulaire ACP, choix unique et multi-choix, free-text |
| Outils (`tool_call`) | une seule ligne « outil … » dans la carte du hub | carte d'outil, sortie de terminal, statuts |
| Plan de l'agent (`plan`) | ignoré | carte de suivi (entries, statuts) |
| Commande slash (`available_commands_update`) | ignoré | autocomplétion de la barre |
| Choix du modèle/mode (`config_option_update`) | ignoré | réglage dans l'UI (`session/set_config_option`) |
| Pensées (`agent_thought_chunk`) | ignoré | bulle « pensée » repliable ou masquée |

La question que tu poses — « le protocole ACP gère tout ça normalement » — est vraie :
**rien de ce qui suit n'est à inventer, tout est déjà dans le protocole et déjà envoyé
par ton moteur.** Le travail est de côté client.

---

## 2. Ce que le protocole rend, mesuré sur ton hub

### 2.1 `session/update` — l'union complète

```
user_message_chunk | agent_message_chunk | agent_thought_chunk |
tool_call | tool_call_update | plan | plan_update | plan_removed |
available_commands_update | current_mode_update | config_option_update |
session_info_update | usage_update
```

Mesuré dans une vraie conversation (`agent_thought_chunk`, `tool_call` `kind:"execute"`,
`plan`, `usage_update`, `session_info_update`, `available_commands_update`,
`config_option_update`) :

- **`agent_thought_chunk`** porte son propre `messageId` (`thinking_…`) : le « pourquoi
  je fais ça » du moteur, à part du message. Il arrive pendant un tour (vu sur un tour
  réel : `{"content":{"type":"text","text":"Thinking…"}}`).
- **`tool_call`** : `{ toolCallId, title, kind, status, rawInput, content, _meta }`.
  Mesuré : `{"title":"Bash: ls -la","kind":"execute","status":"pending",
  "rawInput":{"command":"ls -la","description":"…"},"_meta":{"claudeCode":{"toolName":"Bash"},
  "terminal_info":{"terminal_id":"call_zhnjj0bv"}},"content":[{"type":"terminal",
  "terminalId":"call_zhnjj0bv"}]}`. Les statuts : `pending` → `in_progress` →
  `completed` (`failed` dans le schéma).
- **Sortie de terminal sans la demander au client** : quand le client
  n'annonce pas la capacité `terminal`, le moteur met la sortie dans
  `_meta.terminal_output.data` et la fin dans `_meta.terminal_exit.exit_code`
  (mesuré : la sortie complète de `ls -la`, puis `exit_code: 0`). On n'a donc **pas**
  à implémenter `terminal/create` pour afficher une sortie d'outil.
- **`plan`** : `{ entries: [{ content, priority: high|medium|low,
  status: pending|in_progress|completed }] }` (+ `plan_update`, `plan_removed`).
- **`available_commands_update`** : `{ availableCommands: [{ name, description,
  input:{hint} }] }` — la liste des `/auto`, `/compact`, … Mesuré : `auto`, `compact`, …
- **`config_option_update`** / `session/new` : `configOptions[]` avec
  `{ id:"model"|"mode"|"thought", name, category, type:"select", currentValue,
  options:[{value,name}] }` — mesuré : la liste complète des modèles, y compris
  `currentValue` qui change seule en cours de tour. Changer : `session/set_config_option`
  `{ sessionId, configId:"model", value }`.
- **`usage_update`** : `{ used, size }` (ex. `used 20943, size 1000000`) — utile dans le
  badge, pas dans la conversation.
- **`session_info_update`** : `{ title, updatedAt }` — le titre que le moteur donne à la
  conversation (utile pour le sélecteur de sessions).

### 2.2 Permissions : ce qu'il faut savoir

Schéma ACP : `RequestPermissionRequest { sessionId, toolCall: ToolCallUpdate,
options: [{ optionId, name, kind }] }`, réponse `{ outcome: { outcome:"cancelled" } |
{ outcome:"selected", optionId } }`. `kind` est un **indice** d'icône/traitement :
`allow_once | allow_always | reject_once | reject_always`.

Trois points mesurés dans le pont (`zcode-acp-server`) qui changent la façon de faire :

1. **`kind` n'est pas borné aux quatre valeurs ACP.** Le pont admet
   `allow_project` et `allow_session` (`interaction/adapter.js:245-248`). Il faut traiter
   `kind` comme un indice, pas comme un contrat.
2. **Le pont injecte ses propres options.** Pour certains outils il ajoute
   `optionId:"allowSession"`, `kind:"allow_always"` (« toujours dans cette session »).
   Les libellés viennent du moteur : on les affiche tels quels, on ne les réécrit pas.
3. **Le client n'envoie que l'`optionId`.** Le pont, côté serveur, réassocie l'option
   choisie à sa décision autoritaire (`acpPermissionResponseToZcode`) — un simple
   `{decision:"allow"}` inventé par le client « dégrade un « always » en un one-shot ».
   Le POC fait déjà juste (il renvoie `optionId`) : ne pas le casser.
4. **Politique du moteur, pas du client** : `ls -la` puis `touch /tmp/… && ls -la` ont
   été exécutés **sans** demande d'autorisation (mode `guarded` du moteur, qui auto-autorise
   ce qu'il juge sûr, et bloque ce qui ne l'est pas). Un client ne doit donc jamais
   compter sur l'existence d'une demande : il doit aussi savoir n'en jamais recevoir.
5. **`$/zcode/ask_settled` porte un `kind`** : `"permission"` ou `"elicitation"`. Le POC
   ne gère que `permission` aujourd'hui (une seule branche dans `onBridgeNotification`) —
   à corriger en même temps que les formulaires.

### 2.3 Les questions à choix : `AskUserQuestion`, et le vrai chemin s'appelle `elicitation/create`

C'est la découverte de la séance. ZCode a un outil `AskUserQuestion` (questions à une ou
plusieurs réponses), et le pont a **deux chemins** :

- **Chemin de secours (sans capacité client)** : un `session/request_permission` par
  question ; et pour une question **multi-select**, une popup **par option** en paires
  yes/no (`options` = `[opt0_yes, opt0_no, opt1_yes, opt1_no, …]` —
  `handlers/server-requests.js:586-600`). C'est ce que ton hub fait aujourd'hui avec
  a-ice, parce que le POC annonce `clientCapabilities: {}`.
- **Chemin préférable : le formulaire ACP.** Le pont teste
  `clientCapabilities.elicitation?.form != null` (`server.js:1004`) et, si le client
  l'annonce, envoie **un seul `elicitation/create` avec un formulaire qui porte toutes
  les questions**.

**Mesuré en direct** (sonde avec `clientCapabilities: { elicitation: { form: {} } }`,
session neuve sur l'instance `534584`, prompt demandant une question à trois choix) :

```json
{ "mode": "form", "sessionId": "1528e25d…", "toolCallId": "call_zu4s34hj",
  "message": "Please answer the question.",
  "requestedSchema": { "type": "object", "required": [], "properties": {
    "q_0": { "type": "string", "title": "Quelle boisson préfères-tu ?",
             "oneOf": [ { "const": "Thé", "title": "Thé" },
                        { "const": "Café", "title": "Café" },
                        { "const": "Jus",  "title": "Jus" },
                        { "const": "__skip__", "title": "Skip this question" } ] },
    "q_0_other": { "type": "string",
                   "title": "↳    or type a custom value (overrides the selection)" } } } }
```

Réponse envoyée par la sonde : `{ "action": "accept", "content": { "q_0": "cafe" } }`.
Le moteur a répondu dans la foulée : outil `completed` avec
`rawOutput: "User has answered your questions: \"Quelle boisson préfères-tu ?\"=\"cafe\"…"`,
puis le message agent « Café ☕ ». Le tour s'est terminé normalement (`end_turn`).

Les règles du formulaire (schéma ACP `CreateElicitationRequest` /
`CreateElicitationResponse`) :

- champs primitifs seulement : `string` (avec `enum` ou `oneOf` = **choix unique**),
  `array` avec `items.anyOf` = **choix multiple**, `number`, `integer`, `boolean` ;
- chaque option est un `EnumOption { const, title, description }` ;
- `required` peut être vide : **toute question peut être laissée** ;
- l'option sentinelle `__skip__` et le champ compagnon `q_<i>_other` (free-text) sont une
  convention du pont : le free-text **écrase** le choix, et en multi-select il s'ajoute
  aux choix ;
- réponse : `{action:"accept", content:{…}}` | `{action:"decline"}` | `{action:"cancel"}` ;
  `content` est conforme au schéma demandé (valeurs : string, number, booléan, tableau de
  strings) ;
- notification `CompleteElicitation` + `ask_settled {kind:"elicitation"}` : la demande est
  diffusée à **toutes** les connexions, la première réponse gagne, les autres voient leur
  carte disparaître.

**Donc, côté a-ice : déclarer `elicitation: { form: {} }` dans `initialize` et rendre un
formulaire** — c'est ce qui évite d'avoir N popups yes/no pour une seule question à
choix multiple. Le chemin `request_permission` doit rester géré (le moteur peut l'utiliser
pour les autorisations d'outils).

### 2.4 Ce que le moteur ne nous dit pas

- `agentCapabilities.elicitation` est `null` sur ZCode 0.76.3 : la capacité est côté
  **client**, et c'est notre annonce qui déclenche le formulaire. Ne pas s'attendre à ce
  que l'agent la publie.
- Les demandes peuvent ne **jamais** arriver (politique d'auto-autorisation du moteur).
- Aucun canal « question hors tour » : une question arrive toujours pendant un tour, et
  un `session/cancel` ou un nouveau prompt l'interrompt côté moteur.

---

## 3. Le markdown de la v1 : ce qui le rendait juste

`src/Bubble.cpp:20-105`, à porter à l'identique dans l'esprit :

1. **Le dialecte** : `QTextDocument::setMarkdown(md, QTextDocument::MarkdownDialectGitHub)`
   — CommonMark + tables + fenced code + task lists. C'est le moteur de rendu, pas un
   parseur maison.
2. **L'air est cuit dans le document, pas dans un CSS** : `applyMdSpacing()` pose
   `topMargin`/`bottomMargin` dans chaque `QTextBlockFormat` (0 au premier bloc, 0 au
   dernier, 14/7 pour les titres, 5/5 pour paragraphes et listes). Choix justifié mesuré
   à l'époque : Qt écrase les marges du `<style>` avec des marges inline, donc le CSS ne
   gagne pas sur ce point.
3. **La couleur du thème** : Qt crache `color:#000000` ; on le remplace par `#e9eaee`.
4. **Un CSS injecté** pour ce qui est purement visuel : `code`/`pre` avec fond blanc
   translucide et coins arrondis, `blockquote` avec barre à gauche, `table` avec
   bordures, `hr` en filet.
5. **La mesure avec le même moteur** : `measureMarkdown()` rejoue le même
   `setMarkdown` + `applyMdSpacing`, `documentMargin = 0`, `setTextWidth(w)`, et lit
   `idealWidth` / `size().height()` — c'est ce qui collait au rendu réel et évitait les
   padding faux relevés dans le brief M1.

À garder intact : le dialecte GitHub, l'air par bloc, le fond des blocs de code, la
mesure alignée sur le rendu. Ce qui change en Qt Quick : le dessin (plus de `QLabel`) et
le fait que la mise en page se passe dans le thread de rendu.

---

## 4. Portage Qt Quick — les trois voies, et la recommandation

| Voie | Comment | Ce qu'elle coûte | Verdict |
| --- | --- | --- | --- |
| A. Un seul `Text` par bulle, `textFormat: Text.RichText`, HTML = `toHtml()` du document entier | quasi gratuit | Qt Quick n'est pas `QLabel` : subset HTML, pas de mesure `idealWidth` exposée, et **on perd la carte qui épouse son texte** (largeur = largeur de la zone) | voie de secours, pas la bonne |
| B. **Bulles par blocs** : un parseur C++ découpe le markdown en blocs, chaque bloc a son HTML de fragment, la carte contient une `Column` de `Text` | ~150 lignes de parseur + un modèle de blocs | la carte continue d'épouser son contenu, le code a son fond, les titres leur air, et le parsing est hors thread | **recommandée** |
| C. Dessin maison (`QTextDocument` → image, ou `QSGGeometry`) | lourd | hors sujet pour maintenant | écartée |

Détail de la voie B :

- **Découpage à la source**, pas à partir du document Qt : machine à états simple sur le
  texte brut (fenced ``` → bloc de code ; `#`→ titre ; `- * + 1.` → liste ; `- [ ]` →
  task list ; `>` → citation ; lignes `|` → tableau ; `---`/`***` → filet ; sinon
  paragraphe jusqu'à ligne blanche). C'est fiable sur de la sortie d'agent, et ça ne
  dépend pas des détails internes de `QTextDocument`.
- **Puis un fragment par bloc** : `QTextDocumentFragment::fromMarkdown(bloc,
  MarkdownDialectGitHub).toHtml()` → HTML court, sans `<style>` de document. Les styles
  en ligne (gras, italique, lien, code) viennent du moteur de Qt, on ne les réécrit pas.
- Le modèle d'une bulle devient : `{ role, raw, messageId, blocks: [ {kind, level, mono,
  html, text} ] }`. `raw` reste la source (pour le streaming et pour copier/coller),
  `blocks` est le rendu.
- **Hors du thread UI** : `MdParser::parseAsync(id, markdown)` sur
  `QThreadPool::globalInstance()`, qui renvoie les blocs par un signal en connexion
  `QueuedConnection`. Le texte qui coule ne se rend **pas** bloc par bloc à chaque chunk :
  on parse après un timer court (120-160 ms sans nouveau chunk) et à `endTurn`, et on
  garde un cache par empreinte du texte (les bulles reprises/figées ne se re-parsent pas).
- Côté rendu : dans `card`, une `Column` de `Text` (un par bloc), `spacing` 5 px / 14 px
  selon le rôle (reprendre les chiffres de la v1), `font.family: win.uiFont`, monospace
  pour les blocs `mono` avec un fond `#18ffffff` et un rayon (le fond d'un bloc de code est
  un `Rectangle` derrière le `Text`, pas du CSS).

Ce qu'il faudra vérifier à l'œil (et pas dans le terminal) : le subset HTML de `Text` en
Qt Quick pour `<pre>` (monospace), les tableaux et les listes imbriquées. Si un bloc
tableau coinçait, on retombe sur la voie B dégradée : tableau rendu en lignes alignées
(monospace), pas en tableau HTML.

---

## 5. Permissions : le design à tenir

- **Une carte par demande**, posée au-dessus de la barre (ce qu'elle fait déjà), avec :
  titre de l'outil (`toolCall.title`), `kind` (execute/read/edit/other), l'entrée de
  l'outil tronçonnée (400 px déjà, à passer à 3 lignes avec « Déplier »), et la sortie si
  elle existe (`_meta.terminal_output.data`, tronçonnée).
- **Boutons groupés par `kind`** : les `allow_*` à gauche (le premier est le choix par
  défaut, Entrée), les `reject_*` à droite (Échap = `cancelled` si une option reject
  existe, sinon `cancelled`). Les libellés viennent du moteur, on ne les réécrit pas.
- **Ne jamais inventer la décision** : renvoyer l'`optionId` choisi (le pont recolle la
  décision autoritaire et ses règles persistantes).
- **File d'attente** : un moteur peut demander plusieurs autorisations d'affilée ; il faut
  une pile visible (compteur « demande 2/3 ») au lieu d'écraser la carte.
- `$/zcode/ask_settled` : retirer la carte **quel que soit le `kind`** si une autre main a
  tranché ; le POC ne gère que `"permission"` — à corriger.
- Une demande sans réponse doit rester possible : `session/cancel` sur le tour l'interrompt
  côté moteur (mesuré : un tour coupé décline les demandes restantes).

---

## 6. Questions à choix : la carte formulaire

- Annoncer `clientCapabilities: { elicitation: { form: {} } }` au `initialize`.
- Une carte par `elicitation/create` (mode `form`), avec une rangée par propriété du
  schéma, dans l'ordre du schéma :
  - `string` avec `oneOf`/`enum` → choix unique (une rangée d'options ; l'option dont le
    `const` est `__skip__` est rendue comme « passer ») ;
  - `array` avec `items.anyOf` → choix multiple (cases à cocher) ;
  - `string` sans choix → champ libre ; `boolean` → case ; `number`/`integer` → champ
    nombre ;
  - le compagnon `q_i_other` → champ libre sous le choix, avec son titre tel quel ;
  - `title` = libellé de la question, `description` = aide secondaire.
- Réponse : `accept` + `content` conforme au schéma (choix multiple = **tableau** de
  strings ; le free-text s'ajoute aux choix, pas à la place), `decline`/`cancel` si on ne
  répond pas.
- Clavier : Entrée = accepter (le choix par défaut s'il n'y a rien à cocher), Échap =
  `cancel`. Rien n'est obligatoire (`required: []`) : on doit pouvoir tout laisser vide.
- Le `toolCallId` est fourni : il permet de rattacher la carte à la bulle d'outil
  correspondante (le `tool_call` avec le même `toolCallId` arrive avant, et son
  `tool_call_update` donne la réponse finale).

---

## 7. Découpage proposé

| Tranche | Contenu | Critère de validation |
| --- | --- | --- |
| **M3a — Markdown** | parseur de blocs C++ + test `tests/test_md.cpp`, parsing hors thread, rendu par blocs dans la carte, air de la v1 | une réponse à plusieurs blocs (titre, liste, code, citation, tableau) tient la carte qui épouse son texte ; parsing invisible sur le thread UI (frame time non dégradé en `AICE_STRESS`) |
| **M3b — Permissions** | groupement par `kind`, défilement des demandes, sortie d'outil, `ask_settled` tous `kind` | une demande d'outil réelle (écriture) traitée depuis la fenêtre, au clavier et à la souris |
| **M3c — Questions** | capacité `elicitation.form`, carte formulaire, multi-select et free-text | `AskUserQuestion` posé par le moteur rendu en **une** carte, réponse acceptée, tool `completed` |
| **M3d — Autour** | carte d'outil (`tool_call`/`tool_call_update`), `plan`, `available_commands_update` (slash), réglage modèle/mode (`set_config_option`), `agent_thought_chunk` | choix du modèle depuis la fenêtre ; les commandes slash en aide dans la barre |

Ordre voulu : M3a d'abord (le rendu est ce qui te manque le plus à l'œil), M3b et M3c
ensuite (ils se ressemblent : une carte, des boutons, une réponse au pont).

## 8. Pièges relevés pendant la recherche (à ne pas refaire)

- **Ne pas annoncer `elicitation.form` et ne pas le rendre** : le pont basculerait sur les
  popups yes/no par option, et une question à choix multiple deviendrait une rafale de
  popups.
- **Traiter `kind` comme un enum ACP strict** : `allow_project`/`allow_session` existent
  chez ZCode ; un filtre `kind === "allow_once"` ferait disparaître des options.
- **`ask_settled` n'est pas que « permission »** : la branche actuelle du POC ne gère que
  ce `kind`-là.
- **Réécrire les libellés des options** ou inventer un `{decision:"allow"}` : le pont
  recolle la décision depuis ses propres options ; on ne renvoie que l'`optionId`.
- **Parser le markdown dans le thread UI** : c'est exactement ce qui a fait la v1 (mesure +
  rendu dans le thread de dessin). Le brief M3 le demande hors UI thread — sinon le lag
  revient dès qu'une réponse fait plusieurs blocs.
- **Relire une bulle figée** : sans cache par empreinte, un scroll dans une conversation
  de 2 000 bulles re-parserait 2 000 markdowns.
- Les sondes de recherche sont dans `/tmp/hub-probe*.mjs` ; elles mintent des sessions neuves
  sur l'instance visée et ne touchent pas les sessions live du téléphone (garde de
  `session/new`). Elles répondent `decline`/`cancelled` pour ne pas bloquer le moteur.
