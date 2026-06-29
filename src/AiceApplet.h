#pragma once

#include <QMainWindow>
#include <QWidget>
#include <memory>
#include "Config.h"

class ChatWidget;

class AiceApplet
{
public:
    AiceApplet();
    ~AiceApplet();

    void init();
    void show();
    void closeApp();

    /// Charge la config depuis un fichier JSON (vide = emplacement par défaut).
    void loadConfig(const QString &path);
    /// Override runtime de l'URL du provider (prioritaire sur la config).
    void overrideServerUrl(const QString &url);

private:
    void setupUI();
    void setupConnections();

    Config m_config;
    std::unique_ptr<QMainWindow> m_window;
    std::unique_ptr<ChatWidget> m_chatWidget;
};
