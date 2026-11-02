---
date: 2026-11-02
type: note de chantier
branch: v2
---

# M3f — les trois modes de fenetre du corps

Ce que Dimitri a demande : garder la fenetre telle qu'elle est aujourd'hui, pouvoir la
garder devant les autres applis, et retrouver le geste de la v1 — une fenetre qui reste
la ou on la regarde **quand on change de bureau**.

## 1. Les trois modes

| Mode | Ce que ca veut dire | Comment c'est pose (Wayland) | Comment c'est pose (X11) |
|---|---|---|---|
| `fenetre` | fenetre ordinaire : peut etre doublee, ne vit que sur le bureau ou elle est posee | rien de special (le comportement actuel du POC) | `SkipTaskbar` |
| `au-dessus` | gardee devant les autres fenetres | couche du composeur, rang `LayerTop` | `KeepAbove` + `SkipTaskbar` |
| `fixe` | au-dessus de tout, et posee sur **tous** les bureaux — comme la v1 | couche du composeur, rang `LayerOverlay` | `KeepAbove` + `Sticky` + `SkipTaskbar` |

Les deux modes non ordinaires passent donc par LayerShellQt, la meme mecanique que la
v1 (`AiceApplet::placeWindow()` : `LayerTop`, ancres, scope), sauf que le POC pose
**aucune ancre** : la fenetre garde la position qu'on lui donne, ce qui laisse la porte
ouverte au deplacement a la barre grab. C'est la difference avec la v1, qui ancrait a
droite.

## 2. Le reglage

- **`/fenetre <mode>`** dans la barre : les trois valeurs sont proposees par la carte de
  suggestions (`/fenetre ` seul les affiche, `/f` complete en `/fenetre`).
- **un clic** dans la carte du hub (`fenetre : <mode>`) qui fait tourner les trois modes.
- **persiste** dans `a-ice.conf` (`window/mode`), comme `voice/enabled` : le choix survit
  aux lancements. `AICE_WINDOW_MODE=<mode>` force le mode au lancement sans le persister
  (knob de test) ; `AICE_LAYER=1`, l'interrupteur experimental du POC, vaut maintenant
  `fixe`.
- **`windowModeRestart`** : le composeur decide de la nature de la surface a sa creation.
  Passer d'une fenetre ordinaire a une couche (et l'inverse) ne se peut pas en cours de
  route — le reglage est quand meme pris, et la carte du hub dit `relancer pour
  appliquer`. Changer seulement de rang de couche (`au-dessus` ↔ `fixe`) se fait sans
  relancer.

## 3. Ou est l'honnetete du modele

- **X11** : les trois modes sont vraiment distincts — `Sticky` est exactement « posee sur
  tous les bureaux » sans changer de nature de fenetre.
- **Wayland** : il n'existe pas de protocole pour demander « garde devant » a une fenetre
  ordinaire. La seule facon d'etre devant, sur KWin, est la couche du composeur — et une
  couche ne depende pas d'un bureau virtuel. Donc sur Wayland, `au-dessus` et `fixe`
  devenir presque la meme chose (sauf le rang : `LayerOverlay` passe au-dessus des
  applis en plein ecran), et un « toujours devant mais seulement sur le bureau courant »
  ne se peut pas depuis le client. A confirmer a l'œil sur le poste de Dimitri (je ne
  peux pas juger le composeur depuis le terminal).

## 4. Mesures

`a-ice-m1` (plateforme wayland) :

- lancement en mode `fenetre` : `fenetre en mode « fenetre » sur wayland — fenetre ordinaire` ;
- `/f` → `[ /fenetre, /fork ]`, `/fenetre ` → `[ fenetre, au-dessus, fixe ]`,
  `/fenetre au` → `au-dessus` ;
- `/fenetre au-dessus` depuis le mode `fenetre` : mode retenu + `relancer le corps pour
  l'appliquer`, `window/mode=au-dessus` ecrit dans `a-ice.conf` ;
- lancement `AICE_WINDOW_MODE=au-dessus` : `couche du composeur` ; `/fenetre fixe`
  change de rang sans relancer ; `/fenetre fenetre` redemande une relance.

Reste a l'œil : le passage d'un bureau a l'autre en mode `fixe` (le vrai test du geste de
la v1), et ce que donnent les deux couches sur les applis en plein ecran.
