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
void VoiceStream::flush()
{
    if (!m_enabled || m_buffer.isEmpty())
        return;

    // Filet anti-blanc : stretch sans AUCUNE ponctuation coupable (bloc de
    // code, URL, énumération sans respiration…) → on coupe en dur à
    // EMERGENCY_CUT_CHARS plutôt que de laisser la lecture arriver à sec.
    static constexpr int EMERGENCY_CUT_CHARS = 500;
    if (m_buffer.size() > EMERGENCY_CUT_CHARS) {
        bool hasCut = false;
        for (int i = 0; i < EMERGENCY_CUT_CHARS; ++i) {
            const QChar ch = m_buffer.at(i);
            const bool punct = ch == QLatin1Char('.') || ch == QLatin1Char('!')
                || ch == QLatin1Char('?') || ch == u'…' || ch == QLatin1Char(',');
            const int next = i + 1;
            if (punct && (next >= m_buffer.size() || m_buffer.at(next).isSpace())) {
                hasCut = true;
                break;
            }
        }
        if (!hasCut) {
            const QString chunk = m_buffer.left(EMERGENCY_CUT_CHARS);
            m_buffer.remove(0, EMERGENCY_CUT_CHARS);
            sendChunk(chunk.trimmed());
            return;
        }
    }

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
                // Uniquement si suivi d'un espace (jamais au milieu d'un
                // nombre "3.14" ou d'une URL) — même règle que le serveur.
                const int next = i + 1;
                if (next >= m_buffer.size() || m_buffer.at(next).isSpace()) {
                    lastStop = i;
                    break;
                }
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

    if (cut <= 0) {
        // Le module ne jette pas le texte : il attend une coupe exploitable.
        // Sans cette trace, « la voix ne sort pas » est indistinguable de « le
        // texte n'arrive jamais » (les seuls logs existants étaient post-envoi).
        qCDebug(lcVoiceStream) << "pas de coupe · buffer" << m_buffer.size() << "car · cible"
                               << target << "· avance" << lead << "· warm" << warm;
        return;
    }

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
