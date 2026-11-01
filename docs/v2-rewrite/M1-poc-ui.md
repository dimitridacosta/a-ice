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

**Verdict : les trois passent** (plus l'ombre et le fade, voir plus bas). Qt Quick
tient la promesse ; on peut engager M2.

## Le code

```
poc/CMakeLists.txt      binaire a-ice-m1, QML embarqué dans la ressource
poc/main.cpp            QGuiApplication + QQmlComponent (les erreurs QML sont affichées)
poc/GlassHelper.h/.cpp  le verre : région de flou et de clic, mesure des frames, captures
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
viewport** (`setClipItem(bubblesView)` : une bulle sortie de la zone ne doit ni
flouter ni capter les clics), écarte les cartes que le fade a éteintes (alpha
effectif mesuré en remontant les parents — rien de peint, donc rien à flouter),
construit des `QRegion` arrondies, puis pose `window->setMask()` +
`KWindowEffects::enableBlurBehind()`. Rien n'est reposé si la région est identique
(anti-clignotement). Le rafraîchissement est fusionné (`scheduleRefresh()`,
`QTimer::singleShot(0)`), déclenché à chaque `frameSwapped` — l'équivalent exact de
ce qu'on faisait au repaint en v1.

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
| `AICE_HUD=1/0` | badge de mesures dans la fenêtre (visible par défaut) |
| `AICE_SHADOW=1/0` | ombres de carte activées ou coupées au démarrage — sert à mesurer ce qu'elles coûtent |
| `AICE_LAYER=1` | overlay layer-shell (ancre désactivée, couche LayerOverlay) |
| `AICE_DEBUG=1` | dump de chaque carte de verre (rect, rayon, **alpha effectif**), et compte des cartes écartées par le fade |
| `AICE_SHOT=<chemin.png>` | capture la scène rendue et l'écrit dans le fichier ; `AICE_SHOT_QUIT=1` ferme juste après. **C'est l'outil qui a servi à voir les deux bugs** décrits plus bas |
| `AICE_QML=<chemin>` | charge le QML depuis le disque : itérer sur l'UI sans recompiler |
| `QT_QPA_PLATFORM=offscreen` | vérification hors écran (voir la mise en garde sur les effets GPU) |

Le badge affiche `frame … ms · pic … ms · N bulles · M cartes`, plus deux liens qui
mesurent le coût GPU à la volée (**ombre**, **fade**) et `fenetre natif` ou
`fenetre par deltas` selon que le compositeur a bien voulu le `startSystemMove()`.

### Voir le rendu sans les yeux sur l'écran

`AICE_SHOT=/tmp/x.png` capture la scène (via `QQuickWindow::grabWindow()`), la pose
sur un fond de contrôle en damier et l'écrit en PNG. Avec `AICE_DEBUG=1`, les cartes
de verre sont en plus entourées sur l'image : on voit où est le verre, et ce qui est
peint réellement. C'est comme ça que l'ombre et le fade ont été validés — la
première version était fausse et **on ne le voyait pas**.

Les captures servent à vérifier, elles ne sont **pas versionnées** (elles sont
jetées avant chaque commit). Le dossier de travail est `poc/shots/`, libre de
suppression.

Mise en garde : hors écran, le container n'a pas de GPU, donc le rendu tombe sur le
RHI logiciel et **les effets (`layer.effect`, `MultiEffect`) ne peignent rien**. Une
capture `QT_QPA_PLATFORM=offscreen` ne prouve donc jamais un ombre/un flou — pour
ça il faut une capture sur l'écran (le POC tourne bien sur le vrai Wayland).

## Mesures

Stress réel sur l'écran Wayland (KDE/Bazzite), 2 000 bulles, redraw continu, 800 × 800 :

| config | frame time | pic | frames | cartes |
| --- | --- | --- | --- |
| sans ombre (`AICE_SHADOW=0`) | **6,10 ms** | 24 ms | 3 600 | 10 |
| avec ombre bleue (défaut) | **6,69 ms** | 31 ms | 3 840 | 10 |

- **Pas de freeze**, pas de dérive : la moyenne ne bouge pas du début à la fin, donc
  le quota du compositeur (16,7 ms) est tenu avec ~10 ms de marge — et l'ombre coûte
  **~0,6 ms/frame**, soit ~3,5 % du quota (mesurée texte hors du layer, voir plus
  bas). C'est le prix à connaitre pour la suite.
- **10 cartes de verre pour 2 000 bulles dans le modèle** : preuve de la
  virtualisation. Seules les bulles réalisées entrent dans la région (le modèle, lui,
  fait 2 000 lignes). Le pic unique de 30-35 ms correspond à l'installation de la
  liste (réalisation des délégués + première région), pas au scroll.
- Le dump `AICE_DEBUG=1` donne l'alpha effectif de chaque carte : à 2 000 bulles, la
  carte coupée en haut de la zone est à `alpha 0,25` et les cartes entièrement sorties
  sont écartées du verre.
- Empilement vérifié à la mesure — identique à la v1 : cartes agent pleine largeur
  (`760 @ x=20`), carte user plus petite alignée à droite (`104 @ x=676`), barre à
  `760x52 @ 20,730`, y des bulles 676, 621, 499, 400, 300, 223, 146, 92, 37,
  espacement de 6 px. Les bulles sont bien **ancrées en bas**, les anciennes montent.

## Ce qui est acquis

| Ingrédient | Comment | Verdict |
| --- | --- | --- |
| Verre par carte | `glassCard` dans l'arbre QML → `QRegion` arrondies + `enableBlurBehind` + `setMask` | ✅ |
| Barre attrapable | `QWindow::startSystemMove()` (`glass.startMove()`), fallback deltas côté QML, position dans `QSettings` `[window]` | ✅ le natif est-il réellement pris : à confirmer à l'œil |
| Bulles virtualisées | `ListView` + `cacheBuffer`, 2 000 lignes, 10 régions | ✅ |
| Fade haut/bas | opacité par carte = fraction de la bulle sortie de la zone | ✅ (mesuré : 1 → 0,25 → écartée du verre) |
| Ombre progressive | effet d'ombre de `MultiEffect` posé sur la **matière** de la carte, teinte bleue | ✅ |
| Pas d'ombre derrière les textes | le layer ne contient que le fond : `cardFill`, `pillFill`, `hudFill` | ✅ |
| Identité visuelle | rayons 16 / 22, teintes v1 (`#5A3478DA` user, `#64222228` agent), encre `#e9eaee` | ✅ |
| Frame time stable | 6,1 ms sans ombre, 6,7 ms avec, 3 600-3 840 frames | ✅ |

## Les deux corrections du premier jour

La première version donnait une fenêtre **sans bulles visibles** et des **ombres en
dur**. Les deux se voyaient enfin grâce à `AICE_SHOT` :

1. **Ombre** : un rectangle sombre plus grand que la carte, flouté par
   `layer.effect`, est **rogé aux bords de sa propre texture** — donc pas de dégradé,
   juste une bande nette autour de la carte, posée à la main carte par carte.
   Corrigé par l'effet d'ombre fourni par Qt, posé sur la carte elle-même
   (`CardShadow` : `shadowEnabled`, `shadowBlur`, `shadowOpacity`,
   `autoPaddingEnabled`) : Qt construit l'ombre à partir de l'alpha de la carte (donc
   de ses coins arrondis) et laisse automatiquement la place au dégradé. Il n'y a
   plus rien à placer, et le dégradé existe. La place laissée autour des cartes a été
   portée à 20 px (`inset 8 + halo 12` marges de la barre) pour que l'ombre ne soit
   pas coupée par le bord de la fenêtre.
2. **Fade** : la première formule mesurait la position de la bulle *dans le repère de la
   vue*. Or une vue `BottomToTop` ne donne pas de position locale exploitable : le
   calcul sortait à 0, toutes les bulles étaient peintes à un alpha < 0,01 — donc
   invisibles, et **exclues du verre** (elles ne floutaient plus, ne captaient plus
   les clics). La formule juste se lit dans les chiffres de la vue : `y` est une
   coordonnée de contenu (négative) et le défilement est `contentY` (négatif aussi) ;
   mesurément, **position à l'écran = `cell.y - bubblesView.contentY`**, y compris
   quand le contenu fait 173 992 pixels. Le fade est ensuite la fraction de la bulle
   sortie de la zone, éteinte sur les 90 premiers pixels sortis.

Effet de bord utile : le C++ calcule maintenant l'alpha effectif de chaque carte
(produit des `opacity` jusqu'à la scène) et **écarte du verre les cartes que le fade
a éteintes**. Une bulle éteinte ne doit pas flouter ni capter les clics ; le dump
`AICE_DEBUG=1` affiche cet alpha et le compte des cartes écartées.

## L'ombre : bleue, et pas derriere les textes

L'ombre est réglée à l'œil sur capture (`CardShadow`) :

| Reglage | Valeur | Pourquoi |
| --- | --- | --- |
| `tint` | `Qt.rgba(0.02, 0.14, 0.72, 1.0)` | un **bleu sature**, pas un bleu pale : l'ombre se compose sur un fond clair (le fond de controle, comme le papier peint floute de Plasma) et une teinte desensee y devient **grise** a la mesure |
| `intensity` | 0,9 | l'ombre ne herite que de l'alpha de la carte (0,43 pour la barre) : sans toute son opacite elle reste lavace |
| `softness` | 0,5 | 50 % du flou max = 11 px de halo, pas une nappe |
| `maxBlur` | 22 px | le halo doit rester **plus court que le devers** : sinon la carte a un liseré lumineux au lieu d'une ombre |
| `drop` | 11 px (14 pour la barre, 9 pour le badge et la carte du hub) | la carte flotte, et le halo est nettement plus bas que haut |

Ces reglages se sont corriges a la mesure, pas a l'oeil : sur la capture, la
penombre sous une carte rend `B/R = 1,9` la ou le fond est a `1,2` (avant la
 correction : 1,6 contre 1,2 — donc gris). Et le halo lateral ne depasse la carte
que de 3-4 px, la penombre projetee fait ~15 px.

Et surtout : **une ombre est calculée à partir de ce que contient le layer**. Les
cartes sont donc des conteneurs (`Item`, qui porte `glassCard` et le rayon de verre)
dont l'enfant de fond (`cardFill`, `pillFill`, `hudFill`) porte seul
`layer.effect: CardShadow {}`. Le texte — `body`, le texte saisi, le badge — reste
**hors du layer**, donc il n'a pas d'ombre. Avant cette séparation, chaque glyphe
heritait de sa propre ombre : un halo sombre sous les lettres.

## Ce qui reste à valider **à l'œil**

1. **La tête du flou Plasma** (thème Utterly-Round) : `enableBlurBehind` suit-il les
   coins arrondis de la `QRegion` ou les englobe-t-il dans un rectangle ?
2. **Le drag natif vs fallback** : lire `fenetre natif / par deltas` dans le badge.
   Si « par deltas », `startSystemMove()` n'est pas accepté par KWin ici et il faut
   l'assumer (le drag devient moins fluide).
3. **`AICE_LAYER=1`** : overlay sans ancre, au-dessus de tout, sans tâche dans le
   panel. Le chemin layer-shell est écrit mais n'a pas été essayé en conditions réelles.
4. **Le gout de l'ombre** (teinte `0.02/0.14/0.72`, intensité 0,9, flou 22 px, halo 50 %, dévers 11 px) et du fade (90 px) au toucher, et le scroll à 2 000 bulles. Ces
   réglages sont discutables — `CardShadow` est un composant unique, ils se changent
   là, et `AICE_SHADOW=0` permet de comparer avec/sans en direct.
5. La persistance de position : relancer et vérifier que la fenêtre se remet où elle
   était (section `[window]` de `~/.config/a-ice/a-ice.conf`).

## Pièges (ne pas les refaire)

- **Un flou posé sur un rectangle est un piège** : `layer.effect` ne peut pas déborder
  de la texture de l'item. Pour une ombre, utiliser l'effet d'ombre de `MultiEffect`
  sur l'item qui doit l'ombre, avec `autoPaddingEnabled`.
- **Le texte dans le layer = ombre par glyphe**. L'effet d'ombre travaille sur le
  contenu du layer : si les `Text` y figurent, chacun devient une source d'ombre et
  un halo sombre apparait sous les lettres. D'où conteneur + `*Fill` : le layer ne
  voit que la matière.
- **Ombre bleue trop forte** : une teinte éclairée avec un halo large
  (`0.15/0.42/1`, intensité 1, flou 40) **colore toute la fenêtre**, parce qu'on la
  voit aussi à travers les cartes translucides. Bleu profond et halo court.
- **Ombre bleue trop lavee = ombre grise** : une teinte a peine bleutee
  (`0.07/0.24/0.62`) composee sur un fond clair rend gris (B/R 1,6 contre 1,2 de
  fond). Il faut un bleu sature, et toute l'opacite possible — l'ombre ne depasse
  jamais l'alpha de la carte qui la porte.
- **Halo plus large que le devers = liseré lumineux** : si le flou (11 px ici) est
  plus epais que le devers (11 px aussi), l'ombre entoure la carte de partout et
  rend une aura, pas une ombre posee dessous.
- **`layer.transparentBorder` n'existe plus en Qt 6.11** (il est remplacé par
  `autoPaddingEnabled`) : le QML refuse de charger avec
  `Cannot assign to non-existent property "transparentBorder"`.
- **RHI logiciel = pas d'effets**. Sur une plateforme sans GPU (offscreen dans le
  container), tout ce qui passe par `layer.effect`/`MultiEffect` ne peint **rien**,
  sans erreur. Ne jamais conclure sur une capture hors écran côté rendu.
- **Le repère local d'une vue `BottomToTop` n'est pas celui de l'écran** : ne pas
  mesurer un fade en `mapToItem` vers la vue. La formule mesurée : position à l'écran
  = `y du délégué - contentY`.
- **Un fade cassé ne se voit pas** si rien ne le mesure : les bulles peintes à un
  alpha < 0,01 disparaissent sans bruit. Le dump avec l'alpha effectif (et le compte
  des cartes écartées) l'aurait montré tout de suite.
- `QQuickWindow` possède déjà son propre `grabWindow()` (qui capture une image) : la
  méthode de déplacement du POC a été renommée `startMove()` pour ne pas mélanger
  les deux.
- **Qt 6.11** : `QQmlComponent::Status` vaut `{Null, Ready, Loading, Error}` ;
  `isCompiling()` / `isCreating()` n'existent plus.
- **`engine.load()` + `rootObjects().isEmpty()` sort silencieusement** (code 1, rien
  affiché). D'où `QQmlComponent` + `errorString()` imprimés par `fprintf` :
  `qInfo`/`console.log` sont niveau debug et **filtrés par défaut** en Qt 6.
- `qt_add_resources` n'a pas de mot-clé `ALIASES` ici, mais l'alias est déjà
  `PREFIX + chemin relatif` : la ressource est `qrc:/poc/qml/Main.qml`.
  `QFile::exists("qrc:/…")` est faux (il écrit `":/…"`).
- **Un `header: Item {}` dimensionné depuis `contentHeight` casse la virtualisation** :
  plus aucun délégué réalisé, et la région de verre ne compte plus qu'une carte.
  On ancre en bas autrement : `verticalLayoutDirection: ListView.BottomToTop` +
  modèle **du plus récent au plus vieux** (`insert(0, …)`). « Être à la fin » devient
  `contentY >= contentYMax - 4`.
- `width: view ? view.width : 0` dans un délégué rend une largeur 0 (et donc des
  largeurs de carte **négatives** dans le dump). Écrire `width: bubblesView.width`.
- `native` est un **identifiant réservé** en QML.
- Le `TextEdit` du noyau QtQuick n'a **ni `accepted`, ni `placeholderText`/
  `placeholderColor`** : `Keys.onReturnPressed` + un `Text` frère pour le texte fantôme.
- Une propriété ne peut pas avoir deux gestionnaires (`Property value set multiple times`).
- Les géométries transitoires pendant un layout peuvent remonter des positions
  négatives dans le dump (le badge HUD à `-233,20`) : c'est sans effet sur le résultat.

## Ce que M2 garde de M1

À basculer tel quel dans la vraie GUI : `GlassHelper` (presque rien à changer, sinon
les cartes réelles), la liste ancrée en bas + modèle le plus récent en tête, le fade
par carte (fraction sortie de la zone), l'ombre par effet Qt (teintée, posée sur la
matière seule), le drag natif avec
fallback, et l'outil de capture `AICE_SHOT`. À ne **pas** garder : `seedFake()` et
`replies` (bouchons de démo), le badge de mesures (outil de mesure, pas une feature),
et les variables `AICE_*` côté moteur. Le point d'entrée du vrai travail : brancher
le client du hub zcode-acp à la place de `send()` / `streamTimer`.
