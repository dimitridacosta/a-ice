# M1 — POC UI : le verre en Qt Quick

*Brief du 2026-10-31. Branche `v2` (créée depuis `voice`), binaire séparé `a-ice-m1`,
moteur agentique non branché. Le plan : [PLAN.md](PLAN.md), la techno : [ADR-0001-techno.md](ADR-0001-techno.md).*

## Ce que M1 devait prouver

Les trois ingrédients du plan, **sur l'a-ice qu'on connait déjà** — pas une maquette
générique : bulles flottantes séparées, chacune dans son propre morceau de verre,
et la barre de prompt qu'on attrape pour déplacer toute la fenêtre.

1. Fenêtre frameless 100 % translucide, floutée par KWin **carte par carte**.
2. Barre de prompt attrapable → `startSystemMove()`, position persistée.
3. Zone de bulles virtualisée, fade en haut et en bas.

Critère du plan : *stress test sans freeze, frame time mesuré et stable*.
Fenêtre carrée **800 × 800** pour tester le principe.

**Verdict : les trois passent.** Qt Quick tient la promesse ; on peut engager M2.

## Le code

```
poc/CMakeLists.txt      binaire a-ice-m1, QML embarqué dans la ressource
poc/main.cpp            QGuiApplication + QQmlComponent (les erreurs QML sont affichées)
poc/GlassHelper.h/.cpp  le verre : région de flou et de clic, mesure des frames
poc/qml/Main.qml        bulles, barre, fade, ombres, badge de mesures
```

À ajouter au build : `option(AICE_BUILD_POC ... ON)` dans le `CMakeLists.txt` racine —
le POC ne touche ni au moteur ni à l'UI v1, il se construit à côté.

### L'idée clé, reprise de la v1

La fenêtre n'existe pas : seul le verre existe. Le flou KWin **et** la région qui
capte les clics ne couvrent **que** les cartes dessinées, jamais le rectangle de la
fenêtre. En v1 on fabriquait ces régions à la main dans le `paintEvent` ; ici on va
les chercher dans l'arbre QML : tout item qui déclare

```qml
property bool glassCard: true
property int glassRadius: 16
```

entre dans la région. `GlassHelper::collect()` parcourt l'arbre, ramène chaque carte
au repère de la scène (en tenant compte des marges d'ancrage), **intersecte avec le
viewport** (`setClipItem(bubblesView)` : une bulle sortie de la zone de conversation
ne doit ni flouter ni capter les clics), construit des `QRegion` arrondis, et pose
`window->setMask()` + `KWindowEffects::enableBlurBehind()`. Rien n'est reposé si la
région est identique (anti-clignotement). Le rafraîchissement est fusionné
(`scheduleRefresh()`, `QTimer::singleShot(0)`), déclenché à chaque `frameSwapped` —
l'équivalent exact de ce qu'on faisait au repaint en v1.

## Lancer

```bash
distrobox enter dev-aice -- bash -c \
  "cd /var/home/dimitri/projects/ai/projects/a-ice/build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j\$(nproc)"

# Fenêtre visible (800 x 800), réponses simulées, badge de mesures
cd build/poc && ./a-ice-m1 --size 800
```

| Variable | Effet |
| --- | --- |
| `--size 800` | côté de la fenêtre carrée (défaut 800) |
| `AICE_STRESS=1` | 2 000 bulles factices au lieu de 9 |
| `AICE_PUMP=1` | redessine en continu : le frame time s'auto-mesure sans interaction |
| `AICE_HUD=1`/`0` | badge de mesures dans la fenêtre (visibles par défaut) |
| `AICE_LAYER=1` | overlay layer-shell (ancre désactivée, couche LayerOverlay) |
| `AICE_DEBUG=1` | dump de chaque carte de verre (rect + rayon) quand elles changent |
| `AICE_QML=<chemin>` | charge le QML depuis le disque : itérer sur l'UI sans recompiler |
| `QT_QPA_PLATFORM=offscreen` | vérification hors écran (aucune fenêtre ne s'affiche) |

Le badge affiche `frame … ms · pic … ms · N bulles · M cartes`, plus deux liens qui
servent à mesurer le coût GPU : **ombre** et **fade** (à couper/remettre à la volée),
et `fenetre natif` ou `fenetre par deltas` selon que le compositeur a bien voulu le
`startSystemMove()`.

## Mesures

Stress réel sur l'écran Wayland (KDE/Bazzite), 2 000 bulles, redraw continu, 800 × 800 :

```
frame time moyen 6,02 – 6,16 ms, pic 41 ms, 1 920 frames, cartes dans le verre 11
```

- **Pas de freeze**, pas de dérive : la moyenne reste à ~6 ms du début à la fin
  (1 920 frames), donc 16 ms/frame largement tenus — le quota du compositeur.
- **11 cartes de verre avec 2 000 bulles dans le modèle** : c'est la preuve de la
  virtualisation. Seules les bulles réellement réalisées (visibles ou dans le
  `cacheBuffer`) entrent dans la région ; le modèle, lui, fait 2 000 lignes.
- Le pic unique à 41 ms correspond à l'installation de la liste au démarrage
  (réalisation des delegates + première région), pas à une frame de scroll.
- Hors écran (`QT_QPA_PLATFORM=offscreen`) : mêmes ordres de grandeur (5,8–6,5 ms).
- Ombres `MultiEffect` et fade par carte coupés depuis le badge : le frame time
  baisse nettement, ce qui confirme que le coût est bien du côté GPU/compositeur
  et non de la logique.

**Empilement vérifié par le dump `AICE_DEBUG=1`** — identique à la v1 :

- cartes agent pleine largeur (`776 @ x=12`), carte user plus petite alignée à
  droite (`104 @ x=684`), y = 676, 621, 499, 400, 300, 223, 146, 92, 37, barre à
  730, espacement de 6 px. Les bulles sont bien **ancrées en bas**, les anciennes
  montent au-dessus.

## Ce qui est acquis

| Ingredient | Comment | Verdict |
| --- | --- | --- |
| Verre par carte | `glassCard` dans l'arbre QML → `QRegion` arrondies + `enableBlurBehind` + `setMask` | ✅ |
| Barre attrapable | `QWindow::startSystemMove()`, fallback deltas côté QML, position dans `QSettings` `[window]` | ✅ (le natif est-il réellement pris : à confirmer à l'œil) |
| Bulles virtualisées | `ListView` + `cacheBuffer`, 2 000 lignes, 11 régions | ✅ |
| Fade haut/bas | opacité par delegate, selon la position dans la zone | ✅ |
| Identité visuelle | rayons 16 / 22, teintes v1 (`#5A3478DA` user, `#64222228` agent), encre `#e9eaee` | ✅ |
| Frame time stable | 6 ms steady, 1 920 frames | ✅ |

## Ce qui reste à valider **à l'œil** (je ne peux pas le voir d'ici)

À faire dans l'ordre, ça prend deux minutes :

1. **Le flou a-t-il la tête du verre Plasma** (thème Utterly-Round), y compris les
   coins arrondis de la région — `enableBlurBehind` suit-il le `QRegion` arrondi ou
   l'englobe-t-il dans un rectangle ?
2. **Le drag natif vs fallback** : lire la ligne `fenetre natif / par deltas` du
   badge. Si « par deltas », `startSystemMove()` n'est pas accepté par KWin dans
   cette configuration et il faudra l'assumer (le drag devient moins fluide).
3. **`AICE_LAYER=1`** : overlay sans ancre, au-dessus de tout, sans tâche dans le
   panel. Le chemin layer-shell est écrit mais n'a pas été testé en conditions réelles.
4. **Le ressenti du fade** (90 px) et le scroll à 2 000 bulles, au toucher.
5. La persistance de position : relancer et vérifier que la fenêtre se remet où elle
   était (section `[window]` de `~/.config/a-ice/a-ice.conf`).

## Pieges (ne pas les refaire)

- **Qt 6.11** : `QQmlComponent::Status` vaut `{Null, Ready, Loading, Error}` ;
  `isCompiling()` / `isCreating()` n'existent plus.
- **`engine.load()` + `rootObjects().isEmpty()` sort silencieusement** (code 1,
  rien affiché). D'où `QQmlComponent` + `errorString()` imprimés par `fprintf` :
  `qInfo`/`console.log` sont niveau debug et **filtrés par défaut** en Qt 6.
- `qt_add_resources` n'a pas de mot-clé `ALIASES` ici, mais l'alias est déjà
  `PREFIX + chemin relatif` : la ressource est `qrc:/poc/qml/Main.qml`.
  `QFile::exists("qrc:/…")` est faux (il écrit `":/…"`).
- **Un `header: Item {}` dimensionné depuis `contentHeight` casse la virtualisation** :
  plus aucun delegate réalisé, et la région de verre ne compte plus qu'une carte.
  C'est ce qui a produit le bug initial. On ancre en bas autrement :
  `verticalLayoutDirection: ListView.BottomToTop` + modèle **du plus récent au plus
  vieux** (`insert(0, …)`). « Être à la fin » devient `contentY >= contentYMax - 4`
  (une vue inversée donne `contentY = contentYMax` quand la plus récente est visible).
- `width: view ? view.width : 0` dans un delegate rend une largeur 0 (et donc des
  largeurs de carte **négatives** dans le dump). Écrire `width: bubblesView.width`.
- `native` est un **identifiant réservé** en QML.
- Le `TextEdit` du noyau QtQuick n'a **ni `accepted`, ni `placeholderText`/`placeholderColor`** :
  `Keys.onReturnPressed` + un `Text` frère pour le texte fantôme.
- Une propriété ne peut pas avoir deux gestionnaires (`Property value set multiple times`).
- Les géométries transitoires pendant un layout peuvent remonter des positions
  négatives dans le dump des cartes : c'est sans effet sur le résultat.

## Ce que M2 garde de M1

À basculer tel quel dans la vraie GUI : `GlassHelper` (presque rien à changer, sinon
les cartes réelles), la liste ancrée en bas + modèle le plus récent en tête, le fade
par carte, le drag natif avec fallback. À ne **pas** garder : `seedFake()`/`replies`
(bouchons de démo), le badge de mesures (outil de mesure, pas une feature), et
`AICE_*` côté moteur. Le point d'entrée du vrai travail : brancher le client du hub
zcode-acp à la place de `send()`/`streamTimer`.
