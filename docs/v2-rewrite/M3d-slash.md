---
date: 2026-11-02
type: note de chantier
branch: v2
---

# M3d — les commandes slash de la barre de prompt

Recherche mesuree dans `~/.local/lib/node_modules/zcode-acp-server/dist/` (le pont
qui nous relie au hub), puis rendu cote corps. Le fil du jour :
[[a-ice-emissaire-elicitation-2026-11-02]] (vault `memory/`).

## 1. Ce que le protocole rend reellement

**La liste n'est pas inventee par le client.** Le pont la publie par
`session/update` → `update.sessionUpdate = "available_commands_update"`, avec
`availableCommands: [{name, description, input? {hint}}]`, peu apres la reponse
d'ouverture (notification differee : 50 ms puis 1 000 ms, plusieurs fois par
session). C'est la seule source de notre autocompletion.

**D'ou viennent les commandes** (`config/skill-discovery.js`,
`config/plugin-commands.js`, `utils.js : SLASH_COMMANDS`) :

| Source | Ou ca vit | Nom publie |
|---|---|---|
| Statiques du pont | `utils.js` | `auto`, `compact`, `goal`, `fork`, `mode`, `model`, `thought`, `quota`, `resume`, `mcp`, `init`, `workflow(s)` |
| Skills | `~/.zcode/skills/*/SKILL.md`, `~/.agents/skills/*/SKILL.md`, skills de plugin, `<cwd>/.agents/skills/*/SKILL.md` | `$nom` pour les editeurs a groupage, **`nom` nu pour un client sans `clientInfo`** |
| Commandes de plugin | cache de plugin (`~/.zcode/plugins/.../<version>/commands/*.md`), declarees par `plugins.enabledPlugins` | nom du fichier, sans `.md` |

Un `.md` de commande ou un `SKILL.md` n'est declare que s'il porte un
`description` dans son frontmatter ; `argument-hint` donne le `input.hint` (sans
lui la commande part sans attendre d'entree) ; `user-invocable: false` la sort de
la liste ; les descriptions sont coupees a 80 caracteres.

**Le prefixe `$` n'est qu'un artifice d'affichage** (`skillPrefixForClient`) :
les clients nommes (Zed) le recoivent, martty et les clients **sans** `clientInfo`
( notre corps, l'appli du telephone) recoivent le nom nu — sinon aucun skill ne
correspondrait a leur menu `/`. Le pont admet les deux spellings cote saisie.

**Ce que le pont intercepte, ce qui passe, ce qui est mort**
(`handlers/slash.js`, `handlers/session.js`) :

- intercepte (tour fabrique, repondu par un chunk de texte + `end_turn`) :
  `compact`, `goal`, `auto`, `fork`, `model`, `mode`, `thought`, `quota`, `mcp`, `workflows` ;
- `UNSUPPORTED_TUI_COMMANDS` = `plugins`, `login`, `logout`, **`new`**, `locale`,
  `expert`, `effort`, `help` → message d'erreur poli. **En mode app-server,
  `/new` n'existe pas : c'est au corps de la rendre.**
- pass-through : `skill`, `init`, les commandes de plugin, les skills — le texte
  part tel quel, le backend resout avant le modele ;
- nom inconnu : `neutralizeSlashText` est **identite** (0.16.9) — le texte original
  part au modele comme un prompt ordinaire, sans casser le tour. (L'ancienne
  croyance d'un dur-echec etait fausse ; l'injecteur de caractere invisible
  corrompait l'historique.)

**Les reglages de session** sont un canal a part, et c'est la voie propre :
`configOptions` (dans la reponse de `session/new` et `session/load`, puis par
`update.sessionUpdate = "config_option_update" a chaque changement, y compris en
cours de tour) porte `{id: "model" | "mode" | "thought", name, currentValue,
options: [{value, name}]}`. Le reglage se fait par
`session/set_config_option {sessionId, configId, value}` : le pont repond la liste
remise a jour et notifie tous les clients attaches a la session. Une valeur hors
des choix publies est refusee (`-32603 … unsupported config option or switch failed`).

## 2. Ce qui a ete rendu cote corps

**Collecte** (`poc/HubClient.cpp`) : `collectCommands()` et
`collectConfigOptions()` alimentent deux proprietes, `hub.commands` et
`hub.configOptions` (signaux `commandsChanged` / `configOptionsChanged`), a partir
de `available_commands_update`, `config_option_update`, et des reponses de
`session/new` / `session/load`. Les listes se vident au `disconnectHub()` : une
commande annoncee par un pont mort ne doit pas rester proposable.

**La barre** (`poc/qml/Main.qml`, section « commandes slash (M3d) ») :

- des la saisie d'un `/`, une carte de suggestions se pose au-dessus de la barre
  (jusqu'a 7 lignes, `commande` en gras a gauche, description et `hint` a droite,
  la ligne choisie surlignee) ;
- **Tab** ou **clic** complete la ligne surlignee, **Haut/Bas** la
  choisissent, **Echap** l'efface (un second Echap ferme la fenetre comme avant),
  **Entree** envoie ce qui est ecrit quand c'est deja la commande entiere, et
  complete sinon ;
- deux regimes : le nom de la commande, puis — quand le nom d'un reglage est ecrit
  suivi d'un blanc — la valeur de l'argument, parmi les choix publies (`/model `
  ouvre les sept modeles de l'instance, `/model g` les filtre) ;
- les reglages ne sont pas durcis : `model`, `mode`, `thought` ne proposent que ce
  que le moteur a publie, et `hub.setConfigOption()` refuse une valeur qu'il ne
  connait pas en l'affichant dans le statut (avec ses choix) ;
- la carte du hub montre la ligne `modele … · mode … · pensee …` (libelles publies,
  pas identifiants complets), et son bouton « conversation neuve » s'appelle
  desormais `conversation neuve (/new)`.

**Qui rend quoi** :

| Commande | Rendue par | Voie |
|---|---|---|
| `/new` | le corps | `win.newConversation()` → `hub.startFreshConversation()` (conversation vierge, session gardee oubliee) |
| `/model <choix>`, `/mode <choix>`, `/thought <choix>` | le corps | `session/set_config_option` — regle sans laisser de faux message dans le fil (la forme texte `/model X` est interceptee par le pont et rend un chunk dans la conversation, rejoue a la reprise) |
| `/model`, `/mode`, `/thought` sans argument | le corps | ne regle rien : ouvre la liste des valeurs publiees |
| tout le reste (`/compact`, `/goal`, `/auto`, `/fork`, `/quota`, `/mcp`, `/init`, `/resume`, les skills, les commandes de plugin) | le pont | le texte part tel quel ; un nom qu'il ne connait pas non plus part au modele comme texte ordinaire |

## 3. Defaut corrige : l'agent de voix balayait nos questions

Trouve chez **l'outil emissaire**, pas chez a-ice :
`~/.config/a-ice/.agents/tools/zcode-emissary.mjs` repondait
`{outcome: {outcome: 'cancelled'}}` a **toute** requete serveur→client qui n'est
pas `session/request_permission` — donc aussi a `elicitation/create`. Or le pont
race ses connexions (« first-response-wins ») : l'emissaire, connecte en
permanence, gagnait la course et annulait le questionnaire que nous montions en
carte. Corrige : il ne repond plus qu'a ce qu'il sait rendre (`elicitation/create`
journalise sans reponse, methode inconnue = erreur `-32601`), le relais des
permissions est laisse tel quel. Note d'incident complete dans le vault :
`memory/knowledge/incidents/a-ice-emissaire-elicitation-2026-11-02.md`, regle
 retenue : **ne jamais repondre a une requete qu'on ne rend pas**.

## 4. Mesures live (2026-11-02, instance 534584, workspace `~/.config/a-ice`)

- `available_commands_update` : **24 commandes**, dont 12 skills publies **sans**
  `$` (notre corps n'envoie pas de `clientInfo`) — `speckit-loom`, `zcode-emissary`,
  `cpp`, `linux`, `loom`… ;
- `configOptions` : `model` (7 choix, valeur courante `ollama_cloud\glm-5.3-flash`),
  `mode` (`plan/build/edit/guarded/yolo`, courant `guarded`), `thought`
  (`low/high/max`, courant `low`) ;
- `session/set_config_option model = local\ornith-35b` : reponse +
  `config_option_update` recus, valeur courante mise a jour ;
- `session/set_config_option model = n-importe-quoi` : refuse par le pont
  (`-32603 unsupported config option or switch failed`) — d'ou la validation en amont ;
- autocompletion, verifiee sans clavier par le harnais `AICE_SLASH` (une saisie
  simulee une fois la session ouverte, `console.log` des lignes obtenues) :
  `/co` → `compact`, `copilot-remote-agent` ; `/model ` → 7 modeles ; `/model g` →
  4 modeles filtres ; `/compact` et `/mode guarded` → « complet » (Entree envoie) ;
  `/zoz` → rien (le texte partira au moteur).

## 5. Déclarer nos propres commandes (le « speckit.specify » de Dimitri)

Ce que le pont admet, mesure ci-dessus : une commande se declare par un
**skill** (`.agents/<nom>/SKILL.md` ou `~/.agents/skills/<nom>/SKILL.md`, avec
`name`, `description`, `argument-hint`) ou par une **commande de plugin** (cache
de plugin + `enabledPlugins` dans la config CLI ZCode). `~/.config/a-ice/.agents/skills/`
est deja explore par le pont : nos skills y sont deja proposes telles quelles.

Le projet vise par la remarque de Dimitri (`if-bpf`) est
`~/projects/if-platform/apps/bpf` : il n'a **pas** de `.claude/commands`, mais
`.claude/skills/` et surtout `.specify/` (scripts, templates,
`memory/constitution.md`, `integrations/*.manifest.json`) et
`.specify/workflows/speckit/workflow.yml`, ou les etapes sont nommees
`speckit.specify`, `speckit.plan`, `speckit.tasks`, `speckit.implement` avec des
portes de revue (`type: gate`, `options: [approve, reject]`, `on_reject: abort`).
Autrement dit : le nom `speckit.*` vient de l'**integration** (le CLI/kit qui
declare ses commandes), pas de fichiers MD poses a la main dans le depot. Pour
amener un cycle speckit dans la barre d'a-ice, la voie la plus courte est donc un
skill par etape (declaration `description` + `argument-hint`), pas un repertoire
de prompts.

## 6. Reste et limites vues

- La liste de suggestions est plafonnee a 7 lignes ; pas de scroll, pas de
  description deployee. A revoir quand le moteur en publie beaucoup.
- La carte de suggestions n'a pas de clic « Tab » visible : on ne le decouvre que
  par la ligne d'aide en tete.
- Le harnais `AICE_SLASH` ne teste que la partie suggestions ; `slashDispatch()`
  n'est pas automatise (il faut taper, ou poser un tour de test).
- Le `hint` d'une commande n'est pas verifie cote corps : une saisie ne correspondant
  a aucun choix publie est refusee pour les reglages, libre pour les autres.

## 7. Pour reprendre le fil

- Commits : voir `git log` sur `v2` (M3d inclus).
- Tests : `tests/test_md` (44), `test_hardline`, `test_chatmessage`, `test_config`,
  `test_voicestream` — tous verts le 2026-11-02.
- Prochain morceau : M4 (polish du corps), et la question du partage de la main
  avec l'agent de voix sur une meme instance.
