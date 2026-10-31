// a-ice 2.0 — M1 : POC de l'UI (Qt Quick), hors moteur agentique.
//
// But : valider les trois ingredients avant de bouger une seule ligne du v1.
//   1. fenetre frameless translucide, floue par KWin (carte par carte)
//   2. barre de prompt attrapable pour deplacer toute la fenetre
//   3. zone de bulles virtualisee, fade en haut et en bas
// + mesure du temps de frame, pour decider si Qt Quick tient la promesse.

#include <QGuiApplication>
#include <QCommandLineParser>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QUrl>

#include <cstdio>

#include "GlassHelper.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    // Meme organisation que l'app v1 : la position se retrouve dans
    // ~/.config/a-ice/a-ice.conf, section [window].
    QGuiApplication::setOrganizationName(QStringLiteral("a-ice"));
    QGuiApplication::setApplicationName(QStringLiteral("a-ice"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "a-ice 2.0 M1 — POC UI (verre, bulles, barre grab). "
        "AICE_STRESS=1 pour 2000 bulles, AICE_PUMP=1 pour mesurer sans interaction, "
        "AICE_LAYER=1 pour l'overlay layer-shell, AICE_HUD=0 pour cacher les mesures, "
        "AICE_QML=/chemin/Main.qml pour iterer sans recompiler."));
    parser.addHelpOption();
    parser.addOption(QCommandLineOption(QStringLiteral("size"),
        QStringLiteral("Cote de la fenetre carre (defaut: 800)."),
        QStringLiteral("px"), QStringLiteral("800")));
    parser.process(app);

    GlassHelper glass;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("glass"), &glass);

    // Taille de test du principe : carree, 800 x 800 par defaut.
    engine.rootContext()->setContextProperty(
        QStringLiteral("pocSize"), qMax(240, parser.value(QStringLiteral("size")).toInt()));

    // AICE_QML=/chemin/Main.qml : charge le QML depuis le disque au lieu de la
    // ressource embarquee. Pratique pour itere sur l'UI sans recompiler.
    QUrl qmlUrl = qEnvironmentVariableIsSet("AICE_QML")
        ? QUrl::fromLocalFile(qEnvironmentVariable("AICE_QML"))
        : QUrl(QStringLiteral("qrc:/poc/qml/Main.qml"));

    // QQmlComponent plut que engine.load() : les erreurs de lecture du QML sont
    // affichees telles quelles, sinon le POC se contente de sortir sans rien dire.
    QQmlComponent component(&engine, qmlUrl);
    if (component.isError()) {
        fprintf(stderr, "a-ice-m1 : QML refuse de charger :\n%s\n",
                qUtf8Printable(component.errorString()));
        return 1;
    }
    QObject *root = component.create();
    if (!root) {
        fprintf(stderr, "a-ice-m1 : creation de l'objet racine impossible :\n%s\n",
                qUtf8Printable(component.errorString()));
        return 1;
    }

    return app.exec();
}
