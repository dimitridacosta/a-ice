#pragma once

#include <QMainWindow>
#include <QWidget>
#include <memory>
#include "Config.h"

class ChatWidget;
class LayerShellQtWindow; // fwd (LayerShellQt::Window)

namespace LayerShellQt { class Window; }

/**
 * Fenêtre overlay verre : frameless, translucide, ancrée en bas à droite
 * juste au-dessus du panel KDE. Pas de chrome, pas de fond — seul le verre
 * (barre + bulles) apparaît, flouté par KWin (glassmorphism Utterly-Round).
 * Raccourci global KDE (Ctrl+Win+Espace) pour la faire glisser hors écran
 * et la ramener avec animation. Voir ROADMAP overlay OS-level.
 */
class AiceApplet
{
public:
    AiceApplet();
    ~AiceApplet();

    void init();
    void show();
    void closeApp();

    void loadConfig(const QString &path);
    void overrideServerUrl(const QString &url);

private:
    void setupUI();
    void setupConnections();
    /// Enregistre le raccourci global KDE (Ctrl+Win+Espace) via KGlobalAccel.
    void setupGlobalShortcut();
    /// Repositionne la fenêtre en bas à droite de l'écran courant.
    void placeWindow();
    /// Re-applique les marges (recalcule la hauteur du panel KDE en live
    /// via availableGeometry) -> la fenêtre suit le panel auto-hide.
    void applyMargins();
    /// Toggle slide-right + fade (cache) / slide-in + fade-in (ramène).
    void toggleVisibility();
    /// Ramene la fenetre au premier plan + reactivate le focus.
    void bringToFront();
    /// Gère Ctrl+Q (quitter) et Échap (masquer).
    bool handleShortcut(class QKeyEvent *e);

    Config m_config;
    std::unique_ptr<QMainWindow> m_window;
    std::unique_ptr<ChatWidget> m_chatWidget;

    // Toggle overlay : LayerTop (au premier plan) <-> LayerBottom (derriere).
    LayerShellQt::Window *m_shell = nullptr;  // layer-shell surface (non-owned)
    bool m_hidden = false;
};