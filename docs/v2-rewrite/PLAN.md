# a-ice 2.0 — Plan de refonte

*Écrit le 2026-10-31 par ddBot avec Dimitri. Branche `v2` créée depuis `voice` (HEAD de la branche vocale) ; le travail UI/vocal continue séparément sur `voice`.*

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

### M3 — Intégration
- Markdown parsé **hors UI thread** (QTextDocument), historique conversation, config/SOUL, récupération progressive des features de `main`.

*Résultat partiel (2026-11-01) : la gestion de session ACP est en place — session
gardée par workspace dans `QSettings`, `session/load` à chaque lancement (le pont
rejoue l'historique), session neuve mintée quand la gardée est périmée, et fin de
tour close par le `turnState` du pont. Voir la dernière section de
[M2-client-hub.md](M2-client-hub.md). Reste : markdown hors UI thread,
multi-sessions, source unique du hub.*

### M4 — Polish
- Ombres, dégradés, animations d'entrée, thème complet, doc utilisateur.

## Règles de travail (émissaire et ddBot)
- **Commits à chaque étape qui compile**, style : `v2 : <fait>` (français, sans point final).
- **Build** (jamais sur l'hôte Bazzite, atomic read-only) :
  `distrobox enter dev-aice -- bash -c "cd /var/home/dimitri/projects/ai/projects/a-ice/build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)"`
- Dependencies manquantes : dnf install **dans la box dev-aice** ; si le process « crashe » en sortie alors que l'install a réussi, vérifier avec `rpm -q` plutôt que relancer.
- Ne pas toucher aux branches `main` et `voice`. Pas de force push.
- Référence API hub : `~/.config/a-ice/.agents/tools/zcode-emissary.mjs` (+ skill émissaire).
