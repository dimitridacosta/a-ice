#pragma once

#include <QMainWindow>
#include <QWidget>
#include <memory>
#include "Config.h"

class ChatWidget;

/**
 * Fenêtre overlay verre : frameless, translucide, ancrée en bas à droite
 * juste au-dessus du panel KDE. Pas de chrome, pas de fond — seul le verre
 * (barre + bulles) apparaît, flouté par KWin (glassmorphism Utterly-Round).
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
    /// Repositionne la fenêtre en bas à droite de l'écran courant.
    void placeWindow();
    /// Gère Ctrl+Q (quitter) et Échap (masquer).
    bool handleShortcut(class QKeyEvent *e);

    Config m_config;
    std::unique_ptr<QMainWindow> m_window;
    std::unique_ptr<ChatWidget> m_chatWidget;
};