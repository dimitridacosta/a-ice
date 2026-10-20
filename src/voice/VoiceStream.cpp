#include "VoiceStream.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QProcess>
#include <QDebug>
#include <algorithm>

Q_LOGGING_CATEGORY(lcVoiceStream, "aice.voice")

// --- paramètres du chunker adaptatif ----------------------------------------
// Débit de parole mesuré sur la voix mymy (logs ddvoice 2026-10-20) :
// 16.0–18.4 car/s selon les phrases. On prend le HAUT de la plage :
// surestimer le débit = sous-estimer l'avance audio = chunks plus courts
// et plus fréquents = anti-blanc (le pire cas est un silence, pas un
// chunk un peu petit).
static constexpr double CHARS_PER_SECOND = 18.5;
// Plafond d'un chunk : le serveur redécoupe lui-même au-delà de 1200 chars
// (anti-dérive AR de son côté) — on reste sous, paragraphes inclus.
static constexpr int MAX_CHUNK_CHARS = 1100;
// Chunk « mûr » minimal : pas la peine d'envoyer 3 caractères.
static constexpr int MIN_CHUNK_CHARS = 24;
// Période du heartbeat flush (filet : ponctuation exotique, fin de stream).
static constexpr int FLUSH_INTERVAL_MS = 120;

VoiceStream::VoiceStream(QObject *parent)
    : QObject(parent)
{
    const QByteArray run = qgetenv("XDG_RUNTIME_DIR");
    m_socketPath = (run.isEmpty() ? QStringLiteral("/tmp")
                                  : QString::fromLocal8Bit(run))
                       + QStringLiteral("/ddvoice.sock");

    m_flushTimer.setInterval(FLUSH_INTERVAL_MS);
    connect(&m_flushTimer, &QTimer::timeout, this, &VoiceStream::flush);
}

VoiceStream::~VoiceStream()
{
    if (m_socket) {
        m_socket->disconnectFromServer();
        m_socket->deleteLater();
    }
}

// --- mesure d'avance ---------------------------------------------------------
double VoiceStream::audioLead() const
{
    if (!m_firstChunkSent || !m_clock.isValid())
        return 0.0;

    // Mesure PRINCIPALE : l'avance RÉELLE de la lecture côté serveur (audio déjà
    // écrit + chunks en file). Voix_serveur.py renvoie TOUJOURS "avance_audio_s"
    // dans le statut → disponible dès que le serveur répond. C'est la seule mesure
    // fiable : elle ne dérive jamais, même quand un tour d'outils interrompt le flux.
    bool reading = false;
    int queued = 0;
    double avanceServeur = -1.0;
    if (serverStatus(reading, queued, &avanceServeur) && avanceServeur >= 0.0)
        return std::max(0.0, avanceServeur);

    // Estimation locale (m_estSecondsQueued) : simple FALLBACK, conservé uniquement
    // pour les deux cas où le serveur ne fournit pas de mesure exploitable :
    //   1) serveur injoignable (chargement ~9 s, redémarrage) → on ne peut pas savoir ;
    //   2) serveur joignable mais "avance_audio_s" absent/négatif (JSON inattendu).
    // C'est la seule source de dérive possible ; dès que le serveur répond, la
    // mesure serveur gagne (branche du dessus) et l'estimation est ignorée.
    const double elapsed = m_clock.elapsed() / 1000.0;
    double lead = m_estSecondsQueued - elapsed;
    if (!reading && queued == 0)
        lead = 0.0;   // file vide et pas de lecture : pas d'avance fantôme
    return std::max(0.0, lead);
}

bool VoiceStream::serverStatus(bool &reading, int &queued, double *avanceServeur) const
{
    if (!connexionFraiche())
        return false;
    QJsonObject req;
    req.insert(QStringLiteral("action"), QStringLiteral("statut"));
    m_socket->write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
    if (!m_socket->waitForBytesWritten(300))
        return false;
    // Lecture d'une ligne (le serveur répond une ligne JSON).
    QByteArray buf;
    while (!buf.contains('\n')) {
        if (!m_socket->waitForReadyRead(500))
            return false;
        buf += m_socket->readAll();
    }
    const QByteArray line = buf.split('\n').first();
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    const QJsonObject obj = doc.object();
    reading = obj.value(QStringLiteral("lecture")).toBool(false);
    // "attente" est une liste d'objets en attente.
    queued = obj.value(QStringLiteral("attente")).toArray().size();
    if (avanceServeur)
        *avanceServeur = obj.value(QStringLiteral("avance_audio_s")).toDouble(-1.0);
    return true;
}

bool VoiceStream::ensureConnected() const
{
    if (m_socket && m_socket->state() == QLocalSocket::ConnectedState)
        return true;
    if (m_socket) {
        m_socket->abort();
    } else {
        m_socket = new QLocalSocket(nullptr);
    }
    m_socket->connectToServer(m_socketPath);
    if (!m_socket->waitForConnected(150)) {
        // Serveur pas lancé → on le démarre (comme ddvoice le fait).
        const QString home = QDir::homePath();
        const QString base = home + QStringLiteral(
            "/projects/ai/projects/faster-qwen3-tts");
        const QString py = base + QStringLiteral("/.venv/bin/python");
        const QString srv = base + QStringLiteral("/voix_serveur.py");
        if (m_autoSpawnEnabled && QFile::exists(py) && QFile::exists(srv)) {
            // Verrou anti-avalanche : chaque chunk qui arrive pendant le
            // chargement (8 s + warmup) repartait un spawn → OOM CUDA.
            // 1 seule tentative toutes les 30 s ; le serveur finira par
            // écouter et les chunks suivants se connecteront.
            if (!m_spawnAttempted || m_lastSpawnAttempt.elapsed() > 30000) {
                const QByteArray run = qgetenv("XDG_RUNTIME_DIR");
                const QString logPath = (run.isEmpty() ? QStringLiteral("/tmp")
                                                       : QString::fromLocal8Bit(run))
                                            + QStringLiteral("/ddvoice.log");
                QProcess::startDetached(py, {srv, QStringLiteral("--inactivite"),
                                             QStringLiteral("300")}, base,
                                        nullptr);
                m_lastSpawnAttempt.start();
                qInfo() << "[voice] serveur ddvoice absent → démarrage (log :" << logPath << ")";
            }
            m_spawnAttempted = true;
        }
        // On retente une fois (le serveur met du temps à charger — les chunks
        // en attente partiront au prochain flush ; on ne bloque pas l'UI).
        m_socket->connectToServer(m_socketPath);
        if (!m_socket->waitForConnected(150))
            return false;
    }
    return true;
}

bool VoiceStream::connexionFraiche() const
{
    if (m_socket)
        m_socket->disconnectFromServer();
    return ensureConnected();
}

// --- découpage ---------------------------------------------------------------
namespace {
// Le dernier retour à la ligne « de paragraphe » disponible (ligne blanche, ou
// buffer qui se termine sur un saut de ligne). Rend la LONGUEUR du chunk, -1 sinon.
int finDeParagraphe(const QString &buffer, int fenetre)
{
    const int from = qMin(fenetre, buffer.size()) - 1;
    for (int i = from; i >= 1; --i) {
        if (buffer.at(i) == QLatin1Char('\n') && buffer.at(i - 1).isSpace()) {
            if (i + 1 >= MIN_CHUNK_CHARS)
                return i + 1;
        }
    }
    if (buffer.endsWith(QLatin1Char('\n')) && buffer.size() >= MIN_CHUNK_CHARS
        && buffer.size() <= fenetre)
        return buffer.size();
    return -1;
}

// La dernière ponctuation « propre » (suivie d'un espace ou d'une fin de buffer,
// jamais au milieu de « 3.14 » ni d'une URL — même règle que le serveur). Rend la
// LONGUEUR du chunk, -1 sinon.
int finDePhrase(const QString &buffer, const QString &ponctuations, int fenetre)
{
    const int from = qMin(fenetre, buffer.size()) - 1;
    for (int i = from; i >= 0; --i) {
        if (!ponctuations.contains(buffer.at(i)))
            continue;
        const int next = i + 1;
        if (next < buffer.size() && !buffer.at(next).isSpace())
            continue;
        if (next < MIN_CHUNK_CHARS)
            continue;
        return next;
    }
    return -1;
}
} // namespace

void VoiceStream::flush()
{
    if (!m_enabled || m_buffer.isEmpty())
        return;

    // HIÉRARCHIE DES COUPES, de la plus naturelle à la moins naturelle :
    //   1. fin de paragraphe (ligne blanche, ou retour à la ligne en fin de buffer)
    //   2. fin de phrase (« . ! ? … » suivis d'un espace ou d'une fin)
    //   3. virgule / point-virgule / deux-points : secours, seulement si le texte
    //      s'empile sans rencontrer de limite naturelle
    //   4. coupe dure : filet anti-blanc (bloc sans AUCUNE ponctuation — code,
    //      URL, énumération…)
    //
    // La taille du chunk ne pilote plus le TTFA, et c'est assumé : mesuré côté
    // serveur le 21/10, le « premier son » tombe à 0,29 s pour 14 car comme à
    // 0,259 s pour 1022 car — le moteur synthétise en streaming intra-phrase.
    // Les coupes agressives ne gagnaient donc RIEN et faisaient seulement
    // redémarrer la prosodie du modèle à chaque morceau (« ça sonne artificiel »).
    static constexpr int SECOURS_A_PARTIR_DE = 400;   // au-delà, une virgule devient acceptable
    static constexpr int COUPE_DURE_A_PARTIR_DE = 500; // au-delà, on tranche même sans ponctuation

    const QString forte = QStringLiteral(".!?…");
    const QString souples = QStringLiteral(",;:");
    int cut = -1;
    const char *motif = "?";

    // Cas le plus fréquent : la phrase (le paragraphe) vient justement de se finir.
    const QChar dernier = m_buffer.at(m_buffer.size() - 1);
    if (m_buffer.size() <= MAX_CHUNK_CHARS
        && (dernier == QLatin1Char('\n') || forte.contains(dernier))) {
        cut = m_buffer.size();
        motif = "fin propre";
    }

    if (cut < 0) {
        if (const int p = finDeParagraphe(m_buffer, MAX_CHUNK_CHARS); p >= 0) {
            cut = p;
            motif = "paragraphe";
        } else if (const int s = finDePhrase(m_buffer, forte, MAX_CHUNK_CHARS); s >= 0) {
            cut = s;
            motif = "phrase";
        } else if (m_buffer.size() < SECOURS_A_PARTIR_DE) {
            // Pas de limite naturelle et le texte reste raisonnable : on n'invente
            // PAS de coupe. Le heartbeat (120 ms) rappellera, la phrase se finira.
            qCDebug(lcVoiceStream) << "en attente d'une limite naturelle · buffer"
                                   << m_buffer.size() << "car";
            return;
        } else if (const int v = finDePhrase(m_buffer, souples, MAX_CHUNK_CHARS); v >= 0) {
            cut = v;
            motif = "virgule (secours)";
        } else {
            cut = COUPE_DURE_A_PARTIR_DE;
            motif = "coupe dure";
        }
    }

    if (cut <= 0) {
        // Le module ne jette pas le texte : il attend une coupe exploitable.
        // Sans cette trace, « la voix ne sort pas » est indistinguable de « le
        // texte n'arrive jamais » (les seuls logs existants étaient post-envoi).
        qCDebug(lcVoiceStream) << "pas de coupe · buffer" << m_buffer.size() << "car";
        return;
    }

    QString chunk = m_buffer.left(cut);
    m_buffer.remove(0, cut);
    chunk = chunk.trimmed();
    if (chunk.isEmpty())
        return;

    qCDebug(lcVoiceStream) << "coupe" << motif << "·" << chunk.size() << "car";
    sendChunk(chunk);
}

// --- envoi -------------------------------------------------------------------
void VoiceStream::sendChunk(const QString &text)
{
    if (!ensureConnected()) {
        // Serveur injoignable : si il avait déjà confirmé des envois, c'est une
        // MORT en plein stream → on abandonne le tour (le requeue protège l'envoi,
        // pas la lecture déjà lancée : on ne peut pas la rattraper). Sinon (serveur
        // jamais démarré) : on remet le chunk en tête, le heartbeat retentera.
        handleServerUnreachable();
        if (!m_streamBroken)
            requeue(text);
        return;
    }
    bool reading = false;
    int queued = 0;
    double avanceServeur = -1.0;
    if (serverStatus(reading, queued, &avanceServeur)) {
        if (queued >= 6 || avanceServeur > 10.0) {
            // Serveur engorgé : on n'empile pas. Le chunk reste dans le buffer,
            // le heartbeat (120 ms) retentera dès que la lecture aura repris.
            requeue(text);
            return;
        }
    }
    QJsonObject req;
    req.insert(QStringLiteral("texte"), text);
    // Le poll « statut » ci-dessus a consommé la connexion : le serveur clôt après
    // chaque réponse. Écrire ici sur la même connexion enverrait le chunk dans un
    // socket fermé — sans aucune erreur côté client Qt : le chunk serait compté
    // « envoyé » et la voix disparaîtrait en silence (bug observé en prod).
    if (!connexionFraiche()) {
        handleServerUnreachable();
        if (!m_streamBroken)
            requeue(text);
        return;
    }
    m_socket->write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
    m_socket->flush();

    const double est = text.size() / CHARS_PER_SECOND;
    m_estSecondsQueued += est;
    m_charsQueued += text.size();
    m_serverConfirmed = true;   // le serveur est vivant : il a reçu ce chunk
    if (!m_firstChunkSent) {
        m_firstChunkSent = true;
        m_clock.start();
    }
    emit chunkSent(text.size(), est);
    qCDebug(lcVoiceStream) << "chunk" << text.size() << "car ≈" << est << "s · avance ≈" << audioLead() << "s";
}

void VoiceStream::handleServerUnreachable()
{
    // Distinction clé :
    //  - serveur JAMAIS confirmé (m_serverConfirmed == false) : il n'a juste pas
    //    encore démarré (chargement ~9 s) → ce n'est pas une "mort", on garde le
    //    requeue (rien n'est perdu, le heartbeat retentera).
    //  - serveur DÉJÀ confirmé puis injoignable : il est mort EN PLEIN stream.
    //    La lecture en cours est perdue (on ne peut pas la rattraper) ; empiler des
    //    chunks qu'aucun serveur ne lira = buffer qui grossit pour rien. On abandonne
    //    le tour : m_streamBroken + serverLost (une seule fois).
    if (m_serverConfirmed && !m_streamBroken) {
        m_streamBroken = true;
        m_buffer.clear();   // on ne garde pas de texte qu'aucun serveur ne lira
        qWarning() << "[voice] serveur ddvoice injoignable en plein stream → tour abandonné";
        emit serverLost();
    }
}

// --- API -------------------------------------------------------------------
void VoiceStream::requeue(const QString &text)
{
    m_buffer.prepend(text);
    if (!m_flushTimer.isActive())
        m_flushTimer.start();
}

void VoiceStream::feed(const QString &chunk)
{
    if (!m_enabled || chunk.isEmpty())
        return;
    if (m_streamBroken)
        return;   // serveur mort ce tour : on n'accumule plus de texte inutile
    m_buffer += chunk;
    if (!m_flushTimer.isActive())
        m_flushTimer.start();
    flush();
}

void VoiceStream::finish()
{
    m_flushTimer.stop();
    qCDebug(lcVoiceStream) << "finish · reste" << m_buffer.size() << "car · enabled"
                           << m_enabled << "· streamBroken" << m_streamBroken;
    if (!m_buffer.trimmed().isEmpty()) {
        const QString chunk = m_buffer.trimmed();
        m_buffer.clear();
        if (m_enabled && !m_streamBroken)
            sendChunk(chunk);
    } else {
        m_buffer.clear();
    }
    // Le clock continue de tourner : l'avance se résorbe pendant que le
    // serveur finit de lire. On ne la force pas à zéro ici.
}

void VoiceStream::stop()
{
    m_buffer.clear();
    m_flushTimer.stop();
    m_firstChunkSent = false;
    m_charsQueued = 0;
    m_estSecondsQueued = 0.0;
    m_clock.invalidate();
    m_streamBroken = false;
    // Un stop n'est PAS une mort serveur : on ne doit pas émettre serverLost si le
    // serveur est déjà parti. On neutralise la "confirmation" avant de tenter le stop.
    m_serverConfirmed = false;
    // Stop immédiat côté serveur : file vidée + son coupé.
    if (connexionFraiche()) {
        QJsonObject req;
        req.insert(QStringLiteral("action"), QStringLiteral("stop"));
        m_socket->write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
        m_socket->flush();
    }
}

void VoiceStream::reset()
{
    m_buffer.clear();
    m_flushTimer.stop();
    m_firstChunkSent = false;
    m_charsQueued = 0;
    m_estSecondsQueued = 0.0;
    m_clock.invalidate();
    m_streamBroken = false;
    m_serverConfirmed = false;
    // Nouveau tour : le verrou anti-avalanche se réarme (un serveur mis en
    // veille depuis le dernier tour doit pouvoir repartir sans délai).
    m_spawnAttempted = false;
    m_lastSpawnAttempt.invalidate();
}

QString VoiceStream::statusLine() const
{
    const QString etat = !m_enabled ? QStringLiteral("off")
                       : m_streamBroken ? QStringLiteral("perdu")
                                        : QStringLiteral("on");
    return QStringLiteral("voice %1 · buffer %2 car · avance ≈ %3 s")
        .arg(etat)
        .arg(m_buffer.size())
        .arg(QString::number(audioLead(), 'f', 1));
}
