#include "VoiceStream.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QProcess>
#include <QDebug>
#include <QRegularExpression>
#include <QStringList>
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
// (anti-dérive AR de son côté) — on reste sous, paragraphes inclus. 500 ≈ 22 s de
// voix au débit réel de Noémie (22,7 car/s, mesure du 26/10) : le mur d'un
// « paragraphe dense », le découpage adaptatif coupe bien avant. Choix Dimitri.
static constexpr int MAX_CHUNK_CHARS = 500;
// Chunk « mûr » minimal : pas la peine d'envoyer 3 caractères.
static constexpr int MIN_CHUNK_CHARS = 24;
// Période du heartbeat flush (filet : ponctuation exotique, fin de stream).
static constexpr int FLUSH_INTERVAL_MS = 120;
// Bornée : le dernier tronçon est relancé jusqu'ici, pas au-delà (un serveur mort ne
// doit pas faire tourner le client indéfiniment sur un morceau déjà périmé).
static constexpr int FIN_EXPIRATION_MS = 60000;

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

// --- ce qui peut être dit ---------------------------------------------------
namespace {

QRegularExpression multiligne(const QString &motif)
{
    QRegularExpression re(motif);
    re.setPatternOptions(QRegularExpression::MultilineOption);  // ^ et $ = ligne
    return re;
}

// Motif appliqué dans cet ordre : blocs de code, tableaux, liens, balises, structures
// de ligne, marques inline, puis caractère par caractère.
const QRegularExpression FENCE(QStringLiteral("```+"));
const QRegularExpression LIEN_IMAGE(QStringLiteral("!\\[([^\\]]*)\\]\\([^)]*\\)"));
const QRegularExpression LIEN(QStringLiteral("\\[([^\\]]*)\\]\\([^)]*\\)"));
const QRegularExpression URL(QStringLiteral("(?:https?://|www\\.)[^\\s)\\]]+"));
const QRegularExpression BALISE_HTML(QStringLiteral("</?[A-Za-z][A-Za-z0-9]*(?:\\s[^>]*)?>"));
const QRegularExpression TITRE(multiligne(QStringLiteral("^ {0,3}#{1,6}[ \\t]+")));
const QRegularExpression CUETON(multiligne(QStringLiteral("^ {0,3}[-+*\x{00b7}][ \\t]+")));
const QRegularExpression CITATION(multiligne(QStringLiteral("^ {0,3}>[ \\t]*")));
const QRegularExpression REGLE_H(multiligne(QStringLiteral("^ {0,3}[-*_]{3,}[ \\t]*$")));
const QRegularExpression MARQUES(QStringLiteral("[*_]{2,}|~{2,}"));
// Caractères invisibles : ils ne se prononcent pas, et ils cassent les ancres de début
// de ligne si on les retire après les règles structurelles (ZWSP devant « ## »).
const QRegularExpression INVISIBLES(QStringLiteral(
    "[\\x{00AD}\\x{200B}-\\x{200D}\\x{2060}\\x{FE00}-\\x{FE0F}\\x{FEFF}]"));

// Recollage : un caractère retiré ne doit pas laisser de trou dans la phrase.
const QRegularExpression POINTS_SUSPENDUS(QStringLiteral("\\.{3}"));
const QRegularExpression ESPACES(QStringLiteral("[ \\t\x{00a0}]+"));
const QRegularExpression ESPACE_AVANT_PONCTUATION(QStringLiteral("[ \\t\x{00a0}]+([,;:.!?])"));
const QRegularExpression ESPACE_APRES_PONCTUATION(QStringLiteral("([,;:.!?])(?=\\p{L})"));
const QRegularExpression PONCTUATION_REPETEE(QStringLiteral("([,;:.!?])\\1+"));
const QRegularExpression ESPACE_AVANT_RETOUR(QStringLiteral(" +\n"));
const QRegularExpression LIGNES_VIDES(QStringLiteral("\n{3,}"));

// Whitelist, pas blacklist : ce qui est prononçable. Les pictogrammes (emoji, ✅, ★,
// flèches, dessin de tableaux) ne passent pas, et les emoji hors BMP sont faits de
// substituants — rejetés aussi.
bool caractereParable(const QChar &c)
{
    const ushort u = c.unicode();
    // Invisibles et « colorants » d'emoji : jamais prononçables seuls.
    if (u == 0x200B || u == 0x200C || u == 0x200D || u == 0x200E || u == 0x200F
        || u == 0x00AD || u == 0xFEFF || (u >= 0xFE00 && u <= 0xFE0F))
        return false;
    if (u == 0x000A || u == 0x000D || u == 0x0009)
        return true;                                  // retour ligne, tabulation
    if (c.isMark()) return true;                      // accent combinant, collé à sa lettre
    if (c.isLetterOrNumber()) return true;            // toutes les écritures
    if (c.isSpace()) return true;
    if (c.isLowSurrogate() || c.isHighSurrogate()) return false;
    if (c.isNonCharacter()) return false;
    if (u < 0x0020 || (u >= 0x007F && u <= 0x009F)) return false;   // contrôles
    if (u >= 0x21 && u <= 0x7E) {
        // Ponctuation utile à l'oreille. Ce qui suit est du balisage, pas de la voix :
        // ` * ~ ^ | < > \  ne sont jamais prononcés.
        switch (u) {
        case 0x60: case 0x2A: case 0x7E: case 0x5E: case 0x7C:
        case 0x3C: case 0x3E: case 0x5C:
            return false;
        default:
            return true;
        }
    }
    if (u >= 0x00A1 && u <= 0x00BF) return true;      // ¡ § ¶ · ± ¿ £ ©
    if (u >= 0x2000 && u <= 0x206F) {
        // « » ‘ ’ “ ” – — … ‰ passent ; les puces de liste (• ‣ ⁃) ne se prononcent pas.
        return u != 0x2022 && u != 0x2023 && u != 0x2043;
    }
    if (u == 0x20AC || u == 0x2122) return true;      // € ™
    return false;   // tout le reste (So/Sk/Sm : ✅ ★ ● → ♦ …) n'est pas prononçable
}
} // namespace

QString VoiceStream::nettoyerPourLaVoix(const QString &chunk, Filtre &etat)
{
    QString txt = etat.suspendu + chunk;
    etat.suspendu.clear();

    // 1. BLOCS DE CODE ``` … ``` : le code n'est pas de la voix (mesuré : le modèle
    //    bute et retombe à ~10 car/s au lieu de 19). L'état survit aux découpages.
    QString parle;
    int position = 0;
    QRegularExpressionMatchIterator fence = FENCE.globalMatch(txt);
    while (fence.hasNext()) {
        const QRegularExpressionMatch m = fence.next();
        if (!etat.dansBlocCode)
            parle += txt.mid(position, m.capturedStart() - position);
        position = m.capturedEnd();
        etat.dansBlocCode = !etat.dansBlocCode;
    }
    if (!etat.dansBlocCode && position < txt.size())
        parle += txt.mid(position);
    txt = parle;

    // 2. Une marque de bloc peut être coupée par le stream (« `` » puis « ` ») : elle
    //    est reportée au chunk suivant au lieu d'être traitée comme du texte.
    int fin = txt.size();
    while (fin > 0 && txt.at(fin - 1) == QLatin1Char('`') && (txt.size() - fin) < 2)
        --fin;
    if (fin < txt.size()) {
        etat.suspendu = txt.mid(fin);
        txt.truncate(fin);
    }

    // 3. Tableaux : « | sortie | actif | » devient « sortie, actif » ; les lignes de
    //    séparation (|---|---|) disparaissent.
    QStringList rangees;
    for (const QString &ligne : txt.split(QLatin1Char('\n'))) {
        if (ligne.count(QLatin1Char('|')) < 2) {
            rangees.append(ligne);
            continue;
        }
        QStringList cellules;
        for (const QString &brute : ligne.split(QLatin1Char('|'))) {
            const QString cellule = brute.trimmed();
            if (cellule.isEmpty())
                continue;
            bool separateur = true;
            for (const QChar &c : cellule) {
                if (c != QLatin1Char('-') && c != QLatin1Char(':') && !c.isSpace()) {
                    separateur = false;
                    break;
                }
            }
            if (!separateur)
                cellules.append(cellule);
        }
        rangees.append(cellules.join(QStringLiteral(", ")));
    }
    txt = rangees.join(QLatin1Char('\n'));

    // 4. Structures et marques.
    txt.remove(INVISIBLES);                     // avant les ancres : un ZWSP devant « ## »
    txt.replace(LIEN_IMAGE, QStringLiteral("\\1"));   // ![alt](url) → alt
    txt.replace(LIEN, QStringLiteral("\\1"));         // [texte](url) → texte
    txt.replace(URL, QStringLiteral("un lien"));      // une URL ne se prononce pas
    txt.replace(BALISE_HTML, QString());              // <br>, <b> …
    txt.replace(REGLE_H, QStringLiteral("\n\n"));     // « ---- » → vraie pause (avant MARQUES,
                                                      // sinon « *** » deviendrait un débris)
    txt.replace(MARQUES, QString());                  // **gras** __gras__ ~~barré~~
    // Les marques font AVANT les titres : mesuré le 21/10, le serveur a reçu
    // « ## Ce qui est déjà tranché … » parce que le LLM écrit « **## Titre** » — dans
    // l'autre ordre, MARQUES retirait les étoiles APRÈS TITRE et les « # » survivaient.
    // Deux passes : retirer une marque peut révéler un titre en début de ligne.
    for (int passe = 0; passe < 2; ++passe) {
        txt.replace(TITRE, QString());            // « ## Étape 1 » → « Étape 1 »
        txt.replace(CITATION, QString());         // « > cité » → « cité »
        txt.replace(CUETON, QString());           // « - ouvrir » → « ouvrir »
    }

    // 5. Caractère par caractère. « — » ne se prononce pas : il devient une virgule,
    //    ou un point quand ce qui suit commence une phrase (choix de Dimitri, qui ne
    //    veut pas du tiret long à l'oreille).
    QString sortie;
    sortie.reserve(txt.size());
    for (int i = 0; i < txt.size(); ++i) {
        const QChar c = txt.at(i);
        if (c == QChar(0x2014) || c == QChar(0x2013)) {           // — et –
            const QChar avant = sortie.isEmpty() ? QChar() : sortie.at(sortie.size() - 1);
            QChar apres;
            for (int j = i + 1; j < txt.size(); ++j) {
                if (!txt.at(j).isSpace()) {
                    apres = txt.at(j);
                    break;
                }
            }
            if (avant.isDigit() && apres.isDigit())
                sortie += QStringLiteral(" à ");        // « 2–3 » → « 2 à 3 »
            else if (avant.isNull() || avant.isSpace())
                sortie += (apres.isUpper() ? QLatin1Char('.') : QLatin1Char(','));
            else
                sortie += QLatin1Char(' ');
            continue;
        }
        if (caractereParable(c))
            sortie += c;
    }

    // 6. Recollage. Les paragraphes sont conservés : c'est dessus que flush() coupe.
    sortie.replace(POINTS_SUSPENDUS, QStringLiteral("…"));
    sortie.replace(ESPACES, QStringLiteral(" "));
    sortie.replace(ESPACE_AVANT_PONCTUATION, QStringLiteral("\\1"));
    sortie.replace(ESPACE_APRES_PONCTUATION, QStringLiteral("\\1 "));
    sortie.replace(PONCTUATION_REPETEE, QStringLiteral("\\1"));
    sortie.replace(ESPACE_AVANT_RETOUR, QStringLiteral("\n"));
    sortie.replace(LIGNES_VIDES, QStringLiteral("\n\n"));
    return sortie;
}

void VoiceStream::flush()
{
    if (!m_enabled)
        return;

    if (!m_finEnAttente.isEmpty()) {
        envoyerLaFin();      // le dernier morceau passe avant toute autre décision
        return;
    }
    if (m_buffer.isEmpty())
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
void VoiceStream::sendChunk(const QString &text, bool pourLaFin)
{
    if (!ensureConnected()) {
        // Serveur injoignable : si il avait déjà confirmé des envois, c'est une
        // MORT en plein stream → on abandonne le tour (le requeue protège l'envoi,
        // pas la lecture déjà lancée : on ne peut pas la rattraper). Sinon (serveur
        // jamais démarré) : on remet le chunk en tête, le heartbeat retentera.
        handleServerUnreachable();
        if (!m_streamBroken)
            (pourLaFin ? m_finEnAttente : m_buffer) += text;
        return;
    }
    bool reading = false;
    int queued = 0;
    double avanceServeur = -1.0;
    if (serverStatus(reading, queued, &avanceServeur)) {
        if (queued >= 6 || avanceServeur > 10.0) {
            // Serveur engorgé : on n'empile pas. Le chunk est gardé (buffer, ou file
            // de fin si c'est le dernier morceau), le heartbeat retentera dès que la
            // lecture aura repris.
            if (pourLaFin)
                m_finEnAttente = text;
            else
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
            (pourLaFin ? m_finEnAttente : m_buffer) += text;
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

    // Rien ne part au TTS tel quel : ce qui arrive est du markdown riche (titres,
    // tableaux, gras, blocs de code) et parfois des emojis. Mesuré sur le serveur : des
    // emojis seuls font partir le modèle en dérive (163 s d'audio pour 6 caractères),
    // bloquant toute la file derrière. Voir nettoyerPourLaVoix().
    const QString parable = nettoyerPourLaVoix(chunk, m_filtre);
    if (parable.isEmpty()) {
        qCDebug(lcVoiceStream) << "rien à dire dans ce morceau" << chunk.size()
                               << "car (balisage ou pictogrammes)";
        return;
    }
    m_buffer += parable;
    if (!m_flushTimer.isActive())
        m_flushTimer.start();
    flush();
}

// Le dernier tronçon est relancé tant que le serveur ne l'a pas pris (heartbeat), sans
// jamais dépasser FIN_EXPIRATION_MS.
void VoiceStream::envoyerLaFin()
{
    if (m_finEnAttente.isEmpty() || m_streamBroken)
        return;
    if (m_finTimer.isValid() && m_finTimer.elapsed() > FIN_EXPIRATION_MS) {
        qWarning() << "[voice] dernier tronçon abandonné après" << FIN_EXPIRATION_MS / 1000
                   << "s d'essais (" << m_finEnAttente.size() << "car) : le serveur n'a rien repris";
        m_finEnAttente.clear();
        return;
    }
    const QString bout = m_finEnAttente;
    m_finEnAttente.clear();
    sendChunk(bout, true);
}

void VoiceStream::finish()
{
    m_flushTimer.stop();

    // Le filtre peut tenir une marque inachevée (un « `` » coupé en deux par le stream) :
    // elle repasse par le filtre plutôt que d'être prononcée comme du texte.
    if (!m_filtre.suspendu.isEmpty()) {
        Filtre neuve;
        m_buffer += nettoyerPourLaVoix(m_filtre.suspendu, neuve);
        m_filtre.suspendu.clear();
    }
    if (m_filtre.dansBlocCode) {
        qWarning() << "[voice] bloc de code jamais fermé dans la réponse → ignoré";
        m_filtre.dansBlocCode = false;
    }

    qCDebug(lcVoiceStream) << "finish · reste" << m_buffer.size() << "car · enabled"
                           << m_enabled << "· streamBroken" << m_streamBroken;
    const QString chunk = m_buffer.trimmed();
    m_buffer.clear();
    if (chunk.isEmpty() || !m_enabled || m_streamBroken)
        return;

    // LE DERNIER MORCEAU DOIT PARTIR. sendChunk() peut le refuser (file serveur ≥ 6 ou
    // avance > 10 s) : avant, il retournait dans m_buffer alors que finish() venait
    // d'ARRÊTER le heartbeat — il y restait jusqu'au reset() du tour suivant, et la fin
    // du message n'était jamais dite.
    m_finEnAttente = chunk;
    m_finTimer.start();
    envoyerLaFin();
    if (!m_finEnAttente.isEmpty() && !m_streamBroken)
        m_flushTimer.start();   // le heartbeat reprend, uniquement pour cette fin
}

void VoiceStream::stop()
{
    m_buffer.clear();
    m_finEnAttente.clear();
    m_finTimer.invalidate();
    m_filtre = Filtre();
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
    m_finEnAttente.clear();
    m_finTimer.invalidate();
    m_filtre = Filtre();      // un bloc de code ouvert au tour précédent ne se promène pas
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
    QString fin;
    if (!m_finEnAttente.isEmpty())
        fin = QStringLiteral(" · fin en attente %1 car").arg(m_finEnAttente.size());
    return QStringLiteral("voice %1 · buffer %2 car · avance ≈ %3 s%4")
        .arg(etat)
        .arg(m_buffer.size())
        .arg(QString::number(audioLead(), 'f', 1))
        .arg(fin);
}
