# a-ice 2.0 — Plan de refonte

*Écrit le 2026-10-31 par ddBot avec Dimitri. Branche `v2` créée depuis `voice` (HEAD de la branche vocale) ; le travail UI/vocal continue séparément sur `voice`.*

*Le point complet de **tout ce que la branche contient** (les six étages M3 compris) : [ETAT.md](ETAT.md), fait le 2026-11-02.*

## Vision (Dimitri, 2026-10-31)
- Un corps plus agréable : fluidité niveau **Zed IDE**, glassmorphism **parfait** accéléré GPU sur KDE/Bazzite.
- Séparer complètement **GUI** et **moteur agentique** : la GUI n'est qu'un skin client du hub zcode-acp (déjà en place sur ddpc/if-pc-01) ; le moteur derrière est interchangeable (ZCode, Gemini, wrapper Strata…).
- Barre de prompt flottante : attrapable et déplaçable n'importe où (déplacement natif Wayland/KWin).
- Zone de conversation : plein écran vertical par défaut, paramétrable ; les bulles disparaissent en **fade-out** en haut et en bas de la zone.

## ADR-0001 — choix techno
Voir [ADR-0001-techno.md](ADR-0001-techno.md). En bref : C++17 + **Qt Quick 7** (rendu GPU via QRHI) + KF6 + LayerShellQt. Qt Widgets abandonné (cause des lags actuels : raster CPU + re-layout global). Plan B documenté : gpui/Rust.

## Architecture cible
- `a-ice-gui` — le corps : overlay Qt Quick, 100 % client du hub zcode-acp (API HTTP `instances`/`sessions`).
- Le hub porte les moteurs (ZCode aujourd'hui). L'GUI ne connaît qu'une chose : le protocole du hub.
- Streaming token par token, permissions affichées dans l'UI, multi-sessions possible.
- Mode direct ACP (JSON-RPC stdio) gardé en option pour un agent sans hub.

## Roadmap

### M1 — POC UI (valider les trois ingrédients) — **fait**

*Résultat : [M1-poc-ui.md](M1-poc-ui.md). Binaire `a-ice-m1` (dossier `poc/`),
fenêtre 800 × 800, ~6 ms/frame en stress à 2 000 bulles, 11 cartes de verre : les
trois ingrédients tiennent. Les proportions de la v0 y sont remises à l'identique
(cartes à 12 px des côtés, air de 12 px, barre de 46 px) et la barre porte une
poignée de redimensionnement à son bord droit. Reste à valider à l'œil (flou, drag
natif, prise de la poignée, layer-shell).*
1. Fenêtre frameless translucide : blur via `KWindowEffects::enableBlurBehind` (test aussi `ext_background_effect_v1`, Plasma 6.7+), `KeepAbove`, skipTaskbar, LayerShellQt.
2. Barre de prompt flottante : déplaçable via `startSystemMove()`, position persistée (`QSettings`).
3. Zone bulles : `ListView` virtualisé, 2 000 bulles factices, scroll fluide, fade-out (dégradé alpha) haut/bas, hauteur paramétrable (plein écran par défaut).
- **Critère** : stress test sans freeze ; frame time mesuré et stable.

### M2 — Client hub ACP — **fait**
- Connexion au hub (endpoint dans `~/.config/a-ice/config.json`), création de session, stream des chunks, affichage live, routage des `request_permission` vers des boutons dans l'UI.

*Résultat : [M2-client-hub.md](M2-client-hub.md). Le meme binaire `a-ice-m1` prompte
maintenant le hub (instance réutilisée ou incubée, session neuve, chunks qui coulent
dans la carte agent, permissions en boutons). Garde vérifiée : `session/new` qui rend
une session déjà live est refusée au profit d'une session neuve.*

### M3 — Intégration — **fait (reste le retour de l'œil)**
- Markdown parsé **hors UI thread** (QTextDocument), historique conversation, config/SOUL, récupération progressive des features de `main`.

*Résultat partiel (2026-11-01) : la gestion de session ACP est en place — session
gardée par workspace dans `QSettings`, `session/load` à chaque lancement (le pont
rejoue l'historique), session neuve mintée quand la gardée est périmée, et fin de
tour close par le `turnState` du pont. Voir la dernière section de
[M2-client-hub.md](M2-client-hub.md).

*Suite (2026-11-01, émissaire) : les trois pièces restantes sont posées.*
1. **Markdown hors UI thread** — `poc/Markdown.cpp` : les bulles agent fermées
   partent au parseur (`QTextDocument::setMarkdown` → `toHtml` dans le pool) ;
   le streaming reste en PlainText, la bulle bascule en RichText quand son html
   revient par `messageId`. La longueur naturelle se mesure en riche aussi.
2. **Multi-sessions** — `/api/instances` est déroulé dans `hub.sessions`
   (sessionId, title, status, workspace) et la carte du hub liste une ligne
   cliquable par session : même instance → `session/load` en place ; autre
   workspace → nouvelle jonction (le pont est par instance), historique rejoué.
   Le modèle de bulles est vidé par `historyReset` avant le replay.
3. **Source unique du hub** — le bloc `hub` de `~/.config/a-ice/config.json`
   (hubUrl, token, workspace) est la seule réglée : la GUI migre le `remote` de
   zcode-acp une fois pour toutes à son premier lancement (bloc écrit, jamais
   réécrit), et le tool zcode-emissary le lit en premier (zcode-acp en repli).

Reste en M3 : le retour de l'œil — un tour réel (end_turn), les clics de la
carte-liste et le changement d'instance. Revue faite par l'émissaire
(2026-11-01, commits 1c4769c et 15e8ee1) : les trois pieces relues ligne à
ligne, deux vrais bugs corrigés au passage. Piece 1 — le gestionnaire
`onParsed` du parseur était branché sur les `Connections` du hub, qui ne porte
pas ce signal : le html des bulles fermées ne serait jamais revenu
(`Connections { target: markdown }` dedicace). Piece 2 — `disconnectHub` ne
vidait pas la liste des sessions : apres coupure, la carte gardait des lignes
cliquables vers un pont mort (la liste se vide a la coupure, se retrace a
openSessionReady). Piece 3 relue contre le tool de reference
zcode-emissary.mjs : memes cles (`hubUrl`, `token`), migration ecrite une
seule fois, repli zcode-acp correct — rien a changer.

M3 fait (2026-11-01, les trois pieces portees) : la note
[M3-md-permissions-questions.md](M3-md-permissions-questions.md) dit ce que le
protocole rend, et le corps le rend.

1. **Markdown par blocs (M3a)** — `poc/Markdown.cpp` : decoupage a la source
   (fence, titre, liste, cases, citation, tableau, filet, paragraphe), un
   fragment HTML par bloc, parsing dans le QThreadPool, cache par empreinte
   (une bulle figee ne se re-parse pas). La carte rend une `Column` de blocs
   avec l'air de la v1 (5 px autour d'un paragraphe et d'une liste, 14/7 autour
   d'un titre, 0 au premier et au dernier bloc), le fond des blocs de code en
   `Rectangle`, la barre de citation et le filet dessines, le code en ligne
   replace en span (Qt le rendait en texte nu). Mesure live : une reponse a
   8 blocs rendue, carte de 251 px dans une fenetre de 460 — elle epouse son
   texte. Decoupage verifie par `tests/test_md` (44 verifications).
2. **Permissions (M3b)** — une file de demandes (la carte porte « demande 1 sur N »),
   les autorisations a gauche (Entrée prend la premiere), les refus a droite
   (Échap prend l'option de refus du moteur, jamais un `cancelled` invente),
   l'entree de l'outil en monospace coupee a trois lignes avec « deployer », la
   sortie de terminal quand le moteur la joint, et `$/zcode/ask_settled` qui
   retire la carte quel que soit le `kind` (permission comme elicitation).
   Mesure live : une vraie demande d'execution (`rm -rf`) montee en carte de 444 x 104.
3. **Questions a choix (M3c)** — capacite `elicitation.form` annoncee a
   `initialize`, carte formulaire par question : choix unique, choix multiple
   (reponse en tableau de chaines), booleen, champ nombre, champ libre
   compagnon `q_i_other` (il ecrase le choix simple, s'ajoute au choix multiple),
   et le sentinel `__skip__` rendu « passer ». Reponse `accept` + contenu
   conforme, `cancel` a l'Échap. Mesure live : `AskUserQuestion` du moteur rendu
   en une seule carte (q_0 single).
4. **Commandes slash (M3d)** — la liste vient du moteur (`available_commands_update`),
   les valeurs des reglages de `configOptions` : carte de suggestions au-dessus de la
   barre (Tab ou clic complete, Haut/Bas choisit, Entrée envoie quand la commande est
   ecrite en entier, Échap efface la carte), `/new` rendu par le corps (le pont declare
   cette commande hors service en mode app-server), `/model`, `/mode` et `/thought`
   regles par `session/set_config_option` — pas par leur forme texte, qui fabrique un
   faux message dans le fil — avec validation stricte contre les choix publies, et la
   carte du hub qui affiche `modele · mode · pensee`. Mesure live : 24 commandes
   publiees (dont 12 skills sans prefixe `$`), 7 modeles proposes, un reglage obtenu, un
   reglage invente refuse. Note : [M3d-slash.md](M3d-slash.md).
5. **Voix streaming (M3e)** — `src/voice/VoiceStream` de la v1 **portee telle quelle**
   (chunkur adaptatif, filtre prononçable, memes constantes, meme cle `voice/enabled`),
   branchee sur les chunks du hub par un pont de 80 lignes (`poc/VoiceControl`) : seuil
   « le tour que nous avons lance » (pas d'historique rejoue, pas les tours des autres
   clients), `/voice on|off` (bascule si rien n'est ecrit) qui annule le tour si on coupe
   en pleine generation. Note : [M3e-voix.md](M3e-voix.md).
6. **Modes de fenetre (M3f)** — trois modes reglables : `fenetre` (ordinaire, ce que la
   v2 faisait jusqu'ici), `au-dessus` (gardee devant), `fixe` (au-dessus de tout et posee
   sur tous les bureaux, le geste de la v1) — les deux derniers poses par LayerShellQt,
   sans ancre pour garder le deplacement. `/fenetre <mode>` dans la barre, clic dans la
   carte du hub, persiste dans `window/mode`. Note : [M3f-fenetres.md](M3f-fenetres.md).

Vu du terminal (mesure, pas un oeil) : la fenetre ne swap pas de frame depuis
l'agent, donc les cartes se dessinent mais ne se regardent pas. Ce qui suit
attend le retour de Dimitri : le rendu a l'œil du markdown (tableaux, listes
imbriquees), les clics et le clavier sur les cartes de permission et de
question, et les suggestions slash a l'œil.

La course des demandes est reglee (2026-11-02) : le pont diffuse la demande a toutes
les connexions de l'instance et garde la premiere reponse, et l'agent de voix
(`zcode-emissary.mjs`) repondait `cancelled` a tout ce qui n'etait pas une permission —
il balayait donc nos questionnaires. Il ne repond plus qu'a ce qu'il sait rendre (note
`memory/knowledge/incidents/a-ice-emissaire-elicitation-2026-11-02.md` dans le vault,
et section 3 de [M3d-slash.md](M3d-slash.md)). Reste a le verifier a l'œil : relancer un
`AskUserQuestion` pendant que l'agent de voix est branche.

Reste pour M4 : polish du corps — ombres, dégradés, animations d'entrée, thème
complet, doc utilisateur ; rejuger les acquis d'origin/v2 (layer-shell par
défaut, shader de fade, fusion de v2).

### M4 — Polish
- Ombres, dégradés, animations d'entrée, thème complet, doc utilisateur.

## Règles de travail (émissaire et ddBot)
- **Commits à chaque étape qui compile**, style : `v2 : <fait>` (français, sans point final).
- **Build** (jamais sur l'hôte Bazzite, atomic read-only) :
  `distrobox enter dev-aice -- bash -c "cd /var/home/dimitri/projects/ai/projects/a-ice/build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)"`
- Dependencies manquantes : dnf install **dans la box dev-aice** ; si le process « crashe » en sortie alors que l'install a réussi, vérifier avec `rpm -q` plutôt que relancer.
- Ne pas toucher aux branches `main` et `voice`. Pas de force push.
- Référence API hub : `~/.config/a-ice/.agents/tools/zcode-emissary.mjs` (+ skill émissaire).
