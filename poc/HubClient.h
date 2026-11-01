// a-ice 2.0 — M2 : le client du hub zcode-acp, cote POC.
//
// Le corps ne connait qu'une chose : le protocole du hub.
//   HTTP  GET   /api/instances                 → l'instance live du workspace
//   HTTP  POST  /api/instances {workspacePath} → incubation si aucune instance live
//   WS    /acp?instance=<id>                   → initialize, session/load (reprise) ou
//                                               session/new, session/prompt, session/update,
//                                               session/request_permission
// Tout est asynchrone : le POC reste libre de dessiner pendant que le tour tourne.
//
// Gestion de session (M3) :
//   - la session vecue est gardeée dans QSettings (cle par workspace) : au lancement
//     suivant on la charge par session/load, et le pont rejoue l historique.
//   - le premier session/new d une connexion herite parfois du dernier fil lie a
//     l instance, meme perime : un prompt qui repond « no longer exists in the
//     backend » n y fait pas mourir le corps, il mint une session neuve relance le
//     tour.

#pragma once

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>
#include <QVariantList>

class QNetworkAccessManager;
class QWebSocket;
class QTimer;

class HubClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool ready READ isReady NOTIFY readyChanged)
    Q_PROPERTY(bool resumed READ isResumed NOTIFY resumedChanged)
    Q_PROPERTY(bool generating READ isGenerating NOTIFY generatingChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString instanceId READ instanceId NOTIFY statusChanged)
    Q_PROPERTY(QString sessionId READ sessionId NOTIFY statusChanged)
    Q_PROPERTY(QString workspace READ workspaceName NOTIFY statusChanged)
    Q_PROPERTY(QString toolLine READ toolLine NOTIFY toolLineChanged)
    Q_PROPERTY(bool permissionPending READ isPermissionPending NOTIFY permissionChanged)
    Q_PROPERTY(QString permissionTool READ permissionTool NOTIFY permissionChanged)
    Q_PROPERTY(QString permissionDetail READ permissionDetail NOTIFY permissionChanged)
    Q_PROPERTY(QVariantList permissionOptions READ permissionOptions NOTIFY permissionChanged)

public:
    explicit HubClient(QObject *parent = nullptr);

    bool isReady() const { return ready; }
    bool isResumed() const { return resumed; }
    bool isGenerating() const { return generating; }
    bool isPermissionPending() const { return permissionRpcId >= 0; }

    QString status() const { return statusText; }
    QString instanceId() const { return instanceIdValue; }
    QString sessionId() const { return sessionIdValue; }
    QString workspaceName() const { return workspace; }

    // Quel dossier du hub ce corps veut-il faire parler ? (--workspace)
    void setWorkspace(const QString &path) { cliWorkspace = path; }
    QString toolLine() const { return toolLineValue; }
    QString permissionTool() const { return permissionToolValue; }
    QString permissionDetail() const { return permissionDetailValue; }
    QVariantList permissionOptions() const { return permissionOptionsValue; }

    // Rejoindre le hub : instance (réutilisée ou incubée), puis session ACP.
    Q_INVOKABLE void connectHub();
    Q_INVOKABLE void disconnectHub();

    // Un tour = un prompt. Les chunks reviennent par chunkReceived().
    Q_INVOKABLE void send(const QString &text);
    Q_INVOKABLE void cancelTurn();

    // Prompt en attente (test M2 : --prompt) : il part des que la session est ouverte.
    void setPrompt(const QString &text) { pendingPrompt = text; }

    // Reponse a une demande d'autorisation : optionId choisi, ou annule.
    Q_INVOKABLE void respondPermission(const QString &optionId);

    // Repartir d une conversation vierge : on oublie la session gardeée.
    Q_INVOKABLE void startFreshConversation();

signals:
    // Un chunk, avec son message : le meme messageId designe la meme bulle (le
    // texte qui coule), un messageId nouveau ouvre une bulle (reprise d historique).
    void chunkReceived(const QString &messageId, const QString &role, const QString &text);
    void turnEnded(const QString &stopReason);
    void failed(const QString &message);

    void readyChanged();
    void resumedChanged();
    void generatingChanged();
    void statusChanged();
    void toolLineChanged();
    void permissionChanged();

private:
    // Les etapes de la prise de contact, dans l'ordre.
    enum Stage { Idle, ListingInstances, CreatingInstance, ConnectingBridge, Live };

    void loadSettings();
    void resolveWorkspace();
    void requestInstances();
    void createInstance();
    void openBridge();
    void startSession();
    void mintSession();
    void forgetStoredSession();
    void rememberSession();
    QString sessionKey() const;
    void restoreStoredSession();
    void rpcSend(const QString &method, const QJsonObject &params, bool promptTurn);
    void onBridgeText(const QString &message);
    void onBridgeNotification(const QJsonObject &msg);
    void showPermissionRequest(const QJsonObject &msg);
    void verifySessionFreshness();
    void openSessionReady();
    void endTurn(const QString &stopReason);
    void sessionStatusLine();
    void onBridgeError();
    void setStatus(const QString &text);
    void fail(const QString &what);
    void setGenerating(bool on);

    QNetworkAccessManager *net = nullptr;
    QWebSocket *bridge = nullptr;
    QTimer *retry = nullptr;

    QString hubHttpBase;      // http://127.0.0.1:8377
    QString hubWsBase;        // ws://127.0.0.1:8377
    QString token;
    QString workspace;

    QString instanceIdValue;
    QString sessionIdValue;
    QString pendingPrompt;
    QString statusText;
    QString toolLineValue;
    QString permissionToolValue;
    QString permissionDetailValue;
    QVariantList permissionOptionsValue;

    QString cliWorkspace;        // --workspace : le dossier vise par ce lancement
    QString storedSessionValue;  // session vecue, retrouvee dans QSettings
    QString retryPromptText;    // le tour relance si la session etait perimeee
    int promptRecoveries = 0;

    qint64 nextRpcId = 1;
    qint64 permissionRpcId = -1;
    int sessionNewTries = 0;
    QMap<qint64, QString> pendingMethod;   // id de requete -> methode, pour trier les reponses

    Stage stage = Idle;
    bool ready = false;
    bool generating = false;
    bool voluntaryClose = false;
    bool resumed = false;
};
