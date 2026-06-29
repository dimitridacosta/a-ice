#include "AiceApplet.h"
#include "ChatWidget.h"
#include "llama_client.h"
#include <QVBoxLayout>
#include <QMargins>
#include <QMainWindow>
#include <QWidget>

AiceApplet::AiceApplet()
{
}

AiceApplet::~AiceApplet()
{
}

void AiceApplet::init()
{
    setupUI();
    setupConnections();
}

void AiceApplet::setupUI()
{
    m_chatWidget = std::make_unique<ChatWidget>();

    auto *central = new QWidget();
    central->setObjectName("aice-window");
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(QMargins{0, 0, 0, 0});
    layout->addWidget(m_chatWidget.get());

    m_window.reset(new QMainWindow());
    m_window->setObjectName("aice-window");
    m_window->resize(480, 600);
    m_window->setCentralWidget(central);
}

void AiceApplet::loadConfig(const QString &path)
{
    m_config.load(path);
    m_chatWidget->applyConfig(m_config);
}

void AiceApplet::overrideServerUrl(const QString &url)
{
    m_chatWidget->setServerUrl(url);
}

void AiceApplet::setupConnections()
{
    // Le ChatWidget gère ses propres événements
}

void AiceApplet::show()
{
    m_window->show();
}

void AiceApplet::closeApp()
{
    m_window->close();
}
