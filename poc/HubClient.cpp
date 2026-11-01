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
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

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
        rpcSend(QStringLiteral("initialize"),
                QJsonObject{{QStringLiteral("protocolVersion"), 1},
                            {QStringLiteral("clientCapabilities"), QJsonObject{}}},
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

// --- reglages ---------------------------------------------------------------

/*
 * Le hub est celui que zcode-acp tient sur la machine : « remote » dans
 * ~/.config/zcode-acp/config.json (hubHost, hubPort, token). Le plan de la
 * refonte previsait un bloc « hub » dans ~/.config/a-ice/config.json : il est
 * lu en priorite, on bascule sur zcode-acp si il manque.
 * AICE_HUB / AICE_TOKEN / AICE_WORKSPACE écrasent tout.
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

    if (hubHttpBase.isEmpty() || token.isEmpty()) {
        const QJsonObject hub = readJsonFile(QDir::homePath() + "/.config/a-ice/config.json")
                                    .value("hub")
                                    .toObject();
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
        if (hubHttpBase.isEmpty() && !remote.isEmpty()) {
            hubHttpBase = QStringLiteral("http://%1:%2")
                              .arg(remote.value("hubHost").toString(QStringLiteral("127.0.0.1")))
                              .arg(remote.value("hubPort").toInt(8377));
        }
        if (token.isEmpty())
            token = remote.value("token").toString();
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
    instanceIdValue.clear();
    sessionIdValue.clear();
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
        if (msg.value(QStringLiteral("method")).toString()
            == QStringLiteral("session/request_permission"))
            showPermissionRequest(msg);
        else
            respondPermission(QString());
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

    // Une autre main (la CLI Z Code) a already tranche la demande : on retire
    // la notre sans repondre — le pont a deja sa reponse.
    if (method == QStringLiteral("$/zcode/ask_settled")) {
        if (permissionRpcId >= 0 && params.value(QStringLiteral("kind")).toString() == QStringLiteral("permission")) {
            permissionRpcId = -1;
            permissionToolValue.clear();
            permissionDetailValue.clear();
            permissionOptionsValue.clear();
            emit permissionChanged();
        }
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
    rememberSession();
    sessionStatusLine();
    emit turnEnded(stopReason);
}

// --- autorisations ---------------------------------------------------------

void HubClient::showPermissionRequest(const QJsonObject &msg)
{
    permissionRpcId = msg.value(QStringLiteral("id")).toVariant().toLongLong();

    const QJsonObject params = msg.value(QStringLiteral("params")).toObject();
    const QJsonObject toolCall = params.value(QStringLiteral("toolCall")).toObject();

    QString tool = toolCall.value(QStringLiteral("title")).toString();
    if (tool.isEmpty()) {
        const QJsonObject meta = params.value(QStringLiteral("_meta")).toObject();
        tool = meta.value(QStringLiteral("claudeCode")).toObject()
                   .value(QStringLiteral("toolName"))
                   .toString();
    }
    permissionToolValue = tool.isEmpty() ? QStringLiteral("outil") : tool;

    const QJsonValue raw = toolCall.contains(QStringLiteral("rawInput"))
                             ? toolCall.value(QStringLiteral("rawInput"))
                             : QJsonValue(toolCall);
    permissionDetailValue = QString::fromUtf8(
        QJsonDocument(raw.toObject()).toJson(QJsonDocument::Compact)).left(400);

    permissionOptionsValue.clear();
    const QJsonArray options = params.value(QStringLiteral("options")).toArray();
    for (const QJsonValue value : options) {
        const QJsonObject option = value.toObject();
        QVariantMap row;
        row.insert(QStringLiteral("optionId"), option.value(QStringLiteral("optionId")).toString());
        row.insert(QStringLiteral("name"), option.value(QStringLiteral("name")).toString());
        row.insert(QStringLiteral("kind"), option.value(QStringLiteral("kind")).toString());
        permissionOptionsValue.append(row);
    }

    setStatus(QStringLiteral("autorisation demandee"));
    emit permissionChanged();
}

void HubClient::respondPermission(const QString &optionId)
{
    if (permissionRpcId < 0)
        return;

    QJsonObject outcome;
    if (optionId.isEmpty())
        outcome.insert(QStringLiteral("outcome"), QStringLiteral("cancelled"));
    else
        outcome.insert(QStringLiteral("outcome"), QStringLiteral("selected"));
    if (!optionId.isEmpty())
        outcome.insert(QStringLiteral("optionId"), optionId);

    QJsonObject reply;
    reply.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    reply.insert(QStringLiteral("id"), permissionRpcId);
    reply.insert(QStringLiteral("result"), QJsonObject{{QStringLiteral("outcome"), outcome}});

    bridge->sendTextMessage(QJsonDocument(reply).toJson());

    permissionRpcId = -1;
    permissionToolValue.clear();
    permissionDetailValue.clear();
    permissionOptionsValue.clear();
    emit permissionChanged();
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
