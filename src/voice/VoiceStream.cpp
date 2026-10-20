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
// Débit de parole français : ~15 caractères par seconde d'audio (voix posée).
static constexpr double CHARS_PER_SECOND = 15.0;
// Avance cible : le TTS doit avoir de la marge (il est RTF ~0.3, il creuse vite).
static constexpr double LEAD_COMFORT_S = 2.0;   // au-dessus → on allonge les chunks
static constexpr double LEAD_LOW_S     = 0.8;   // en dessous → chunk court (virgule)
// Plafond d'un chunk : le serveur redécoupe au-delà de 1200 chars (anti-dérive
// AR de son côté) — on reste sous, paragraphes inclus.
static constexpr int MAX_CHUNK_CHARS = 1100;
// Premier chunk : agressif, c'est lui qui porte le TTFA perçu.
static constexpr int FIRST_CHUNK_MAX = 120;
// Chunk "mûr" minimal : pas la peine d'envoyer 3 caractères.
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
    // Avance = durée audio estimée envoyée au serveur - temps écoulé depuis
    // le début du flux. Le serveur lit en continu dès qu'il a des chunks,
    // donc le temps écoulé ≈ temps de lecture (au TTFA près, négligeable).
    const double elapsed = m_clock.elapsed() / 1000.0;
    double lead = m_estSecondsQueued - elapsed;
    // Recalage serveur : si sa file est vide et qu'il ne lit plus rien,
    // l'avance réelle est ~0 (pas de réserve fantôme).
    bool reading = false;
    int queued = 0;
    if (serverStatus(reading, queued)) {
        if (!reading && queued == 0)
            lead = 0.0;
    }
    return std::max(0.0, lead);
}

bool VoiceStream::serverStatus(bool &reading, int &queued) const
{
    if (!ensureConnected())
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
        if (QFile::exists(py) && QFile::exists(srv)) {
            const QByteArray run = qgetenv("XDG_RUNTIME_DIR");
            const QString logPath = (run.isEmpty() ? QStringLiteral("/tmp")
                                                   : QString::fromLocal8Bit(run))
                                        + QStringLiteral("/ddvoice.log");
            QProcess::startDetached(py, {srv, QStringLiteral("--inactivite"),
                                         QStringLiteral("300")}, base,
                                    nullptr);
            qInfo() << "[voice] serveur ddvoice absent → démarrage (log :" << logPath << ")";
        }
        // On retente une fois (le serveur met du temps à charger — les chunks
        // en attente partiront au prochain flush ; on ne bloque pas l'UI).
        m_socket->connectToServer(m_socketPath);
        if (!m_socket->waitForConnected(150))
            return false;
    }
    return true;
}

// --- découpage ---------------------------------------------------------------
void VoiceStream::flush()
{
    if (!m_enabled || m_buffer.isEmpty())
        return;

    const double lead = audioLead();
    const bool warm = m_firstChunkSent && lead > LEAD_COMFORT_S;

    // Taille visée :
    //  - tout début : petit (premier son vite)
    //  - avance faible : petit aussi (on ne peut pas se permettre d'attendre
    //    la fin d'une longue phrase — on coupe à la virgule)
    //  - avance confortable : long (phrases complètes, plusieurs phrases,
    //    jusqu'au paragraphe entier si ≤ MAX_CHUNK_CHARS)
    int target;
    if (!m_firstChunkSent)
        target = FIRST_CHUNK_MAX;
    else if (lead < LEAD_LOW_S)
        target = 90;   // coupe fine : virgule autorisée
    else
        target = MAX_CHUNK_CHARS;

    // Cherche la meilleure coupe dans le buffer.
    int cut = -1;

    if (m_buffer.size() <= target) {
        // Buffer entier sous le plafond : on l'envoie s'il est "mûr".
        // Mûr = se termine par une ponctuation forte, ou (mode fin/virgule)
        // contient une virgule, ou il déborde de partout (heartbeat).
        const QLatin1String fin("!?…");
        const QChar last = m_buffer.at(m_buffer.size() - 1);
        const bool pointFinal = last == QLatin1Char('.') || fin.contains(last);
        const bool virguleFin = last == QLatin1Char(',') || last == QLatin1Char(';')
                                || last == QLatin1Char(':');
        if (pointFinal || virguleFin
            || (warm && m_buffer.size() >= MIN_CHUNK_CHARS)) {
            cut = m_buffer.size();
        } else if (m_buffer.size() >= MAX_CHUNK_CHARS) {
            // Trop long sans aucune ponctuation → coupe forcée (filet).
            cut = MAX_CHUNK_CHARS;
        }
    } else {
        // Buffer plus long que la cible : coupe à la dernière ponctuation
        // forte AVANT la cible (jamais à la virgule une fois chaud —
        // on préfère légèrement dépasser la cible que couper au milieu).
        QStringView v(m_buffer);
        int lastStop = -1;
        for (int i = std::min<int>(target, m_buffer.size()) - 1; i >= 0; --i) {
            const QChar c = m_buffer.at(i);
            if (c == QLatin1Char('.') || c == QLatin1Char('!') ||
                c == QLatin1Char('?') || c == u'…') {
                lastStop = i;
                break;
            }
        }
        if (lastStop >= MIN_CHUNK_CHARS) {
            cut = lastStop + 1;
        } else if (!warm) {
            // Froid : première virgule après la cible (agressif, TTFA).
            const int comma = m_buffer.indexOf(QLatin1Char(','));
            if (comma >= 0 && comma < target * 2)
                cut = comma + 1;
        } else {
            // Chaud sans point dans la fenêtre : on attend (le heartbeat
            // rappellera ; paragraphe long = pas de coupe à la virgule).
            return;
        }
    }

    if (cut <= 0)
        return;

    QString chunk = m_buffer.left(cut);
    m_buffer.remove(0, cut);
    chunk = chunk.trimmed();
    if (chunk.isEmpty())
        return;

    sendChunk(chunk);
}

// --- envoi -------------------------------------------------------------------
void VoiceStream::sendChunk(const QString &text)
{
    if (!ensureConnected()) {
        qWarning() << "[voice] serveur injoignable — chunk perdu (" << text.size() << "car.)";
        return;
    }
    QJsonObject req;
    req.insert(QStringLiteral("texte"), text);
    m_socket->write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
    m_socket->flush();

    const double est = text.size() / CHARS_PER_SECOND;
    m_estSecondsQueued += est;
    m_charsQueued += text.size();
    if (!m_firstChunkSent) {
        m_firstChunkSent = true;
        m_clock.start();
    }
    emit chunkSent(text.size(), est);
    qCDebug(lcVoiceStream) << "chunk" << text.size() << "car ≈" << est << "s · avance ≈" << audioLead() << "s";
}

// --- API -------------------------------------------------------------------
void VoiceStream::feed(const QString &chunk)
{
    if (!m_enabled || chunk.isEmpty())
        return;
    m_buffer += chunk;
    if (!m_flushTimer.isActive())
        m_flushTimer.start();
    flush();
}

void VoiceStream::finish()
{
    m_flushTimer.stop();
    if (!m_buffer.trimmed().isEmpty()) {
        const QString chunk = m_buffer.trimmed();
        m_buffer.clear();
        if (m_enabled)
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
    // Stop immédiat côté serveur : file vidée + son coupé.
    if (ensureConnected()) {
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
}

QString VoiceStream::statusLine() const
{
    return QStringLiteral("voice %1 · buffer %2 car · avance ≈ %3 s")
        .arg(m_enabled ? QStringLiteral("on") : QStringLiteral("off"))
        .arg(m_buffer.size())
        .arg(QString::number(audioLead(), 'f', 1));
}
