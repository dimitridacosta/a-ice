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
#include <QStringList>
#include <QUrl>

#include <cstdio>

#include "GlassHelper.h"
#include "HubClient.h"
#include "Markdown.h"
#include "VoiceControl.h"

#include "../src/voice/VoiceStream.h"

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
        "AICE_QML=/chemin/Main.qml pour iterer sans recompiler, "
        "AICE_SLASH='/mo,/model ' pour simuler la saisie slash (test M3d), "
        "AICE_VOICE=0 pour mesurer sans voix (la voix se regle par /voice)."));
    parser.addHelpOption();
    parser.addOption(QCommandLineOption(QStringLiteral("size"),
        QStringLiteral("Cote de la fenetre (defaut: 800), ou largeur x hauteur : 900x640."),
        QStringLiteral("px"), QStringLiteral("800")));
    parser.addOption(QCommandLineOption(QStringLiteral("prompt"),
        QStringLiteral("Envoie ce prompt des que la session ACP est ouverte (test M2)."),
        QStringLiteral("texte")));
    parser.addOption(QCommandLineOption(QStringLiteral("workspace"),
        QStringLiteral("Quel dossier du hub faire parler (ex : /var/home/dimitri/.config/a-ice). "
                       "Pose le choix pour les lancements suivants."),
        QStringLiteral("chemin")));
    parser.process(app);

    GlassHelper glass;
    HubClient hub;
    Markdown markdown;
    // La voix de la v1, reprise telle quelle : le module (src/voice/) n'est pas
    // reecrit, seul ce pont est nouveau.
    VoiceStream voiceStream;
    VoiceControl voice(&voiceStream);
    if (parser.isSet(QStringLiteral("workspace")))
        hub.setWorkspace(parser.value(QStringLiteral("workspace")));
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("glass"), &glass);
    engine.rootContext()->setContextProperty(QStringLiteral("hub"), &hub);
    engine.rootContext()->setContextProperty(QStringLiteral("markdown"), &markdown);
    engine.rootContext()->setContextProperty(QStringLiteral("voice"), &voice);

    // Test M2 : --prompt envoie un tour des que la session est ouverte,
    // AICE_TURN_QUIT ferme la fenetre des que ce tour est termine.
    if (parser.isSet(QStringLiteral("prompt")))
        hub.setPrompt(parser.value(QStringLiteral("prompt")));

    // Le hub ne parle que par ses signaux : sans ces lignes, un echec de
    // jonction ou un tour qui ne repond pas ne laisse aucune trace.
    int chunks = 0;
    int chars = 0;
    QObject::connect(&hub, &HubClient::chunkReceived, &app,
                     [&chunks, &chars](const QString &messageId, const QString &role, const QString &text) {
        (void) messageId;
        ++chunks;
        chars += text.size();
        (void) role;
    });
    QObject::connect(&hub, &HubClient::failed, &app, [](const QString &message) {
        fprintf(stderr, "a-ice-m1 : hub %s\n", qUtf8Printable(message));
    });
    if (qEnvironmentVariableIsSet("AICE_DEBUG")) {
        QObject::connect(&hub, &HubClient::statusChanged, &app, [&hub]() {
            fprintf(stderr, "a-ice-m1 : hub %s\n", qUtf8Printable(hub.status()));
        });
    }

    /*
     * M3e — la voix streaming de la v1, branchee sur les chunks du hub.
     *
     * Le filtre « ce qui peut etre dit » (ni markdown, ni emoji, ni blocs de code)
     * est dans VoiceStream : on ne lui livre que du texte d'agent, jamais notre
     * propre saisie.
     *
     * Le seuil est le tour QUE NOUS avons lance : la reprise d'une session
     * (session/load) rejoue tout l'historique par le meme signal, et un client
     * voisin (le telephone) peut parler sur la meme session — sans ce seuil le corps
     * lirait a voix haute toute la conversation reprise.
     */
    QObject::connect(&hub, &HubClient::chunkReceived, &voiceStream,
                     [&hub, &voiceStream](const QString &messageId, const QString &role, const QString &text) {
        (void) messageId;
        if (!hub.isGenerating() || role != QLatin1String("agent"))
            return;
        voiceStream.feed(text);
    });

    // Fin de tour : on vide le buffer (flush force, meme sans ponctuation finale).
    // Coupe ou erreur : on ne relance pas ce qui restait — on coupe, comme la v1.
    QObject::connect(&hub, &HubClient::turnEnded, &voiceStream, [&voiceStream](const QString &stopReason) {
        if (stopReason == QLatin1String("cancelled") || stopReason == QLatin1String("error"))
            voiceStream.stop();
        else
            voiceStream.finish();
    });
    QObject::connect(&hub, &HubClient::failed, &voiceStream, [&voiceStream]() { voiceStream.stop(); });

    // Serveur TTS mort en plein stream : le module a abandonne le tour (plus de
    // requeue, donc pas d'empilement fantome) — le corps le dit, une fois.
    QObject::connect(&voiceStream, &VoiceStream::serverLost, &app, [&voiceStream]() {
        fprintf(stderr, "a-ice-m1 : voix %s (serveur injoignable en pleine lecture)\n",
                qUtf8Printable(voiceStream.statusLine()));
    });
    if (qEnvironmentVariableIsSet("AICE_TURN_QUIT")) {
        QObject::connect(&hub, &HubClient::turnEnded, &app, [&app, &chunks, &chars](const QString &reason) {
            fprintf(stderr, "a-ice-m1 : tour termine (%s), %d chunks, %d caracteres\n",
                    qUtf8Printable(reason), chunks, chars);
            app.quit();
        });
    }

    // Taille de test du principe : un cote (carre) ou largeur x hauteur, pour
    // regarder ce que la fenetre redimensionnee fait du verre et des bulles.
    const QString sizeArg = parser.value(QStringLiteral("size"));
    int pocWidth = 800;
    int pocHeight = 800;
    const int separator = sizeArg.indexOf(QLatin1Char('x'));
    if (separator > 0) {
        pocWidth = qMax(240, sizeArg.left(separator).toInt());
        pocHeight = qMax(240, sizeArg.mid(separator + 1).toInt());
    } else {
        pocWidth = pocHeight = qMax(240, sizeArg.toInt());
    }
    engine.rootContext()->setContextProperty(QStringLiteral("pocWidth"), pocWidth);
    engine.rootContext()->setContextProperty(QStringLiteral("pocHeight"), pocHeight);

    // AICE_SLASH="/mo,/model " : alimente la barre de prompt sans clavier, une fois
    // la session ouverte, pour verifier les suggestions slash (test M3d).
    engine.rootContext()->setContextProperty(QStringLiteral("slashInputs"),
        qEnvironmentVariable("AICE_SLASH")
            .split(QLatin1Char(','), Qt::SkipEmptyParts));

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
