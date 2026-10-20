// Test VoiceStream.
//  - Sans serveur : états (enabled, buffer, statusLine), feed/finish/stop/reset
//    proprement (chunks perdus, pas de crash).
//  - AVEC un faux serveur in-process (QLocalServer éphémère, protocole ddvoice) :
//    on vérifie le CHUNKING réel (coupe ponctuation / coupe virgule à froid),
//    la BACKPRESSURE (file serveur pleine → requeue local) et la REPRISE propre
//    quand le serveur meurt EN PLEIN stream (signal serverLost, buffer vidé).
#include <QCoreApplication>
#include <QtTest/QtTest>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QThread>
#include <QMutex>
#include <QMutexLocker>
#include <atomic>
#include <memory>
#include "../src/voice/VoiceStream.h"

// Faux serveur ddvoice minimal, dans son PROPRE thread (son propre event loop).
// VoiceStream utilise des waitForReadyRead bloquants : si le faux serveur tournait
// sur le même thread, il ne pourrait pas accepter/répondre pendant ces waits →
// deadlock. Un thread dédié évite ça (comme le vrai ddvoice, qui est un process à part).
//   - répond au protocole {"action":"statut"} (attente/lecture/avance_audio_s)
//   - enregistre les requêtes {"texte":…} dans receivedTexts()
class FakeServerThread : public QThread
{
    Q_OBJECT
public:
    bool prepare()
    {
        if (!m_dir.isValid())
            return false;
        m_path = m_dir.filePath(QStringLiteral("ddvoice.sock"));
        QLocalServer::removeServer(m_path);
        return true;
    }
    QString path() const { return m_path; }

    void setQueued(int q) { QMutexLocker lk(&m_mutex); m_queued = q; }
    void setAvance(double a) { QMutexLocker lk(&m_mutex); m_avance = a; }
    QStringList receivedTexts() const { QMutexLocker lk(&m_mutex); return m_received; }

    void stopServer() { m_stop = true; }
    // Mode « protocole réel » : ddvoice clôt la connexion après chaque requête.
    // Sans cette option, le faux serveur lit plusieurs requêtes par connexion et
    // ne reproduit donc pas le comportement qui a fait perdre les chunks en prod.
    void setSingleShot(bool v) { m_singleShot = v; }

protected:
    void run() override
    {
        QLocalServer server;
        if (!server.listen(m_path))
            return;
        while (!m_stop) {
            if (!server.waitForNewConnection(100))
                continue;
            QLocalSocket *sock = server.nextPendingConnection();
            if (!sock)
                continue;
            while (!m_stop) {
                if (!sock->waitForReadyRead(100))
                    break;
                const QByteArray raw = sock->readAll();
                for (const QByteArray &line : raw.split('\n')) {
                    if (line.trimmed().isEmpty())
                        continue;
                    if (m_singleShotHandled && m_singleShot)
                        break;   // une seule requête, puis on ferme (comme ddvoice)
                    const QJsonObject obj = QJsonDocument::fromJson(line).object();
                    if (obj.value(QStringLiteral("action")).toString() == QStringLiteral("statut")) {
                        QJsonArray attente;
                        int q; double adv;
                        { QMutexLocker lk(&m_mutex); q = m_queued; adv = m_avance; }
                        for (int i = 0; i < q; ++i)
                            attente.append(QJsonObject());
                        QJsonObject rep;
                        rep.insert(QStringLiteral("pid"), 1);
                        rep.insert(QStringLiteral("modele"), QStringLiteral("fake"));
                        rep.insert(QStringLiteral("attente"), attente);
                        rep.insert(QStringLiteral("lecture"), true);
                        rep.insert(QStringLiteral("avance_audio_s"), adv);
                        sock->write(QJsonDocument(rep).toJson(QJsonDocument::Compact) + '\n');
                        sock->flush();
                    } else {
                        const QString t = obj.value(QStringLiteral("texte")).toString();
                        if (!t.isEmpty()) {
                            QMutexLocker lk(&m_mutex);
                            m_received.append(t);
                        }
                    }
                    m_singleShotHandled = true;   // « 1 requête = 1 connexion »
                }
                if (m_singleShot && m_singleShotHandled)
                    break;                        ///ddvoice ferme après la réponse
            }
            sock->close();
            delete sock;
            m_singleShotHandled = false;
        }
        server.close();
    }

private:
    QTemporaryDir m_dir;
    QString m_path;
    mutable QMutex m_mutex;
    QStringList m_received;
    int m_queued = 0;
    double m_avance = 0.0;
    bool m_singleShot = false;
    bool m_singleShotHandled = false;
    std::atomic<bool> m_stop{false};
};

// Façade : démarre/arrête le thread serveur, expose path()/receivedTexts()/killServer().
class FakeTtsServer
{
public:
    FakeTtsServer() = default;
    ~FakeTtsServer() { killServer(); }   // ne jamais laisser le thread serveur en vie

    bool start()
    {
        if (!m_srv.prepare())
            return false;
        m_srv.start();
        // Attendre que le thread ait appelé listen() (le socket existe) SANS ouvrir
        // de connexion probe : une probe resterait acceptée et bloquerait la boucle
        // waitForNewConnection du serveur avec un socket mort.
        for (int i = 0; i < 100; ++i) {
            if (QFile::exists(m_srv.path()))
                return true;
            QTest::qWait(20);
        }
        return QFile::exists(m_srv.path());
    }
    QString path() const { return m_srv.path(); }
    void setQueued(int q) { m_srv.setQueued(q); }
    void setAvance(double a) { m_srv.setAvance(a); }
    QStringList receivedTexts() const { return m_srv.receivedTexts(); }
    void killServer() { m_srv.stopServer(); m_srv.quit(); m_srv.wait(2000); }
    void singleShot() { m_srv.setSingleShot(true); }   // 1 requête = 1 connexion

private:
    FakeServerThread m_srv;
};

class TestVoiceStream : public QObject
{
    Q_OBJECT
private slots:
    void disabledByDefault();
    void feedWithoutServerDoesNotCrash();
    void stopResetsState();
    void statusLineFormat();
    void firstChunkCutAtPunctuation();
    void noArtificialCutInsideASentence();
    void paragraphCutPreferred();
    void commaOnlyAsFallbackWhenTextPilesUp();
    void hardCutWhenNoPunctuationAtAll();
    void backpressureHoldsChunkLocal();
    void serverLostMidStreamAbandonsTurn();
    void streamRecoversAfterServerLost();
    void statutPollDoesNotStealTheChunk();

private:
    // VoiceStream branché sur le faux serveur, auto-spawn coupé (jamais de vrai ddvoice).
    static std::unique_ptr<VoiceStream> makeStream(const FakeTtsServer &srv)
    {
        auto v = std::make_unique<VoiceStream>();
        v->setSocketPath(srv.path());
        v->setAutoSpawnEnabled(false);
        return v;
    }

    // Chemin de socket volontairement absent : les tests « sans serveur » ne doivent
    // JAMAIS toucher le vrai ddvoice.sock (sans ça, un v.stop() de test vide la file
    // du serveur en production et coupe le son en cours de conversation).
    static QString cheminSansServeur()
    {
        QTemporaryDir dir;
        const QString p = dir.filePath(QStringLiteral("ddvoice-absent.sock"));
        QLocalServer::removeServer(p);
        return p;
    }
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
    v.setSocketPath(cheminSansServeur());   // ne jamais parler au vrai ddvoice
    v.setAutoSpawnEnabled(false);   // ne jamais lancer le vrai ddvoice pendant un test
    v.feed("Bonjour Dimitri, ceci est un test du module voix.");
    v.finish();     // flush forcé : le serveur absent → chunk perdu proprement
    v.stop();       // stop sans serveur : pas de crash
    QVERIFY(true);
}

void TestVoiceStream::stopResetsState()
{
    VoiceStream v;
    v.setEnabled(true);
    v.setSocketPath(cheminSansServeur());   // ne jamais parler au vrai ddvoice
    v.setAutoSpawnEnabled(false);
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

// --- chunking réel, avec faux serveur in-process ----------------------------
void TestVoiceStream::firstChunkCutAtPunctuation()
{
    FakeTtsServer srv;
    QVERIFY(srv.start());
    auto v = makeStream(srv);
    v->setEnabled(true);

    // Phrase courte terminée par un point : elle doit partir en un seul chunk.
    v->feed(QStringLiteral("Bonjour le monde."));
    QTest::qWait(400);

    QVERIFY2(!srv.receivedTexts().isEmpty(), "aucun chunk recu par le faux serveur");
    QCOMPARE(srv.receivedTexts().first(), QStringLiteral("Bonjour le monde."));
}

// Pas de coupe inventée : une phrase sans limite naturelle reste dans le buffer
// (quitte à partir entière à la fin du tour) plutôt que d'être brisée à la virgule.
void TestVoiceStream::noArtificialCutInsideASentence()
{
    FakeTtsServer srv;
    QVERIFY(srv.start());
    auto v = makeStream(srv);
    v->setEnabled(true);

    const QString phrase = QStringLiteral(
        "Bonjour Dimitri, voici une phrase longue avec plusieurs virgules, qui ne se"
        " termine par aucun point, et qui reste sous les quatre cents caracteres pour"
        " verifier qu'aucune coupe arbitraire n'est envoyee en plein milieu");
    QVERIFY(phrase.size() < 400);
    v->feed(phrase);
    QTest::qWait(400);

    QVERIFY2(srv.receivedTexts().isEmpty(),
             qPrintable(QStringLiteral("coupe arbitraire dans une phrase non finie : ")
                        + srv.receivedTexts().join(QStringLiteral(" // "))));

    v->finish();          // fin du tour : le texte part COMPLET
    QTest::qWait(300);
    QCOMPARE(srv.receivedTexts().size(), 1);
    QCOMPARE(srv.receivedTexts().first(), phrase);
}

// La limite la plus naturelle est le paragraphe : il est préféré à toute autre.
void TestVoiceStream::paragraphCutPreferred()
{
    FakeTtsServer srv;
    QVERIFY(srv.start());
    auto v = makeStream(srv);
    v->setEnabled(true);

    const QString paragraphe = QStringLiteral(
        "Une premiere ligne assez longue pour depasser les vingt quatre caracteres"
        " requis, et sans ponctuation forte");
    const QString suite = QStringLiteral(
        "une deuxieme ligne qui attend encore un peu avant de pouvoir etre coupee");
    v->feed(paragraphe + QStringLiteral("\n\n") + suite);
    QTest::qWait(400);

    QVERIFY2(!srv.receivedTexts().isEmpty(), "aucun chunk recu");
    QCOMPARE(srv.receivedTexts().size(), 1);
    QCOMPARE(srv.receivedTexts().first(), paragraphe);

    v->finish();
    QTest::qWait(300);
    QCOMPARE(srv.receivedTexts().size(), 2);
    QCOMPARE(srv.receivedTexts().at(1), suite);
}

// La virgule redevient utilisable… seulement quand le texte s'empile sans aucune
// limite naturelle (au-delà de SECOURS_A_PARTIR_DE) : filet anti-sec, plus habitude.
void TestVoiceStream::commaOnlyAsFallbackWhenTextPilesUp()
{
    FakeTtsServer srv;
    QVERIFY(srv.start());
    auto v = makeStream(srv);
    v->setEnabled(true);

    QString texte;
    while (texte.size() < 480)
        texte += QStringLiteral("voici un morceau de texte sans point mais avec une virgule, ");
    QVERIFY(texte.size() > 400);
    v->feed(texte);
    QTest::qWait(400);

    QVERIFY2(!srv.receivedTexts().isEmpty(), "le filet de secours n'a pas joue");
    const QString first = srv.receivedTexts().first();
    QVERIFY2(first.endsWith(QLatin1Char(',')),
             qPrintable(QStringLiteral("secours attendu sur la virgule, recu : ") + first));
    QVERIFY(first.size() < texte.size());
}

// Bloc sans AUCUNE ponctuation (code, URL…) : le filet coupe en dur, la lecture
// n'arrive jamais à sec.
void TestVoiceStream::hardCutWhenNoPunctuationAtAll()
{
    FakeTtsServer srv;
    QVERIFY(srv.start());
    auto v = makeStream(srv);
    v->setEnabled(true);

    QString bloc;
    while (bloc.size() < 600)
        bloc += QStringLiteral("mot ");
    v->feed(bloc);
    QTest::qWait(400);

    QVERIFY2(!srv.receivedTexts().isEmpty(), "aucun chunk recu sur un bloc sans ponctuation");
    // Coupe dure a 500 car : le 500e caractere tombe sur un espace (« mot » repetes),
    // retire par trimmed() → 499.
    QCOMPARE(srv.receivedTexts().first().size(), 499);
}

void TestVoiceStream::backpressureHoldsChunkLocal()
{
    FakeTtsServer srv;
    QVERIFY(srv.start());
    srv.setQueued(8);   // file serveur pleine (>= 6) → VoiceStream doit requeue localement
    auto v = makeStream(srv);
    v->setEnabled(true);

    v->feed(QStringLiteral("Texte a envoyer mais le serveur est deja engorge."));
    QTest::qWait(400);   // plusieurs heartbeats : retentent, restent bloqués par la file pleine

    QVERIFY2(srv.receivedTexts().isEmpty(),
             "chunk envoye alors que la file serveur etait pleine (backpressure ignoree)");
    QVERIFY2(!v->statusLine().contains(QStringLiteral("buffer 0 car")),
             "buffer local vide alors que le chunk aurait du etre requeue");
}

void TestVoiceStream::serverLostMidStreamAbandonsTurn()
{
    FakeTtsServer srv;
    QVERIFY(srv.start());
    auto v = makeStream(srv);
    v->setEnabled(true);

    QSignalSpy spy(v.get(), &VoiceStream::serverLost);

    // 1) un chunk passe → le serveur est "confirmé" vivant.
    v->feed(QStringLiteral("Premier chunk."));
    QTest::qWait(300);
    QVERIFY(!srv.receivedTexts().isEmpty());

    // 2) le serveur meurt en plein stream.
    srv.killServer();
    QTest::qWait(150);

    // 3) la suite : VoiceStream detecte la mort, emet serverLost (1x), vide le
    //    buffer et n'accumule plus rien (pas de requeue fantome).
    v->feed(QStringLiteral("Et la suite, qui ne sera jamais lue."));
    QTest::qWait(200);

    QCOMPARE(spy.count(), 1);
    QVERIFY(v->statusLine().contains(QStringLiteral("perdu")));
    QVERIFY(v->statusLine().contains(QStringLiteral("buffer 0 car")));
}

// Un serveur mort ne doit pas condamner la conversation : ChatWidget appelle
// reset() au debut de chaque tour -> le tour suivant doit pouvoir reparler.
void TestVoiceStream::streamRecoversAfterServerLost()
{
    FakeTtsServer srv1;
    QVERIFY(srv1.start());
    auto v = makeStream(srv1);
    v->setEnabled(true);

    v->feed(QStringLiteral("Premier chunk."));
    QTest::qWait(300);
    QVERIFY(!srv1.receivedTexts().isEmpty());

    srv1.killServer();
    QTest::qWait(150);
    v->feed(QStringLiteral("Suite perdue."));
    QTest::qWait(200);
    QVERIFY(v->statusLine().contains(QStringLiteral("perdu")));

    // Tour suivant : reset() (cree par ChatWidget avant d'envoyer).
    v->reset();
    QVERIFY2(!v->statusLine().contains(QStringLiteral("perdu")),
             "le stream est reste condamne apres reset()");

    FakeTtsServer srv2;
    QVERIFY(srv2.start());
    v->setSocketPath(srv2.path());
    v->feed(QStringLiteral("Le tour suivant doit reparler."));
    QTest::qWait(400);

    QVERIFY2(!srv2.receivedTexts().isEmpty(),
             "le tour suivant n'a rien envoye : voix morte pour toute la conversation");
}

// Le vrai ddvoice clôt la connexion après chaque requête. sendChunk poll « statut »
// (backpressure) puis écrit le « texte » : si la connexion n'est pas reprise, le
// chunk part dans un socket fermé et la voix disparait sans aucune erreur.
void TestVoiceStream::statutPollDoesNotStealTheChunk()
{
    FakeTtsServer srv;
    srv.singleShot();
    QVERIFY(srv.start());
    auto v = makeStream(srv);
    v->setEnabled(true);

    v->feed(QStringLiteral("Bonjour Dimitri, voici un test du protocole."));
    QTest::qWait(500);

    QVERIFY2(!srv.receivedTexts().isEmpty(),
             "chunk perdu : le poll « statut » a consommé la connexion du serveur");
}

QTEST_MAIN(TestVoiceStream)
#include "test_voicestream.moc"
