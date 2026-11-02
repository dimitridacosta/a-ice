---
date: 2026-11-02
type: note de chantier
branch: v2
---

# a-ice 2.0 — tout ce que la branche `v2` contient

*Point complet fait le 2026-11-02, commit `c16a874`. Le plan vivait dans
[PLAN.md](PLAN.md) : ce document est l'état, l'autre est la feuille de route.*

**En une phrase :** un **nouveau corps** pour a-ice — une fenêtre de verre
(Qt Quick, GPU, blur du composeur) qui ne connait qu'un moteur par son hub — plus
**la voix streaming de la v1 portée telle quelle**, ses 854 lignes de C++ intactes.
5 456 lignes de C++/QML dans `poc/`, 5 harnais de test verts.

---

## 1. La lignée

La branche `v2` est née de `voice` (le HEAD de la branche vocale de la v1),
elle porte donc l'intégralité de l'héritage voix + la quarantaine de commits de
refonte. Deux pistes Qt Quick ont coexisté au début ; la ligne locale est celle
qui fait aujourd'hui foi :

| Étape | Commit | Ce qui est posé |
| --- | --- | --- |
| Plan + vision | `53ed238`, `6b3d0c7` | vision, architecture cible, `docs/v2-rewrite/` |
| POC M1 | `e7392e5` | verre Qt Quick, bulles flottantes séparées, barre grab |
| série ombres/UI | `0a49d9c` → `b658e60` | ombre noire diffuse, police Ubuntu, proportions v0, poignée de taille |
| M2 client hub | `531b5da`, `dd67851` | jonction au hub zcode-acp, session, chunks, permissions |
| gestion de session | `4af3bae`, `1e0e763`, `f03243a` | session persistée, multi-sessions, source unique du hub |
| M3a markdown | `c56845f` | markdown rendu bloc par bloc, hors UI thread |
| M3b + M3c | `18d8951` | permissions en file, questionnaires elicitation |
| M3d slash | `3706ab3` | suggestions branchées sur le moteur, réglages rendus par le corps |
| M3e voix | `ed50738` | le module v1 repris tel quel, branché sur les chunks du hub |
| M3f fenêtres | `c16a874` | les trois modes de fenêtre |

Les acquis de la première piste (d'`origin/v2` : shader de fade en `.qsb`,
layer-shell par défaut, stress `FrameAnimation`) sont **rejugés en M4**, pas
perdus — [PLAN.md](PLAN.md).

## 2. L'architecture en un dessin

```
┌──────────────────────────── Zed / téléphone / voix ────┐
│             autres clients de la même session          │
└────────────────────────────┬───────────────────────────┘
                             │ hub zcode-acp (HTTP + WebSocket)
┌────────────────────────────▼───────────────────────────┐
│  hub : instances · pont ACP · sessions · permissions   │
└───────┬─────────────────────────────────────┬──────────┘
        │ protocole ACP (JSON-RPC)            │ moteur au choix
┌───────▼────────────────────┐    ┌───────────▼──────────┐
│  CORPS v2 — poc/a-ice-m1   │    │  ZCode (interchangeable)
│  Main.qml + glass · hub ·  │    └──────────────────────┘
│  markdown · voice          │
└───────┬───────────┬────────┘
        │           └→ src/voice/VoiceStream (v1, portée telle quelle)
        └→ serveur ddvoice (faster-qwen3-tts, socket unix)
```

Le corps ne connait qu'un canal : **son client hub** (`HubClient`). Le moteur
derrière est interchangeable — c'est le contrat de la refonte.

## 3. Le binaire : `poc/a-ice-m1`

Un seul dossier `poc/`, un seul binaire `build/poc/a-ice-m1`, 5 456 lignes.

```bash
build/poc/a-ice-m1                                # 800 × 800, verre + bulles + barre
build/poc/a-ice-m1 --size 460x800                 # le corps étroit des vrais tours
build/poc/a-ice-m1 --size 900x640                 # ce que donne le redimensionnement
build/poc/a-ice-m1 --size 460x800 --prompt "..."  # envoie le tour à la session ouverte
build/poc/a-ice-m1 --workspace ~/.config/a-ice    # quel cerveau du hub faire parler
```

## 4. Le corps : ce que la fenêtre sait faire (M1)

| Ingrédient | Ce qu'il fait | Où |
| --- | --- | --- |
| **Verre par carte** | le flou du composeur et la région qui capte les clics couvrent *exactement* les cartes dessinées, et rien d'autre : tout item qui déclare `glassCard: true` entre dans la région, une bulle sortie du viewport est retranchée | `GlassHelper` |
| **Bulles flottantes séparées** | `ListView` virtualisé, ancrées en bas, les plus récentes en tête : 2 000 bulles dans le modèle, **10 cartes dans le verre** | `Main.qml` |
| **Fade haut/bas** | une carte sortie de la zone s'estompe par opacité (1 → 0,25 → écartée du verre), sur 90 px | `fadeBand` |
| **Barre grab** | attraper la barre de prompt déplace **toute la fenêtre** : `startSystemMove()` natif, fallback par deltas, position persistée | `glass.startMove()` |
| **Redimensionnement** | au clic sur le bord droit de la barre : `startSystemResize` (`RightEdge | BottomEdge`), poignée invisible (hover = curseur de redimensionnement seulement) | `glass.startResize()` |
| **Ombre** | `MultiEffect` posé sur la **matière seule** (le texte n'hérite de rien) : noire, diffuse, halo ~42 px (`blurMax 50`, softness 0.85), intensité 0,6, dévers 5 px (6 pour la barre) | `CardShadow` |
| **Identité visuelle** | rayons 16 / 22, teintes de la v1 (`#5A3478DA` user, `#64222228` agent), encre `#e9eaee`, police **Ubuntu** (+ Ubuntu Mono pour le code), cartes qui épousent leur texte, padding serré 12/8 | `Main.qml` |
| **Badge de mesures** | frame time, pic, frames, cartes dans le verre, ombre on/off — outil, pas une feature | `AICE_HUD` |

**Mesures** (2 000 bulles, redraw continu, 800 × 800) : **6,10 ms/frame sans
ombre, 6,69 ms avec** — l'ombre coûte ~0,6 ms (~3,5 % du quota), pas de
freeze, pas de dérive. Le critère de M1 est tenu.

## 5. Le branchement : le corps est un client du hub (M2)

| Ce qui est fait | Détail |
| --- | --- |
| **Jonction** | endpoint + token + workspace dans le bloc `hub` de `~/.config/a-ice/config.json` ; instance réutilisée si vivante, incubée sinon |
| **Session persistée** | la session vécue vit dans `[hub] session-<instance>` de `a-ice.conf` : au lancement le corps reprend (`session/load`, l'historique rejoué), session périmée remplacée par une neuve |
| **Streaming** | chunks → bulle agent grandissante, PlainText pendant le tour, **bascule RichText** à la fermeture (M3a) |
| **Garde de fin de tour** | la fin vient du `turnState` du pont (`end_turn` / cancellé / erreur), pas d'une heuristique |
| **Multi-sessions** | `/api/instances` déroulé dans `hub.sessions` : une ligne cliquable par session, même instance → chargement en place, autre workspace → jonction neuve et historique rejoué |
| **Source unique** | le bloc `hub` de `config.json` (hubUrl, token, workspace) est la seule source réglée : la GUI migre le remote de zcode-acp une seule fois, jamais réécrite |
| **Permissions** | file de demandes, carte « demande 1 sur N », autorisations à gauche (Entrée), refus à droite (Échap prend l'option de refus du moteur, jamais un `cancelled` inventé) |
| **Questions à choix** | capacité `elicitation.form` annoncée : choix simple, choix multiple, booléen, nombre, champ libre compagnon, « passer » |

**Mesure** : jonction complète sur le hub réel (`instance 815997 · session…`,
tour `end_turn`), garde anti-détournement vérifiée, un vrai tour rendu à
l'écran et capturé (`AICE_SHOT_MS=12000`).

## 6. L'intégration (M3) — six étages, tous faits

| Étage | Ce qu'il apporte | Mesure |
| --- | --- | --- |
| **M3a — markdown par blocs** | découpage à la source (fence, titre, liste, cases, citation, tableau, filet, paragraphe), un fragment HTML par bloc, parsing dans le `QThreadPool`, cache par empreinte ; l'air de la v1 (5 px par paragraphe, 14/7 pour un titre, 0 en premier/dernier) | 44 vérifications (`test_md`), carte de 251 px qui épouse son texte |
| **M3b — permissions** | file, clavier, monospace coupé à trois lignes avec « déployer », sortie de terminal jointe, `ask_settled` qui retire la carte quelle que soit sa cause | vraie demande `rm -rf` en carte 444 × 104 |
| **M3c — questions** | formulaire par question (`q_i`), `q_i_other` compagnon, `__skip__` rendu « passer », Échap = cancel | `AskUserQuestion` rendu en une carte |
| **M3d — slash** | liste **publiée par le moteur** (`available_commands_update`, 24 commandes dont 12 skills nus), valeurs de `configOptions` (7 modèles), carte de suggestions (Tab ou clic complete, Haut/Bas choisit, Entrée envoie, Échap efface) ; `/new` rendu par le corps, `/model` `/mode` `/thought` par `session/set_config_option` (jamais par la forme texte, qui fabriquait un faux message) | 24 commandes, un réglage obtenu, un réglage inventé refusé |
| **M3e — voix** | le module v1 **porté tel quel** (chunker adaptatif, filtre prononçable, verrou anti-avalanche, même clé `voice/enabled`), branché sur les chunks du hub par un pont (`VoiceControl`) avec le seuil **« le tour que nous avons lancé »** | tour de 350 mots → 12 chunks vocalisés, `/voice off` → `STOP` + annulation du tour |
| **M3f — modes de fenêtre** | `fenetre` (ordinaire), `au-dessus` (gardée devant), `fixe` (au-dessus de tout et posée sur tous les bureaux, le geste de la v1) ; `/fenetre <mode>`, clic dans la carte du hub, persiste dans `window/mode` | change de rang de couche sans relancer, fenêtre → couche demande une relance |

Notes détaillées : [M3-md-permissions-questions.md](M3-md-permissions-questions.md),
[M3d-slash.md](M3d-slash.md), [M3e-voix.md](M3e-voix.md),
[M3f-fenetres.md](M3f-fenetres.md).

## 7. La voix : ce qui est arrivé de la v1

`src/voice/VoiceStream` est entré dans le POC **sans être retapé** — c'est le point
que le contrat demandait. Il continue d'apporter :

- protocole serveur `[$XDG_RUNTIME_DIR | /tmp]/ddvoice.sock` (1 ligne = 1 requête, le
  serveur clôt après chaque réponse, d'où `connexionFraiche()`) ;
- découpe adaptatif : 18,5 car/s, 24 → 500 car, flush 120 ms, verrou 1 spawn / 30 s,
  spawn auto du serveur ;
- filtre « ce qui peut être dit » : markdown, emoji, pictogrammes, tirets longs, et
  un état `Filtre` qui survit aux découpages de chunks ;
- réglages : `/voice on | off | bascule` (bascule comme la v1), la **même clé**
  `voice/enabled` — monter la voix côté v1 la monte côté v2 ;
- couper **pendant** un tour annule le tour ; fin d'un tour cancellé = `stop()`, sinon
  `finish()` (le dernier morceau part malgré tout).

Le **seul** point où la v2 diffère : le seuil. `VoiceControl` ne vocalise que le tour
qu'on a lancé (`hub.isGenerating() && role == "agent"`) — l'historique rejoué par
`session/load` et les tours des autres clients (téléphone, émissaire) passent silencieux.
Mesuré : 59 chunks / 11 027 car rejoués → **1 seul** chunk vocalisé.

## 8. Modes de fenêtre, réglages et persistance

| Mode | Effet | Wayland | X11 |
| --- | --- | --- | --- |
| `fenetre` | ordinaire, peut être doublée | rien de spécial | `SkipTaskbar` |
| `au-dessus` | gardée devant les autres applis | couche `LayerTop` | `KeepAbove` + `SkipTaskbar` |
| `fixe` | au-dessus de tout **et sur tous les bureaux** (le geste de la v1) | couche `LayerOverlay` | `KeepAbove` + `Sticky` |

En Wayland, les deux modes non ordinaires passent par **LayerShellQt sans aucune
ancre** (le geste de la v1 — ancres à droite — n'est pas repris, pour que le
déplacement garde du sens) ; en X11 ils ne sont pas des couches du composeur mais
des états de fenêtre. **Ce qui reste à trancher à l'œil** : est-ce que le mode
`fixe` suit vraiment à travers les bureaux ? (une couche ne dépend pas d'un bureau
virtuel, mais c'est le composeur qui décide).

Ce que le corps persiste — tout dans `~/.config/a-ice/` :

| Fichier | Clés | Porté par |
| --- | --- | --- |
| `a-ice.conf` `[window]` | `x`, `y`, **`mode`** (fenetre / au-dessus / fixe) | position + modes de fenêtre |
| `a-ice.conf` `[hub]` | `session-<instance>`, `workspace` | session vécue + cerveau du hub |
| `a-ice.conf` `[voice]` | `enabled` | la voix (même clé que la v1) |
| `config.json` bloc `hub` | `hubUrl`, `token`, `workspace` | la seule source du branchement (jamais réécrite après migration) |

## 9. Les harnais de lancement (options et knobs AICE_*)

| Option / knob | Ce qu'il fait |
| --- | --- |
| `--size N [xN]` | la taille de la fenêtre : un côté ou largeur × hauteur |
| `--prompt <texte>` | envoie le tour dès la session ouverte (test end-to-end) |
| `--workspace <chemin>` | désigne le cerveau du hub à faire parler (pose le choix) |
| `AICE_STRESS=1` | 2 000 bulles factices, la mesure de M1 |
| `AICE_PUMP=1` | redraw continu, frame time sans interaction |
| `AICE_SHADOW=0` / `AICE_VOICE=0` / `AICE_LAYER=1` | mesure sans ombre / sans voix ; `AICE_LAYER`, l'ancien interrupteur couche du POC, vaut maintenant `fixe` |
| `AICE_HUD=0` | cache le badge de mesures |
| `AICE_QML=/chemin/Main.qml` | charge le QML du disque : itérer sur l'UI sans recompiler |
| `AICE_SLASH="/mo,/model ,@/voice off"` | saisie slash sans clavier ; `@` **dispatche** (teste `/voice` en plein tour) |
| `AICE_WINDOW_MODE=fenetre|au-dessus|fixe` | force le mode de fenêtre sans le persister |
| `AICE_SHOT=/chemin.png` (+ `AICE_SHOT_MS`, `AICE_SHOT_QUIT`) | capture hors écran : voir le rendu sans les yeux |
| `AICE_TURN_QUIT=1` | ferme l'appli à la fin du tour (test automatisé) |
| `AICE_DEBUG` | log de tous les changements d'état du hub |

## 10. Les tests

| Harnais | Ce qu'il garde | État |
| --- | --- | --- |
| `tests/test_hardline` | TerminalSafety : bloc-liste, dangereux (héritage v1) | 10 vérifications |
| `tests/test_chatmessage` | ChatMessage : réparations, sérialisation (héritage v1) | 17 vérifications |
| `tests/test_config` | config : switch, défauts, api_key par provider (héritage v1) | 5 vérifications |
| `tests/test_voicestream` | le module voix **hérité tel quel** : chunker, filtre, garde anti-avalanche | **25 vérifications, aucune retouchée** |
| `poc/test_md` | le découpage markdown par blocs (M3a) | 44 vérifications |

Le module voix porte le contrat : **25 vérifications passent sans qu'une seule
assertion ait dû être retouchée** — c'est la preuve mécanique du portage tel quel.

## 11. Fichiers (qui fait quoi)

| Fichier | Lignes | Rôle |
| --- | --- | --- |
| `poc/qml/Main.qml` | 2 144 | le corps tout entier : bulles, barre, cartes, permission, question, suggestions, carte du hub |
| `poc/HubClient.{h,cpp}` | 1 661 + 256 | le client hub : jonction, session, chunks, permissions, elicitation, sessions, commandes slash |
| `poc/Markdown.{h,cpp}` | 328 + 61 | découpage markdown par blocs, parsing hors UI, cache |
| `poc/GlassHelper.{h,cpp}` | 502 + 151 | le verre : cartographie des cartes, régions, mesure de frames, move/resize, modes de fenêtre |
| `poc/VoiceControl.{h,cpp}` | 116 + 53 | le pont QML ↔ `VoiceStream` (80 l.), la persistance de la voix |
| `poc/main.cpp` | 184 | collage des cinq objets + harnais de test |
| `src/voice/VoiceStream.{h,cpp}` | 854 | **la voix de la v1, portée telle quelle** |

Les 5 456 lignes de `poc/` sont neuves ; la seule chose reprise à l'identique de la
v1 est le module voix (et ses tests).

## 12. Ce qui reste

- **M4 — polish** : dégradés, animations d'entrée, thème complet, doc utilisateur ;
  rejuger les acquis d'`origin/v2` (shader de fade en `.qsb`, layer-shell par défaut).
- **Le retour de l'œil** (attente de Dimitri) : le rendu du markdown (tableaux,
  listes imbriquées), les clics sur les cartes de permission et de question, les
  suggestions slash, le grab et le resize, les trois modes de fenêtre (surtout `fixe`
  à travers les bureaux), le rythme réel de la voix.
- **Émissaire en cours de tour** : confirmer à l'œil qu'un `AskUserQuestion` est
  bien rendu pendant que l'agent de voix est branché (la course des demandes est
  réglée côté outil, pas encore à l'œil).
- **Le passage de relais** : décider quand le corps v2 remplace celui de la v1 dans
  `main` — la voix et les réglages sont déjà portés. `origin/v2` a **6 commits de
  retard** sur la branche ; à pousser quand Dimitri le veut.

---

*Règles tenues : build uniquement en distrobox `dev-aice`, commits `v2 : <fait>` à
chaque étape qui compile, branches `main` et `voice` jamais touchées. Les mesures
détaillées de chaque étage sont dans les notes listées plus haut.*