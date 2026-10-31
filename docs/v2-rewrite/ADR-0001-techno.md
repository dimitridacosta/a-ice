# ADR-0001 — Techno GUI de a-ice 2.0

*2026-10-31. Statut : accepté.*

## Contexte
La v1 (Qt Widgets) lag et freeze quand la conversation grandit : raster CPU de toute la fenêtre + re-layout complet à chaque token. Objectif nouveau : fluidité type Zed IDE, glassmorphism GPU natif sur KDE/Bazzite, GUI 100 % découplée du moteur (client hub ACP).

## Décision
**C++17 + Qt Quick 7 (QtWidgets abandonnés), KF6, LayerShellQt.**
- Rendu 100 % GPU (QRHI) ; `ListView` recyclé virtualise des milliers de bulles.
- Blur géré par KWin côté compositeur : `KWindowEffects::enableBlurBehind` (KF6), migration vers `ext_background_effect_v1` (Plasma 6.7+, l'ancien `org_kde_kwin_blur` est déprécié — cf. Ghostty/Zed).
- Fade bulles = shader/gradient trivial sur la liste ; déplacement barre = `startSystemMove()` (geste natif).
- Capital KF6/LayerShellQt/TerminalSafety conservé ; refonte en semaines, pas en mois.

## Alternatives écartées
- **gpui (Rust, moteur de Zed)** : superbe mais pré-1.0, lié à Zed, doc standalone rare, blur KDE codé à la main dans des forks, coût d'apprentissage Rust. → **Plan B assumé** si le POC Qt Quick déçoit (décision à revoir après M1).
- **iced / Slint / egui** : aucun blur KDE natif, écosystème moins mature desktop, egui redessine en continu.
- **Electron/web** : écarté d'office — latence mémoire et non-natif contraire à l'esprit du projet.

## Conséquences
- QML introduit dans le projet (logique UI déclarative), moteur C++ inchangé dans l'esprit.
- Performance visée : 120 fps sur la RTX 2060, frame time stable en streaming.
