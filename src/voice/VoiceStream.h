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

    /// Morceau de texte streamé (contentChunk, sans le thinking). Passe par le filtre
    /// « ce qui peut être dit » : ni markdown, ni emoji (voir nettoyerPourLaVoix).
    void feed(const QString &chunk);
    /// Fin du tour : vide le buffer (flush forcé, même sans ponctuation finale).
    void finish();
    /// Coupe immédiatement : son en cours + file serveur + buffer local.
    void stop();
    /// Vide le buffer local sans couper le son (fin de tour propre).
    void reset();

    void setEnabled(bool on) { m_enabled = on; }
    bool enabled() const { return m_enabled; }

    /// Chemin du socket serveur (override pour tests avec faux serveur in-process).
    void setSocketPath(const QString &path) { m_socketPath = path; }
    /// Désactive le démarrage auto du serveur (tests : ne jamais spawn le vrai ddvoice).
    void setAutoSpawnEnabled(bool on) { m_autoSpawnEnabled = on; }

    /// Diagnostic court (pour /voice).
    QString statusLine() const;

    /// État du filtre : il doit SURVIVRE aux découpages du stream. Un bloc ``` … ```
    /// ou une marque « ** » peuvent tomber pile entre deux contentChunk.
    struct Filtre {
        bool dansBlocCode = false;   // à l'intérieur d'un bloc ``` … ```
        bool dansCodeInline = false; // à l'intérieur d'un inline `code` : les symboles
                                     // de liaison deviennent des espaces, on lit les mots
        QString suspendu;            // marque inachevée reportée au chunk suivant
    };

    /** Ne laisse que du texte et de la ponctuation.
     *
     * Motif, mesuré le 21/10 sur le serveur : des emojis seuls (« 😀🎧🔊⚠️✅👍 », 6 car)
     * font partir le modèle en DÉRIVE — 163 s d'audio en 62 s de wall, et toute la file
     * bloquée derrière. Le markdown (## ** - |) alourdit sans casser, mais il est
     * illisible à l'oreille ; les blocs de code font buter le modèle (~10 car/s au lieu
     * de 19). Donc : titres, puces, citations, tableaux, liens, balises HTML et code
     * supprimes ou remis en prose ; tirets longs remplaces par une virgule (ou un point
     * devant une majuscule) ; pictogrammes et caracteres non prononcables retires.
     * Statique et sans socket : testable seul.
     */
    static QString nettoyerPourLaVoix(const QString &chunk, Filtre &etat);

signals:
    /// Un chunk a été envoyé au serveur (pour logs/debug).
    void chunkSent(int chars, double estimatedSeconds);
    /// Le serveur TTS est mort EN PLEIN stream (il avait confirmé des envois puis
    /// ne répond plus) : le stream est abandonné, plus de requeue (anti-blanc).
    void serverLost();

private:
    /// Envoie un chunk au serveur (démarrage auto si le serveur ne répond pas).
    /// pourLaFin = true : c'est LE dernier morceau du tour ; s'il ne part pas il est
    /// remis dans m_finEnAttente (et non dans m_buffer) pour être relancé.
    void sendChunk(const QString &text, bool pourLaFin = false);
    /// Remet un chunk en tête de buffer (serveur pas prêt : on ne perd rien).
    void requeue(const QString &text);
    /// Relance le dernier tronçon du tour tant que le serveur ne l'a pas accepté.
    void envoyerLaFin();
    /// Mesure l'avance estimée du TTS sur la lecture (secondes d'audio en réserve).
    double audioLead() const;
    /// Découpe et envoie ce qui est mûr dans le buffer.
    void flush();
    /// Statut serveur (false si pas de réponse). avanceServeur (optionnel) reçoit
    /// l'avance de lecture MESURÉE côté serveur, en secondes d'audio.
    bool serverStatus(bool &reading, int &queued, double *avanceServeur = nullptr) const;
    /// Connecte au socket (bloquant, timeout court).
    bool ensureConnected() const;   // mutable m_socket (reconnexion transparente)
    /// Connexion neuve pour UNE requête. Le protocole est « 1 ligne = 1 requête » et
    /// le serveur clôt après chaque réponse : un poll « statut » consomme donc la
    /// connexion, et écrire la requête suivante dessus l'enverrait dans un socket
    /// fermé — sans erreur côté client (la voix disparait silencieusement).
    bool connexionFraiche() const;
    /// Le serveur est injoignable : si il avait confirmé des envois (mort en plein
    /// stream), on marque le stream cassé (plus de requeue) et on émet serverLost.
    void handleServerUnreachable();

    mutable QLocalSocket *m_socket = nullptr;   // créé à la volée (parent = nullptr, deleteLater au destructeur)
    QString m_socketPath;
    mutable QElapsedTimer m_lastSpawnAttempt;   // verrou anti-avalanche : 1 spawn / 30 s max
    mutable bool m_spawnAttempted = false;      // une tentative de spawn déjà faite
    QString m_buffer;              // texte pas encore envoyé
    Filtre m_filtre;               // état du filtre, par tour
    QString m_finEnAttente;         // dernier tronçon pas encore accepté par le serveur
    QElapsedTimer m_finTimer;       // depuis quand on l'essaie (expiration évitée : bornée)
    bool m_enabled = false;
    bool m_firstChunkSent = false; // le premier son est-il sorti ?
    bool m_serverConfirmed = false; // le serveur a confirmé ≥1 envoi (sinon : jamais démarré)
    bool m_streamBroken = false;   // serveur mort en plein stream → on abandonne le tour
    bool m_autoSpawnEnabled = true; // false en test : ne jamais lancer le vrai serveur
    qint64 m_charsQueued = 0;      // total chars envoyés non encore "consommés"
    double m_estSecondsQueued = 0; // durée audio estimée envoyée au serveur
    QElapsedTimer m_clock;         // temps depuis le premier chunk
    QTimer m_flushTimer;           // heartbeat : flush périodique (filet)
};
