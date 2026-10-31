#include <QCoreApplication>
#include <QApplication>
#include <QCommandLineParser>
#include "AiceApplet.h"
#include "Config.h"
#include "tools/TerminalTool.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("a-ice");

    QCommandLineParser parser;
    parser.setApplicationDescription("A-ICE - chat branché sur un provider OpenAI-compatible");
    parser.addHelpOption();

    QCommandLineOption configOption(
        QStringList() << "config",
        QStringLiteral("Chemin du fichier de config JSON (défaut: ~/.config/a-ice/config.json)."),
        QStringLiteral("path"));
    QCommandLineOption serverUrlOption(
        QStringList() << "server-url",
        QStringLiteral("Override de l'URL du provider (ex: http://localhost:8080/v1)."),
        QStringLiteral("url"));
    parser.addOption(configOption);
    parser.addOption(serverUrlOption);
    parser.process(app);

    // Filet de test terminal : A_ICE_TERMINAL_TEST=1 → aucune commande n'atteint bash.
    // Activé au lancement, non modifiable par le modèle depuis l'intérieur de l'app.
    // "0" ou vide = OFF, toute autre valeur = ON.
    {
        const QByteArray v = qgetenv("A_ICE_TERMINAL_TEST");
        if (!v.isEmpty() && v != "0" && v != "false")
            TerminalTool::setTestMode(true);
    }

    auto w = std::make_unique<AiceApplet>();
    w->init();

    // Chargement de la config : fichier --config, sinon emplacement par défaut.
    const QString configPath = parser.value(configOption);
    w->loadConfig(configPath);

    // Override CLI éventuel : prend le dessus sur l'api_url de la config.
    const QString serverUrl = parser.value(serverUrlOption);
    if (!serverUrl.isEmpty()) {
        w->overrideServerUrl(serverUrl);
    }

    w->show();
    return app.exec();
}
