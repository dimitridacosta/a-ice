// a-ice 2.0 — M2 : le client du hub, cote POC.

#include "HubClient.h"

#include <QAbstractSocket>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QCryptographicHash>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include <algorithm>

namespace {

QJsonObject readJsonFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object() : QJsonObject();
}

// Les identifiants du hub sont des nombres cote JSON, des chaines cote URL.
QString jsonId(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (value.isDouble())
        return QString::number(static_cast<qint64>(value.toDouble()));
    return {};
}

QString trimmed(const QString &url)
{
    QString out = url;
    while (out.endsWith(QLatin1Char('/')))
        out.chop(1);
    return out;
}

/*
 * Est-ce que « longer » finit sur un composant egal a « shorter » ?
 * /var/home/dimitri/projects/ai/projects/a-ice et /home/dimitri/projects/ai/
 * projects/a-ice sont le MEME projet aux yeux du hub et de la box : la box a
 * /home (reel), la machine a /var/home (le reel de Btrfs), et un simple
 * endsWith avec une barre de separation ajoutee doublerait le slash au milieu.
 */
bool sameTail(const QString &longer, const QString &shorter)
{
    if (longer == shorter)
        return true;

    const QString tail = shorter.startsWith(QLatin1Char('/')) ? shorter.mid(1) : shorter;
    if (tail.isEmpty() || longer.size() <= tail.size())
        return false;

    return longer.endsWith(tail)
        && longer.at(longer.size() - tail.size() - 1) == QLatin1Char('/');
}

/*
 * Un echec du pont porte son vrai motif dans « data » : soit une chaine, soit un
 * objet { details }. Sans ce detail, session/prompt ne dit que « Internal error ».
 */
QString errorDetail(const QJsonObject &error)
{
    const QJsonValue data = error.value(QStringLiteral("data"));
    if (data.isString())
        return data.toString();
    if (data.isObject())
        return data.toObject().value(QStringLiteral("details")).toString();
    return {};
}

/*
 * Ce que le pont repond quand la session qu il nous a rendue n existe plus cote
 * backend : « session … no longer exists in the backend — this thread cannot be
 * recovered; start a new thread ».
 */
bool looksLikeDeadSession(const QString &detail)
{
    return detail.contains(QStringLiteral("no longer exists"))
        || detail.contains(QStringLiteral("cannot be recovered"));
}

// Un identifiant complet ne tient pas dans la carte du hub.
QString shortId(const QString &id)
{
    return id.left(8);
}

} // namespace

HubClient::HubClient(QObject *parent)
    : QObject(parent)
{
    net = new QNetworkAccessManager(this);

    bridge = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);

    // Le pont est ouvert : on se presente (initialize), puis le pont repond et
    // la suite de la jonction se pilote a travers les reponses.
    connect(bridge, &QWebSocket::connected, this, [this]() {
        // Le questionnaire en mode formulaire est annonce des l'ouverture : sans
        // lui le pont retombe sur un popup yes/no par option, et une question a
        // choix multiple devient une rafale de popups.
        rpcSend(QStringLiteral("initialize"),
                QJsonObject{{QStringLiteral("protocolVersion"), 1},
                            {QStringLiteral("clientCapabilities"), QJsonObject{
                                {QStringLiteral("elicitation"),
                                 QJsonObject{{QStringLiteral("form"), QJsonObject{}}}}}}},
                false);
    });
    connect(bridge, &QWebSocket::textMessageReceived, this, &HubClient::onBridgeText);
    connect(bridge, &QWebSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) { onBridgeError(); });
    connect(bridge, &QWebSocket::disconnected, this, [this]() {
        if (voluntaryClose)
            return;
        ready = false;
        emit readyChanged();
        fail(QStringLiteral("pont ACP ferme"));
        retry->start();
    });

    retry = new QTimer(this);
    retry->setInterval(4000);
    connect(retry, &QTimer::timeout, this, [this]() {
        if (!voluntaryClose)
            connectHub();
    });
}

// --- la carte du hub, liste d'ouvreurs (M3) ----------------------------------

/*
 * Les sessions vivantes du hub, toutes instances : sessionId, titre, status et
 * workspace de chacune, pour la liste cliquable de la carte. Ce n'est pas une
 * condition de la jonction : un echec laisse la carte telle quelle.
 */
void HubClient::listSessions()
{
    if (hubHttpBase.isEmpty() || token.isEmpty())
        return;

    QNetworkRequest req(QUrl(hubHttpBase + QStringLiteral("/api/instances")));
    req.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());

    QNetworkReply *reply = net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            setStatus(QStringLiteral("liste des sessions : ") + reply->errorString());
            return;
        }

        QVariantList rows;
        const QJsonArray list = QJsonDocument::fromJson(reply->readAll()).array();
        for (const QJsonValue value : list) {
            const QJsonObject entry = value.toObject();
            const QString workspacePath = entry.value(QStringLiteral("workspace")).toString();
            const QString workspaceName = QFileInfo(workspacePath).fileName();
            const QString instanceStatus = entry.value(QStringLiteral("status")).toString();
            for (const QJsonValue session : entry.value(QStringLiteral("sessions")).toArray()) {
                const QJsonObject one = session.toObject();
                if (one.value(QStringLiteral("sessionId")).toString().isEmpty())
                    continue;
                QVariantMap row;
                row.insert(QStringLiteral("sessionId"),
                           one.value(QStringLiteral("sessionId")).toString());
                row.insert(QStringLiteral("title"),
                           one.value(QStringLiteral("title")).toString());
                // Sans titre pose par le backend, l'identifiant raccourci fait l'affaire.
                row.insert(QStringLiteral("state"),
                           one.value(QStringLiteral("status")).toString(instanceStatus));
                row.insert(QStringLiteral("workspace"), workspaceName);
                row.insert(QStringLiteral("workspacePath"), workspacePath);
                rows.append(row);
            }
        }

        sessionsValue = rows;
        emit sessionsChanged();
    });
}

/*
 * Faire parler une autre session du hub : meme instance, on session/load en
 * place (l'historique rejoue, comme a la reprise d'un lancement) ; autre
 * instance, on rejoint son pont avant de charger. Dans les deux cas, le modele
 * de bulles est vide par historyReset() — l'historique rejoue vient d'ailleurs.
 */
void HubClient::openSession(const QString &sessionId, const QString &workspacePath)
{
    if (sessionId.isEmpty())
        return;
    if (sessionId == sessionIdValue)   // deja la notre, rien a faire
        return;

    emit historyReset();
    setGenerating(false);
    retryPromptText.clear();
    promptRecoveries = 0;

    const QString canonical = QDir(workspacePath).canonicalPath();
    openWorkspacePath = canonical.isEmpty() ? workspacePath : canonical;

    if (openWorkspacePath.isEmpty() || openWorkspacePath == workspace) {
        sessionIdValue = sessionId;
        resumed = true;
        emit resumedChanged();
        setStatus(QStringLiteral("ouverture de la session %1…").arg(shortId(sessionId)));
        rpcSend(QStringLiteral("session/load"),
                QJsonObject{{QStringLiteral("sessionId"), sessionId},
                            {QStringLiteral("cwd"), workspace},
                            {QStringLiteral("mcpServers"), QJsonArray{}}},
                false);
        return;
    }

    // Une autre cerveau du hub : le pont est posé par instance, il faut une
    // nouvelle jonction. PendingOpenSession pilote la suite ( voir startSession) ;
    // la session gardee de l'autre workspace n'est pas ecrasee ici — un tour vecu
    // la remplacera (endTurn → rememberSession).
    pendingOpenSession = sessionId;
    cliWorkspace = openWorkspacePath;
    workspace = openWorkspacePath;
    QSettings settings;
    settings.setValue(QStringLiteral("hub/workspace"), openWorkspacePath);

    voluntaryClose = true;
    if (bridge->isValid())
        bridge->close();
    voluntaryClose = false;

    ready = false;
    emit readyChanged();
    instanceIdValue.clear();
    sessionIdValue.clear();
    resumed = false;
    emit resumedChanged();
    stage = Idle;
    setStatus(QStringLiteral("jonction vers « %1 »…").arg(QFileInfo(openWorkspacePath).fileName()));
    connectHub();
}

// --- reglages ---------------------------------------------------------------

/*
 * Le hub se regle EN UN SEUL ENDROIT : le bloc « hub » de
 * ~/.config/a-ice/config.json (hubUrl, token, workspace), partage par la GUI
 * et les tools. S'il manque mais que ~/.config/zcode-acp/config.json connaît
 * son « remote », on migre les valeurs une fois pour toutes dans le bloc —
 * zcode-acp redevient un detail d'installation. AICE_HUB / AICE_TOKEN /
 * AICE_WORKSPACE ecrasent tout (dev).
 */
void HubClient::loadSettings()
{
    hubHttpBase = qEnvironmentVariable("AICE_HUB");
    token = qEnvironmentVariable("AICE_TOKEN");
    workspace = qEnvironmentVariable("AICE_WORKSPACE");

    // --workspace est une decision de lancement : on la garde dans QSettings pour
    // que les suivent sans avoir a la repeter. Priorite : variable d environment,
    // puis --workspace, puis ce qui a ete pose au lancement precedent.
    if (!cliWorkspace.isEmpty()) {
        workspace = cliWorkspace;
        QSettings settings;
        settings.setValue(QStringLiteral("hub/workspace"), cliWorkspace);
    }
    if (workspace.isEmpty()) {
        QSettings settings;
        workspace = settings.value(QStringLiteral("hub/workspace")).toString();
    }

    const QString aiceConfigPath = QDir::homePath() + "/.config/a-ice/config.json";

    if (hubHttpBase.isEmpty() || token.isEmpty()) {
        const QJsonObject aice = readJsonFile(aiceConfigPath);
        const QJsonObject hub = aice.value("hub").toObject();
        if (!hub.isEmpty()) {
            if (hubHttpBase.isEmpty())
                hubHttpBase = hub.value("hubUrl").toString();
            if (token.isEmpty())
                token = hub.value("token").toString();
            if (workspace.isEmpty())
                workspace = hub.value("workspace").toString();
        }
    }

    if (hubHttpBase.isEmpty() || token.isEmpty()) {
        const QJsonObject remote =
            readJsonFile(QDir::homePath() + "/.config/zcode-acp/config.json").value("remote").toObject();
        if (!remote.isEmpty()) {
            if (hubHttpBase.isEmpty())
                hubHttpBase = QStringLiteral("http://%1:%2")
                                  .arg(remote.value("hubHost").toString(QStringLiteral("127.0.0.1")))
                                  .arg(remote.value("hubPort").toInt(8377));
            if (token.isEmpty())
                token = remote.value("token").toString();
            if (workspace.isEmpty())
                workspace = remote.value("workspace").toString();

            // Migration : le bloc devient la source unique des prochains
            // lancements (et de tout ce qui lit config.json). On ecrit sans
            // reecrire les cles que l'utilisateur y a posees.
            QJsonObject migrated;
            migrated.insert(QStringLiteral("hubUrl"), hubHttpBase);
            migrated.insert(QStringLiteral("token"), token);
            if (!workspace.isEmpty())
                migrated.insert(QStringLiteral("workspace"), workspace);
            QJsonObject full = readJsonFile(aiceConfigPath);
            if (full.isEmpty() && !QFileInfo(aiceConfigPath).dir().mkpath("."))
                return;
            if (full.value("hub").toObject().isEmpty()) {
                full.insert(QStringLiteral("hub"), migrated);
                QFile out(aiceConfigPath);
                if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
                    out.write(QJsonDocument(full).toJson(QJsonDocument::Indented));
            }
        }
    }

    if (workspace.isEmpty())
        workspace = QDir::homePath() + "/projects/ai/projects/a-ice";

    // Le hub, lui, connait le chemin reel : /home/dimitri est un lien, et sa
    // liste de workspaces est en /var home. Sans cette resolution on ne retrouve
    // jamais l'instance live et on en incube une en double.
    const QString canonical = QDir(workspace).canonicalPath();
    if (!canonical.isEmpty())
        workspace = canonical;

    hubHttpBase = trimmed(hubHttpBase);
    hubWsBase = hubHttpBase;
    hubWsBase.replace(0, 4, QStringLiteral("ws"));
}

// --- jonction ---------------------------------------------------------------

void HubClient::connectHub()
{
    loadSettings();

    if (hubHttpBase.isEmpty()) {
        fail(QStringLiteral("aucun hub regle (AICE_HUB, bloc « hub » de "
                            "~/.config/a-ice/config.json, ou « remote » de ~/.config/zcode-acp/config.json)"));
        return;
    }
    if (token.isEmpty()) {
        fail(QStringLiteral("jeton du hub introuvable"));
        return;
    }

    resolveWorkspace();
}

/*
 * Le hub vit sur la machine, pas dans la box de build : lui connait le chemin
 * reel du workspace (/var home), alors qu'une box le voit sous /home. Sans
 * resolution, on ne retrouve jamais l'instance live et on en incube une a
 * chaque lancement. On demande donc les projets du hub (GET /api/projects) et on
 * y rattache notre chemin — exact, par queue de chemin, ou par nom de dossier
 * (une seule candidate) comme le fait l'outil emissaire.
 */
void HubClient::resolveWorkspace()
{
    QNetworkRequest req(QUrl(hubHttpBase + QStringLiteral("/api/projects")));
    req.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());

    QNetworkReply *reply = net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() == QNetworkReply::NoError) {
            const QJsonArray list = QJsonDocument::fromJson(reply->readAll()).array();
            QStringList known;
            for (const QJsonValue value : list) {
                const QJsonObject entry = value.toObject();
                const QString path = entry.isEmpty() ? value.toString()
                                                     : entry.value(QStringLiteral("workspacePath")).toString();
                if (!path.isEmpty())
                    known << path;
            }

            bool resolved = false;
            for (const QString &path : known) {
                if (sameTail(path, workspace) || sameTail(workspace, path)) {
                    workspace = path;
                    resolved = true;
                    break;
                }
            }

            if (!resolved) {
                const QString oursName = QFileInfo(workspace).fileName();
                QString candidate;
                int hits = 0;
                for (const QString &path : known) {
                    if (QFileInfo(path).fileName() == oursName) {
                        ++hits;
                        candidate = path;
                    }
                }
                if (hits == 1) {
                    workspace = candidate;
                } else if (hits > 1) {
                    fail(QStringLiteral("workspace ambigu (%1) : « %2 » ne matche aucun des %3 projets du hub (%4)")
                             .arg(oursName, workspace).arg(known.size()).arg(known.join(QStringLiteral(" | "))));
                }
            }
        }

        requestInstances();
    });
}

void HubClient::disconnectHub()
{
    voluntaryClose = true;
    retry->stop();
    if (bridge->isValid())
        bridge->close();
    ready = false;
    emit readyChanged();
    setGenerating(false);
    clearPermissionAsks();
    instanceIdValue.clear();
    sessionIdValue.clear();
    // La liste des sessions suit : des lignes cliquables pointeraient sur un
    // pont mort ; elles reviennent de openSessionReady() a la reprise.
    sessionsValue.clear();
    emit sessionsChanged();
    resumed = false;
    emit resumedChanged();
    setStatus(QStringLiteral("debranche"));
}

void HubClient::requestInstances()
{
    stage = ListingInstances;
    setStatus(QStringLiteral("instances du hub…"));

    QNetworkRequest req(QUrl(hubHttpBase + QStringLiteral("/api/instances")));
    req.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());

    QNetworkReply *reply = net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            fail(QStringLiteral("GET /api/instances : ")+ reply->errorString());
            retry->start();
            return;
        }

        const QJsonArray list = QJsonDocument::fromJson(reply->readAll()).array();
        for (const QJsonValue value : list) {
            const QJsonObject entry = value.toObject();
            if (entry.value("workspace").toString() != workspace)
                continue;
            const QString id = jsonId(entry.value("id"));
            if (id.isEmpty())
                continue;
            instanceIdValue = id;
            restoreStoredSession();
            openBridge();
            return;
        }

        // Aucune instance live pour ce workspace : on en incube une, comme le
        // fait deja l'outil emissaire.
        createInstance();
    });
}

void HubClient::createInstance()
{
    stage = CreatingInstance;
    setStatus(QStringLiteral("incubation d'une instance…"));

    QNetworkRequest req(QUrl(hubHttpBase + QStringLiteral("/api/instances")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());

    QJsonObject body;
    body.insert(QStringLiteral("workspacePath"), workspace);

    QNetworkReply *reply = net->post(req, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            fail(QStringLiteral("POST /api/instances : ") + reply->errorString());
            retry->start();
            return;
        }
        instanceIdValue = jsonId(QJsonDocument::fromJson(reply->readAll()).object().value("id"));
        if (instanceIdValue.isEmpty()) {
            fail(QStringLiteral("le hub n'a pas donne d'instance"));
            retry->start();
            return;
        }
        openBridge();
    });
}

void HubClient::openBridge()
{
    stage = ConnectingBridge;
    setStatus(QStringLiteral("pont ACP vers l'instance…"));

    QNetworkRequest req(QUrl(hubWsBase + QStringLiteral("/acp?instance=") + instanceIdValue));
    req.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());
    bridge->open(req);
}

// --- la session, d une fenetre a l autre ------------------------------------

/*
 * Une session par workspace, pas par lancement : la cle est le hash du chemin
 * tel que le hub le connait (/var home), donc la meme de n'importe ou on lance.
 */
QString HubClient::sessionKey() const
{
    const QByteArray hash = QCryptographicHash::hash(workspace.toUtf8(), QCryptographicHash::Md5).toHex();
    return QStringLiteral("hub/session-") + QString::fromLatin1(hash.left(16));
}

void HubClient::restoreStoredSession()
{
    QSettings settings;
    storedSessionValue = settings.value(sessionKey()).toString();
}

void HubClient::rememberSession()
{
    if (sessionIdValue.isEmpty())
        return;
    QSettings settings;
    settings.setValue(sessionKey(), sessionIdValue);
    storedSessionValue = sessionIdValue;
}

void HubClient::forgetStoredSession()
{
    QSettings settings;
    settings.remove(sessionKey());
    storedSessionValue.clear();
}

/*
 * Deux entrees possible, selon qu on a deja vecu dans ce workspace :
 *   - session/load sur la session gardee : le pont rejoue l histoire (user et
 *     agent_message_chunk) avant meme de repondre, donc le corps retrouve ses
 *     bulles sans qu on ait a les stocker ici;
 *   - session/new sinon.
 */
void HubClient::startSession()
{
    sessionNewTries = 0;
    promptRecoveries = 0;

    // Une ouverture demandee par la carte du hub : elle passe devant la session
    // gardee, c'est elle que le corps fait parler maintenant.
    if (!pendingOpenSession.isEmpty()) {
        const QString requested = pendingOpenSession;
        pendingOpenSession.clear();
        sessionIdValue = requested;
        resumed = true;
        emit resumedChanged();
        setStatus(QStringLiteral("ouverture de la session %1…").arg(shortId(requested)));
        rpcSend(QStringLiteral("session/load"),
                QJsonObject{{QStringLiteral("sessionId"), requested},
                            {QStringLiteral("cwd"), workspace},
                            {QStringLiteral("mcpServers"), QJsonArray{}}},
                false);
        return;
    }

    if (storedSessionValue.isEmpty()) {
        mintSession();
        return;
    }

    setStatus(QStringLiteral("reprise de la session %1…").arg(shortId(storedSessionValue)));

    // Le replay arrive par session/update : le filtre porte deja sur cette session.
    sessionIdValue = storedSessionValue;
    resumed = true;
    emit resumedChanged();

    rpcSend(QStringLiteral("session/load"),
            QJsonObject{{QStringLiteral("sessionId"), storedSessionValue},
                        {QStringLiteral("cwd"), workspace},
                        {QStringLiteral("mcpServers"), QJsonArray{}}},
            false);
}

void HubClient::mintSession()
{
    rpcSend(QStringLiteral("session/new"),
            QJsonObject{{QStringLiteral("cwd"), workspace},
                        {QStringLiteral("mcpServers"), QJsonArray{}}},
            false);
}

void HubClient::startFreshConversation()
{
    forgetStoredSession();
    sessionIdValue.clear();
    retryPromptText.clear();
    promptRecoveries = 0;
    sessionNewTries = 0;
    resumed = false;
    emit resumedChanged();

    if (!bridge || !bridge->isValid()) {
        connectHub();
        return;
    }
    mintSession();
}

// --- un tour ---------------------------------------------------------------

void HubClient::send(const QString &text)
{
    if (text.trimmed().isEmpty())
        return;
    if (!ready) {
        setStatus(QStringLiteral("hub pas pret — reconnect"));
        retry->start();
        return;
    }
    if (generating)
        return;

    setGenerating(true);

    // Le tour qu on tente : s il bute sur une session perimeee, il repart avec une
    // session neuve (voir la reponse d erreur).
    retryPromptText = text;
    promptRecoveries = 0;

    QJsonArray prompt;
    prompt.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                             {QStringLiteral("text"), text}});

    QJsonObject params;
    params.insert(QStringLiteral("sessionId"), sessionIdValue);
    params.insert(QStringLiteral("prompt"), prompt);

    rpcSend(QStringLiteral("session/prompt"), params, true);
}

void HubClient::cancelTurn()
{
    if (!bridge || !generating)
        return;

    QJsonObject note;
    note.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    note.insert(QStringLiteral("method"), QStringLiteral("session/cancel"));
    note.insert(QStringLiteral("params"),
                QJsonObject{{QStringLiteral("sessionId"), sessionIdValue}});
    bridge->sendTextMessage(QJsonDocument(note).toJson());

    setGenerating(false);
    emit turnEnded(QStringLiteral("cancelled"));
}

void HubClient::rpcSend(const QString &method, const QJsonObject &params, bool promptTurn)
{
    const qint64 id = nextRpcId++;
    pendingMethod.insert(id, method);

    QJsonObject request;
    request.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    request.insert(QStringLiteral("id"), id);
    request.insert(QStringLiteral("method"), method);
    request.insert(QStringLiteral("params"), params);

    bridge->sendTextMessage(QJsonDocument(request).toJson());

    if (promptTurn)
        setStatus(QStringLiteral("tour en cours…"));
}

// --- ce qui revient du hub ------------------------------------------------

void HubClient::onBridgeText(const QString &message)
{
    const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject())
        return;
    const QJsonObject msg = doc.object();

    const bool hasMethod = msg.contains(QStringLiteral("method"));
    const bool hasId = msg.contains(QStringLiteral("id"));

    // Requete du pont (une methode ET un id) : c'est lui qui nous demande
    // quelque chose — en pratique une autorisation.
    if (hasMethod && hasId) {
        const QString method = msg.value(QStringLiteral("method")).toString();
        if (method == QStringLiteral("session/request_permission"))
            showPermissionRequest(msg);
        else if (method == QStringLiteral("elicitation/create"))
            showElicitationRequest(msg);
        else
            rejectRequest(msg);   // une requete qu'on ne rend pas ne doit pas rester suspendue
        return;
    }

    // Notification du pont : le texte du tour, et les outils qui bougent.
    if (hasMethod) {
        onBridgeNotification(msg);
        return;
    }

    // Reponse a l'une de nos requetes : la methode attendue dit ou on en est.
    const qint64 id = msg.value(QStringLiteral("id")).toVariant().toLongLong();
    const QString method = pendingMethod.take(id);
    if (method.isEmpty())
        return;

    if (msg.contains(QStringLiteral("error"))) {
        const QJsonObject error = msg.value(QStringLiteral("error")).toObject();
        const QString detail = errorDetail(error);
        const QString message = detail.isEmpty()
            ? error.value(QStringLiteral("message")).toString()
            : error.value(QStringLiteral("message")).toString() + QStringLiteral(" — ") + detail;

        // Le premier session/new d une connexion herite parfois du dernier fil lie
        // a l instance, meme quand ce fil est mort cote backend : le tour echoue
        // alors sur une session que croyions bonne. On n en meurt pas — on mint une
        // session neuve et on relance le tour, deux fois par tour au plus.
        if (method == QStringLiteral("session/prompt") && promptRecoveries < 2
            && looksLikeDeadSession(detail) && !retryPromptText.isEmpty()) {
            ++promptRecoveries;
            setStatus(QStringLiteral("session perimeee : j en mint une neuve"));
            pendingPrompt = retryPromptText;
            sessionIdValue.clear();
            sessionNewTries = 0;
            resumed = false;
            emit resumedChanged();
            mintSession();
            return;
        }

        fail(QStringLiteral("%1 : %2").arg(method, message));

        if (method == QStringLiteral("session/prompt")) {
            setGenerating(false);
            retryPromptText.clear();
            emit turnEnded(QStringLiteral("error"));
            return;
        }

        if (method == QStringLiteral("session/load")) {
            // La session gardenee n etait plus repriseable : on repart sur du neuf.
            setStatus(QStringLiteral("reprise impossible : conversation neuve"));
            forgetStoredSession();
            sessionIdValue.clear();
            sessionNewTries = 0;
            resumed = false;
            emit resumedChanged();
            mintSession();
        }
        return;
    }

    const QJsonObject result = msg.value(QStringLiteral("result")).toObject();

    if (method == QStringLiteral("initialize")) {
        startSession();
    } else if (method == QStringLiteral("session/new")) {
        sessionIdValue = jsonId(result.value(QStringLiteral("sessionId")));
        resumed = false;
        emit resumedChanged();
        verifySessionFreshness();
    } else if (method == QStringLiteral("session/load")) {
        // L historique a defile pendant la requete (voir onBridgeNotification) :
        // la repondre veut dire que la conversation est la.
        openSessionReady();
    } else if (method == QStringLiteral("session/prompt")) {
        endTurn(result.value(QStringLiteral("stopReason")).toString());
    }
}

void HubClient::onBridgeNotification(const QJsonObject &msg)
{
    const QString method = msg.value(QStringLiteral("method")).toString();
    const QJsonObject params = msg.value(QStringLiteral("params")).toObject();

    // Un pont diffuse a toutes ses connexions : sans ce filtre on verrait le
    // texte des AUTRES sessions de l'instance.
    if (!sessionIdValue.isEmpty()
        && params.value(QStringLiteral("sessionId")).toString() != sessionIdValue)
        return;

    // Une autre main (la CLI Z Code) a deja tranche la demande : on retire la
    // notre sans repondre — le pont a deja sa reponse. Le kind n'est pas que
    // « permission » : un questionnaire tranche de meme.
    if (method == QStringLiteral("$/zcode/ask_settled")) {
        settleAsk(params.value(QStringLiteral("kind")).toString(),
                  params.value(QStringLiteral("toolCallId")).toString());
        return;
    }

    if (method != QStringLiteral("session/update")) {
        // Le pont declare son etat de tour (agentCapabilities._meta.zcode.turnState).
        // Un tour peut tres bien finir cote hub sans que la reponse JSON-RPC de
        // session/prompt nous parvienne — un client tue en plein tour laisse le pont
        // dans un etat ou il ne repond plus. Ce signal-la, lui, vient toujours.
        if (method == QStringLiteral("$/zcode/turnState") || method == QStringLiteral("_zcode/turnState")) {
            if (generating && !params.value(QStringLiteral("running")).toBool()) {
                setStatus(QStringLiteral("tour termine par le pont"));
                endTurn(QStringLiteral("end_turn"));
            }
            return;
        }
        return;
    }

    const QJsonObject update = params.value(QStringLiteral("update")).toObject();
    const QString kind = update.value(QStringLiteral("sessionUpdate")).toString();

    if (kind == QStringLiteral("agent_message_chunk") || kind == QStringLiteral("user_message_chunk")) {
        const QJsonObject content = update.value(QStringLiteral("content")).toObject();
        if (content.value(QStringLiteral("type")).toString() != QStringLiteral("text"))
            return;

        // messageId designe le MEME message d un tour a l autre : c est ce qui fait
        // qu un chunk qui s ajoute ne rouvre pas une bulle, et que le replay de
        // session/load en ouvre une par message reprise.
        const QString messageId = update.value(QStringLiteral("messageId")).toString();
        const QString role = kind == QStringLiteral("user_message_chunk")
                                ? QStringLiteral("user")
                                : QStringLiteral("agent");
        const QString text = content.value(QStringLiteral("text")).toString();
        if (text.isEmpty())
            return;
        emit chunkReceived(messageId, role, text);
        return;
    }

    if (kind == QStringLiteral("tool_call") || kind == QStringLiteral("tool_call_update")) {
        const QString title = update.value(QStringLiteral("title")).toString();
        const QString status = update.value(QStringLiteral("status")).toString();
        const QString line = status.isEmpty() ? title : title + QStringLiteral(" · ") + status;
        if (line.isEmpty())
            return;   // aucun titre : pas exploitable tel quel
        if (toolLineValue == line)
            return;
        toolLineValue = line;
        emit toolLineChanged();
        return;
    }

    // plan, available_commands_update, usage_update : pas encore exploites ici.
}

void HubClient::onBridgeError()
{
    fail(QStringLiteral("pont ACP : ") + bridge->errorString());
}

/*
 * Garde reprise de l'outil emissaire : sur une instance deja live, le premier
 * session/new d'une connexion peut ADOPTER la session que le pont tient deja
 * (celle du telephone). On verifie dans /api/instances que la session qu'on
 * vient de recevoir n'est pas une session existante : sinon on retente une
 * fois, et si elle colle encore on refuse — mieux vaut le dire que parler a la
 * place d'un autre maitre.
 */
void HubClient::verifySessionFreshness()
{
    ++sessionNewTries;

    QNetworkRequest req(QUrl(hubHttpBase + QStringLiteral("/api/instances")));
    req.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());

    QNetworkReply *reply = net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            // Cette verification n'est pas une condition de la jonction.
            openSessionReady();
            return;
        }

        const QJsonArray list = QJsonDocument::fromJson(reply->readAll()).array();
        bool adoptsAnExistingSession = false;
        QStringList declared;
        for (const QJsonValue value : list) {
            const QJsonObject entry = value.toObject();
            if (jsonId(entry.value(QStringLiteral("id"))) != instanceIdValue)
                continue;
            for (const QJsonValue session : entry.value(QStringLiteral("sessions")).toArray()) {
                const QString id = session.toObject().value(QStringLiteral("sessionId")).toString();
                declared << id;
                if (id == sessionIdValue)
                    adoptsAnExistingSession = true;
            }
            break;
        }

        if (qEnvironmentVariableIsSet("AICE_DEBUG"))
        // Ce que le hub declare au moment ou le pont nous repond : c'est la
        // seule facon de voir si la garde porte sur de vra donnees.
            setStatus(QStringLiteral("garde : instance %1, sessions declarees %2, la notre %3")
                          .arg(instanceIdValue, declared.join(QStringLiteral(", ")), sessionIdValue));

        if (!adoptsAnExistingSession) {
            openSessionReady();
            return;
        }
        if (sessionNewTries < 2) {
            setStatus(QStringLiteral("session reprise : on en mint une neuve"));
            rpcSend(QStringLiteral("session/new"),
                    QJsonObject{{QStringLiteral("cwd"), workspace},
                                {QStringLiteral("mcpServers"), QJsonArray{}}},
                    false);
            return;
        }

        fail(QStringLiteral("instance occupee : une session live porte deja ce workspace"));
    });
}

void HubClient::openSessionReady()
{
    ready = true;
    stage = Live;
    emit readyChanged();
    sessionStatusLine();
    listSessions();

    if (!pendingPrompt.isEmpty()) {
        const QString prompt = pendingPrompt;
        pendingPrompt.clear();
        send(prompt);
    }
}

void HubClient::sessionStatusLine()
{
    setStatus(QStringLiteral("instance %1 · session %2 · %3")
                  .arg(instanceIdValue, shortId(sessionIdValue),
                       resumed ? QStringLiteral("conversation reprise")
                               : QStringLiteral("conversation neuve")));
}

/*
 * Fermer un tour, d une reponse JSON-RPC ou du turnState du pont : dans les deux
 * cas la session est vivante, donc c est elle qu on garde pour le prochain
 * lancement.
 */
void HubClient::endTurn(const QString &stopReason)
{
    setGenerating(false);
    retryPromptText.clear();
    promptRecoveries = 0;
    // Un tour clos n'a plus de demande en cours : le moteur decline ce qui reste.
    clearPermissionAsks();
    rememberSession();
    sessionStatusLine();
    emit turnEnded(stopReason);
}

// --- autorisations ---------------------------------------------------------

namespace {

/*
 * L'ordre des boutons : les autorisations a gauche (la premiere est le choix par
 * defaut, Entrée), les refus a droite. Les kinds ne sont pas les quatre kinds ACP
 * stricts : le pont connait aussi allow_project et allow_session (chez ZCode), et
 * un filtre strict les ferait disparaitre.
 */
int optionRank(const QString &kind)
{
    if (kind.startsWith(QLatin1String("allow")))
        return 0;
    if (kind.startsWith(QLatin1String("reject")))
        return 2;
    return 1;
}

QString compactInput(const QJsonObject &toolCall)
{
    const QJsonValue raw = toolCall.contains(QStringLiteral("rawInput"))
                             ? toolCall.value(QStringLiteral("rawInput"))
                             : QJsonValue::Undefined;
    const QJsonValue shown = raw.isUndefined() ? QJsonValue(toolCall) : raw;
    QString text = QString::fromUtf8(
        QJsonDocument(shown.toObject()).toJson(QJsonDocument::Compact));
    // Une entree d'outil peut faire des milliers de caracteres : la carte en
    // porte le debut, et le texte entier reste accessible ailleurs.
    if (text.size() > 900)
        text = text.left(900) + QStringLiteral(" …");
    return text;
}

// Le moteur joint parfois deja la sortie de son terminal dans la demande.
QString terminalOutput(const QJsonObject &toolCall)
{
    const QJsonObject meta = toolCall.value(QStringLiteral("_meta")).toObject();
    const QString data = meta.value(QStringLiteral("terminal_output")).toObject()
                            .value(QStringLiteral("data")).toString();
    if (data.isEmpty())
        return QString();
    QString text = data.trimmed();
    if (text.size() > 600)
        text = text.right(600) + QStringLiteral(" …");
    return text;
}

} // namespace

int HubClient::askCount() const
{
    return permissionAsks.size() + elicitationAsks.size();
}

int HubClient::askIndex() const
{
    // La carte montree est la premiere de la file : le compteur dit sur combien.
    return askCount() > 0 ? 1 : 0;
}

QString HubClient::permissionTool() const
{
    return permissionAsks.isEmpty() ? QString() : permissionAsks.first().tool;
}

QString HubClient::permissionKind() const
{
    return permissionAsks.isEmpty() ? QString() : permissionAsks.first().toolKind;
}

QString HubClient::permissionDetail() const
{
    return permissionAsks.isEmpty() ? QString() : permissionAsks.first().detail;
}

QString HubClient::permissionOutput() const
{
    return permissionAsks.isEmpty() ? QString() : permissionAsks.first().output;
}

QVariantList HubClient::permissionOptions() const
{
    return permissionAsks.isEmpty() ? QVariantList() : permissionAsks.first().options;
}

void HubClient::showPermissionRequest(const QJsonObject &msg)
{
    AskRequest ask;
    ask.rpcId = msg.value(QStringLiteral("id")).toVariant().toLongLong();

    const QJsonObject params = msg.value(QStringLiteral("params")).toObject();
    const QJsonObject toolCall = params.value(QStringLiteral("toolCall")).toObject();

    ask.toolCallId = toolCall.value(QStringLiteral("toolCallId")).toString();

    QString tool = toolCall.value(QStringLiteral("title")).toString();
    if (tool.isEmpty()) {
        const QJsonObject meta = params.value(QStringLiteral("_meta")).toObject();
        tool = meta.value(QStringLiteral("claudeCode")).toObject()
                   .value(QStringLiteral("toolName"))
                   .toString();
    }
    if (tool.isEmpty())
        tool = toolCall.value(QStringLiteral("kind")).toString();
    ask.tool = tool.isEmpty() ? QStringLiteral("outil") : tool;

    ask.toolKind = toolCall.value(QStringLiteral("kind")).toString();
    ask.detail = compactInput(toolCall);
    ask.output = terminalOutput(toolCall);

    const QJsonArray options = params.value(QStringLiteral("options")).toArray();
    for (const QJsonValue value : options) {
        const QJsonObject option = value.toObject();
        const QString kind = option.value(QStringLiteral("kind")).toString();
        QVariantMap row;
        row.insert(QStringLiteral("optionId"), option.value(QStringLiteral("optionId")).toString());
        row.insert(QStringLiteral("name"), option.value(QStringLiteral("name")).toString());
        row.insert(QStringLiteral("kind"), kind);
        row.insert(QStringLiteral("allow"), optionRank(kind) == 0);
        ask.options.append(row);
    }

    // Le pont ne classe pas ses options : c'est la carte qui les groupe.
    std::stable_sort(ask.options.begin(), ask.options.end(), [](const QVariant &a, const QVariant &b) {
        return optionRank(a.toMap().value(QStringLiteral("kind")).toString())
             < optionRank(b.toMap().value(QStringLiteral("kind")).toString());
    });

    permissionAsks.append(ask);

    setStatus(QStringLiteral("autorisation demandee"));
    emit permissionChanged();
}

/*
 * Le pont vient de trancher ailleurs (la CLI Z Code, un autre client, ou le
 * moteur lui-meme) : la carte se retire sans repondre. Le `kind` de la demande
 * n'est pas seulement « permission » : un questionnaire tranche de meme.
 */
void HubClient::settleAsk(const QString &kind, const QString &toolCallId)
{
    bool changed = false;

    auto matches = [&toolCallId](const AskRequest &ask) {
        return !ask.toolCallId.isEmpty() && ask.toolCallId == toolCallId;
    };

    if (kind == QLatin1String("permission")) {
        for (int i = 0; i < permissionAsks.size(); ++i) {
            if (toolCallId.isEmpty() || matches(permissionAsks.at(i))) {
                permissionAsks.removeAt(i);
                changed = true;
                break;
            }
        }
    } else if (kind == QLatin1String("elicitation")) {
        for (int i = 0; i < elicitationAsks.size(); ++i) {
            if (toolCallId.isEmpty() || matches(elicitationAsks.at(i))) {
                elicitationAsks.removeAt(i);
                changed = true;
                break;
            }
        }
    }

    if (changed)
        emit permissionChanged();
}

void HubClient::clearPermissionAsks()
{
    if (permissionAsks.isEmpty() && elicitationAsks.isEmpty())
        return;
    permissionAsks.clear();
    elicitationAsks.clear();
    emit permissionChanged();
}

void HubClient::respondPermission(const QString &optionId)
{
    if (permissionAsks.isEmpty())
        return;

    const AskRequest ask = permissionAsks.takeFirst();

    QJsonObject outcome;
    if (optionId.isEmpty())
        outcome.insert(QStringLiteral("outcome"), QStringLiteral("cancelled"));
    else
        outcome.insert(QStringLiteral("outcome"), QStringLiteral("selected"));
    if (!optionId.isEmpty())
        outcome.insert(QStringLiteral("optionId"), optionId);

    QJsonObject reply;
    reply.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    reply.insert(QStringLiteral("id"), static_cast<qint64>(ask.rpcId));
    reply.insert(QStringLiteral("result"), QJsonObject{{QStringLiteral("outcome"), outcome}});

    bridge->sendTextMessage(QJsonDocument(reply).toJson());

    emit permissionChanged();
}

/*
 * Le raccourci clavier du refus : Échap doit porter le refus du moteur s'il en
 * propose un, pas une annulation qui le laisse choisir autre chose.
 */
QString HubClient::rejectOption() const
{
    if (permissionAsks.isEmpty())
        return QString();
    for (const QVariant &value : permissionAsks.first().options) {
        const QVariantMap option = value.toMap();
        if (optionRank(option.value(QStringLiteral("kind")).toString()) == 2)
            return option.value(QStringLiteral("optionId")).toString();
    }
    return QString();
}

// --- questionnaires (elicitation/create, mode form) ------------------------

namespace {

/*
 * Les champs du formulaire, dans l'ordre du schema. Le schema n'autorise que des
 * types primitifs : chaine (avec oneOf/enum = choix unique), tableau (anyOf =
 * choix multiple), booleen, nombre — et le champ libre compagnon q_i_other,
 * qui n'est pas une question de plus mais la reponse ecrite a la question qui
 * la precede (elle ecrase le choix simple, elle s'ajoute au choix multiple).
 */
QVariantList formFields(const QJsonObject &schema)
{
    QVariantList fields;
    const QJsonObject properties = schema.value(QStringLiteral("properties")).toObject();

    QSet<QString> required;
    const QJsonArray requiredList = schema.value(QStringLiteral("required")).toArray();
    for (const QJsonValue value : requiredList)
        required.insert(value.toString());

    // QJsonObject trie ses cles : q_i vient avant q_i_other, qui suit sa question.
    const QStringList keys = properties.keys();
    for (const QString &key : keys) {
        if (key.endsWith(QStringLiteral("_other")))
            continue;

        const QJsonObject prop = properties.value(key).toObject();
        const QString type = prop.value(QStringLiteral("type")).toString();
        QVariantList options;
        QString kind = QStringLiteral("text");

        QJsonArray listed;
        if (type == QLatin1String("array")) {
            kind = QStringLiteral("multi");
            listed = prop.value(QStringLiteral("items")).toObject()
                        .value(QStringLiteral("anyOf")).toArray();
        } else if (type == QLatin1String("string")
                   && (prop.contains(QStringLiteral("oneOf"))
                       || prop.contains(QStringLiteral("enum")))) {
            kind = QStringLiteral("single");
            listed = prop.contains(QStringLiteral("oneOf"))
                        ? prop.value(QStringLiteral("oneOf")).toArray()
                        : prop.value(QStringLiteral("enum")).toArray();
        } else if (type == QLatin1String("boolean")) {
            kind = QStringLiteral("boolean");
        } else if (type == QLatin1String("number") || type == QLatin1String("integer")) {
            kind = QStringLiteral("number");
        }

        for (const QJsonValue value : listed) {
            QVariantMap row;
            if (value.isString()) {
                row.insert(QStringLiteral("value"), value.toString());
                row.insert(QStringLiteral("label"), value.toString());
            } else {
                const QJsonObject option = value.toObject();
                QString label = option.value(QStringLiteral("title")).toString();
                const QString constant = option.value(QStringLiteral("const")).toString();
                if (label.isEmpty())
                    label = constant;
                row.insert(QStringLiteral("value"), constant);
                row.insert(QStringLiteral("label"), label);
                row.insert(QStringLiteral("description"),
                           option.value(QStringLiteral("description")).toString());
            }
            options.append(row);
        }

        QVariantMap field;
        field.insert(QStringLiteral("key"), key);
        QString title = prop.value(QStringLiteral("title")).toString();
        field.insert(QStringLiteral("title"), title.isEmpty() ? key : title);
        field.insert(QStringLiteral("help"), prop.value(QStringLiteral("description")).toString());
        field.insert(QStringLiteral("kind"), kind);
        field.insert(QStringLiteral("options"), options);
        field.insert(QStringLiteral("required"), required.contains(key));

        const QString companion = key + QStringLiteral("_other");
        if (properties.contains(companion)) {
            field.insert(QStringLiteral("otherKey"), companion);
            field.insert(QStringLiteral("otherTitle"),
                         properties.value(companion).toObject()
                             .value(QStringLiteral("title")).toString());
        }

        fields.append(field);
    }

    return fields;
}

// Le contenu rendu par la carte : des valeurs primitives, et des tableaux de
// chaines pour les choix multiples.
QJsonValue contentToJson(const QVariant &value)
{
    const int type = value.userType();
    if (type == QMetaType::QVariantList || type == QMetaType::QStringList) {
        QJsonArray array;
        for (const QVariant &item : value.toList())
            array.append(item.toString());
        return array;
    }
    if (type == QMetaType::Bool)
        return value.toBool();
    if (type == QMetaType::Int || type == QMetaType::LongLong || type == QMetaType::Double
        || type == QMetaType::UInt || type == QMetaType::ULongLong)
        return value.toDouble();
    return value.toString();
}

} // namespace

QString HubClient::elicitationMessage() const
{
    return elicitationAsks.isEmpty() ? QString() : elicitationAsks.first().tool;
}

QVariantList HubClient::elicitationFields() const
{
    return elicitationAsks.isEmpty() ? QVariantList() : elicitationAsks.first().fields;
}

void HubClient::showElicitationRequest(const QJsonObject &msg)
{
    AskRequest ask;
    ask.rpcId = msg.value(QStringLiteral("id")).toVariant().toLongLong();

    const QJsonObject params = msg.value(QStringLiteral("params")).toObject();
    ask.toolCallId = params.value(QStringLiteral("toolCallId")).toString();

    // Le message du pont ne repete pas les questions : chaque question est deja
    // le titre de son champ. On le garde comme bandeau de la carte.
    ask.tool = params.value(QStringLiteral("message")).toString();
    ask.toolKind = params.value(QStringLiteral("mode")).toString();
    ask.fields = formFields(params.value(QStringLiteral("requestedSchema")).toObject());

    elicitationAsks.append(ask);

    setStatus(QStringLiteral("le moteur attend une reponse"));
    emit permissionChanged();
}

void HubClient::respondElicitation(const QString &action, const QVariantMap &content)
{
    if (elicitationAsks.isEmpty())
        return;

    const AskRequest ask = elicitationAsks.takeFirst();

    QJsonObject result;
    result.insert(QStringLiteral("action"), action.isEmpty() ? QStringLiteral("cancel") : action);
    if (result.value(QStringLiteral("action")).toString() == QStringLiteral("accept")) {
        QJsonObject shaped;
        for (auto it = content.constBegin(); it != content.constEnd(); ++it)
            shaped.insert(it.key(), contentToJson(it.value()));
        if (!shaped.isEmpty())
            result.insert(QStringLiteral("content"), shaped);
    }

    QJsonObject reply;
    reply.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    reply.insert(QStringLiteral("id"), static_cast<qint64>(ask.rpcId));
    reply.insert(QStringLiteral("result"), result);

    bridge->sendTextMessage(QJsonDocument(reply).toJson());

    emit permissionChanged();
}

/*
 * Une requete du pont que ce corps ne sait pas rendre : on repond quand meme,
 * sinon le moteur reste suspendu sur une demande que personne ne fermera.
 */
void HubClient::rejectRequest(const QJsonObject &msg)
{
    QJsonObject error;
    error.insert(QStringLiteral("code"), -32601);
    error.insert(QStringLiteral("message"),
                 QStringLiteral("a-ice ne rend pas cette requete : ")
                     + msg.value(QStringLiteral("method")).toString());

    QJsonObject reply;
    reply.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    reply.insert(QStringLiteral("id"), msg.value(QStringLiteral("id")));
    reply.insert(QStringLiteral("error"), error);

    bridge->sendTextMessage(QJsonDocument(reply).toJson());
}

// --- etats ----------------------------------------------------------------

void HubClient::fail(const QString &what)
{
    setStatus(what);
    emit failed(what);
}

void HubClient::setStatus(const QString &text)
{
    if (statusText == text)
        return;
    statusText = text;
    emit statusChanged();
}

void HubClient::setGenerating(bool on)
{
    if (generating == on)
        return;
    generating = on;
    emit generatingChanged();
}
