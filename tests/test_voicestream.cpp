// Test VoiceStream — sans réseau : le socket n'est jamais connecté ici.
// On vérifie les états (enabled, buffer, statusLine) et le comportement
// feed/finish/stop/reset sans envoi réel (serveur absent → chunks perdus
// proprement, pas de crash).
#include <QCoreApplication>
#include <QtTest/QtTest>
#include "../src/voice/VoiceStream.h"

class TestVoiceStream : public QObject
{
    Q_OBJECT
private slots:
    void disabledByDefault();
    void feedWithoutServerDoesNotCrash();
    void stopResetsState();
    void statusLineFormat();
};

void TestVoiceStream::disabledByDefault()
{
    VoiceStream v;
    QVERIFY(!v.enabled());
    v.setEnabled(true);
    QVERIFY(v.enabled());
}

void TestVoiceStream::feedWithoutServerDoesNotCrash()
{
    VoiceStream v;
    v.setEnabled(true);
    v.feed("Bonjour Dimitri, ceci est un test du module voix.");
    v.finish();     // flush forcé : le serveur absent → chunk perdu proprement
    v.stop();       // stop sans serveur : pas de crash
    QVERIFY(true);
}

void TestVoiceStream::stopResetsState()
{
    VoiceStream v;
    v.setEnabled(true);
    v.feed("Un texte assez long pour remplir le buffer du module voix.");
    v.stop();
    // statusLine doit refléter l'état vide (buffer 0).
    QVERIFY(v.statusLine().contains("buffer 0"));
    QVERIFY(v.statusLine().contains("off") == false || !v.enabled());
}

void TestVoiceStream::statusLineFormat()
{
    VoiceStream v;
    const QString s = v.statusLine();
    QVERIFY(s.contains("voice off"));
    QVERIFY(s.contains("avance"));
}

QTEST_MAIN(TestVoiceStream)
#include "test_voicestream.moc"
