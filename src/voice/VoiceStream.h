#pragma once

#include <QLoggingCategory>
#include <QObject>
#include <QString>
#include <QLocalSocket>
#include <QElapsedTimer>
#include <QTimer>

/**
 * VoiceStream — streaming TTS temps réel pour ddBot (ddvoice / faster-qwen3-tts).
 *
 * Branché sur LlamaClient::contentChunk (le texte parlé, JAMAIS le thinking),
 * il découpe le flux incrémental en chunks adaptatifs et les enfile au serveur
 * ddvoice (socket unix $XDG_RUNTIME_DIR/ddvoice.sock, protocole JSON ligne).
 *
 * Stratégie de découpage (latence minimale, zéro temps mort) :
 *  - début de réponse : chunk agressif, coupe à la première virgule → premier
 *    son très vite (TTFA ~200-400 ms côté TTS).
 *  - ensuite : on mesure l'avance du TTS sur la lecture (durée audio estimée
 *    des chunks envoyés moins le temps écoulé) et on allonge les chunks tant
 *    que l'avance est confortable : phrases complètes, puis plusieurs phrases,
 *    puis paragraphes entiers — toujours aux points (JAMAIS à la virgule
 *    une fois le premier son sorti).
 *  - le serveur garde l'ordre FIFO : les chunks arrivent dans l'ordre.
 *
 * Avance recalée par le statut du serveur : si sa file est vide et qu'il ne
 * lit rien, l'avance est de toute façon ~0 (on ne triche pas avec un stockage
 * local fantôme).
 *
 * Arrêt : stop() envoie {"action":"stop"} (file vidée + son coupé immédiatement
 * côté serveur) et vide le buffer local. Utilisé par /voice off et le bouton Stop.
 */
Q_DECLARE_LOGGING_CATEGORY(lcVoiceStream)

class VoiceStream : public QObject
{
    Q_OBJECT

public:
    explicit VoiceStream(QObject *parent = nullptr);
    ~VoiceStream() override;

    /// Morceau de texte streamé (contentChunk, sans le thinking).
    void feed(const QString &chunk);
    /// Fin du tour : vide le buffer (flush forcé, même sans ponctuation finale).
    void finish();
    /// Coupe immédiatement : son en cours + file serveur + buffer local.
    void stop();
    /// Vide le buffer local sans couper le son (fin de tour propre).
    void reset();

    void setEnabled(bool on) { m_enabled = on; }
    bool enabled() const { return m_enabled; }

    /// Diagnostic court (pour /voice).
    QString statusLine() const;

signals:
    /// Un chunk a été envoyé au serveur (pour logs/debug).
    void chunkSent(int chars, double estimatedSeconds);

private:
    /// Envoie un chunk au serveur (démarrage auto si le serveur ne répond pas).
    void sendChunk(const QString &text);
    /// Remet un chunk en tête de buffer (serveur pas prêt : on ne perd rien).
    void requeue(const QString &text);
    /// Mesure l'avance estimée du TTS sur la lecture (secondes d'audio en réserve).
    double audioLead() const;
    /// Découpe et envoie ce qui est mûr dans le buffer.
    void flush();
    /// Statut serveur (false si pas de réponse). avanceServeur (optionnel) reçoit
    /// l'avance de lecture MESURÉE côté serveur, en secondes d'audio.
    bool serverStatus(bool &reading, int &queued, double *avanceServeur = nullptr) const;
    /// Connecte au socket (bloquant, timeout court).
    bool ensureConnected() const;   // mutable m_socket (reconnexion transparente)

    mutable QLocalSocket *m_socket = nullptr;   // créé à la volée (parent = nullptr, deleteLater au destructeur)
    QString m_socketPath;
    mutable QElapsedTimer m_lastSpawnAttempt;   // verrou anti-avalanche : 1 spawn / 30 s max
    mutable bool m_spawnAttempted = false;      // une tentative de spawn déjà faite
    QString m_buffer;              // texte pas encore envoyé
    bool m_enabled = false;
    bool m_firstChunkSent = false; // le premier son est-il sorti ?
    qint64 m_charsQueued = 0;      // total chars envoyés non encore "consommés"
    double m_estSecondsQueued = 0; // durée audio estimée envoyée au serveur
    QElapsedTimer m_clock;         // temps depuis le premier chunk
    QTimer m_flushTimer;           // heartbeat : flush périodique (filet)
};
