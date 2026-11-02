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
#include <QVector>

class QNetworkAccessManager;
class QWebSocket;
class QTimer;

/*
 * Une demande du moteur qui attend notre reponse : autorisation (permission) ou,
 * plus tard, questionnaire (elicitation). Le pont diffuse la demande a toutes ses
 * connexions et garde la premiere reponse ; chacune de nos demandes doit donc
 * pouvoir etre retiree toute seule quand une autre main a tranche.
 *
 * Les champs sont ceux du protocole, tels quels : les libelles d'options viennent
 * du moteur, on ne les reecrit pas — et on ne renvoie jamais qu'un optionId, sinon
 * le pont recolle une decision one-shot a une demande qui voulait etre durable.
 */
struct AskRequest
{
    qint64 rpcId = -1;
    QString toolCallId;   // sert a retirer la bonne carte quand une autre main tranche
    QString tool;         // titre de l'outil, tel que le moteur le nomme
    QString toolKind;     // execute | read | edit | search | fetch | other
    QString detail;       // l'entree de l'outil (rawInput)
    QString output;       // la sortie de terminal, quand le moteur la joint deja
    QVariantList options; // permission : { optionId, name, kind, allow }
    QVariantList fields;  // elicitation : { key, title, help, kind, options, otherKey, otherTitle }
};

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
    Q_PROPERTY(QString permissionKind READ permissionKind NOTIFY permissionChanged)
    Q_PROPERTY(QString permissionDetail READ permissionDetail NOTIFY permissionChanged)
    Q_PROPERTY(QString permissionOutput READ permissionOutput NOTIFY permissionChanged)
    Q_PROPERTY(QVariantList permissionOptions READ permissionOptions NOTIFY permissionChanged)
    // File d'attente : un moteur peut demander plusieurs autorisations d'affilée.
    Q_PROPERTY(int askCount READ askCount NOTIFY permissionChanged)
    Q_PROPERTY(int askIndex READ askIndex NOTIFY permissionChanged)
    Q_PROPERTY(QVariantList sessions READ sessions NOTIFY sessionsChanged)
    // Le questionnaire du moteur (elicitation/create, mode form) : une carte, un
    // champ par question. La file de demandes se suit par askCount/askIndex.
    Q_PROPERTY(bool elicitationPending READ isElicitationPending NOTIFY permissionChanged)
    Q_PROPERTY(QString elicitationMessage READ elicitationMessage NOTIFY permissionChanged)
    Q_PROPERTY(QVariantList elicitationFields READ elicitationFields NOTIFY permissionChanged)
    // M3d — ce que le moteur DECLARE, et rien d'autre : les commandes slash qu il
    // publie (available_commands_update) et les reglages de session qu il tient
    // (configOptions : modele, mode, pensee). Aucun nom n est durci ici.
    Q_PROPERTY(QVariantList commands READ commands NOTIFY commandsChanged)
    Q_PROPERTY(QVariantList configOptions READ configOptions NOTIFY configOptionsChanged)

public:
    explicit HubClient(QObject *parent = nullptr);

    bool isReady() const { return ready; }
    bool isResumed() const { return resumed; }
    bool isGenerating() const { return generating; }
    bool isPermissionPending() const { return !permissionAsks.isEmpty(); }
    bool isElicitationPending() const { return !elicitationAsks.isEmpty(); }

    // Combien de demandes attendent, et laquelle est montree (carte « demande 2/3 »).
    int askCount() const;
    int askIndex() const;

    QString status() const { return statusText; }
    QString instanceId() const { return instanceIdValue; }
    QString sessionId() const { return sessionIdValue; }
    QString workspaceName() const { return workspace; }

    // Quel dossier du hub ce corps veut-il faire parler ? (--workspace)
    void setWorkspace(const QString &path) { cliWorkspace = path; }
    QString toolLine() const { return toolLineValue; }
    QString permissionTool() const;
    QString permissionKind() const;
    QString permissionDetail() const;
    QString permissionOutput() const;
    QVariantList permissionOptions() const;
    QString elicitationMessage() const;
    QVariantList elicitationFields() const;
    // Les sessions du hub, toutes instances dehors (carte hub, M3) : {sessionId,
    // title, state, workspace, workspacePath} par session vivante.
    QVariantList sessions() const { return sessionsValue; }

    // Commandes slash publiees par le pont : {name, description, hint}.
    QVariantList commands() const { return commandsValue; }
    // Reglages de session publies par le pont : {id, name, currentValue,
    // options: [{value, name}]} — l id est ce que regoit setConfigOption().
    QVariantList configOptions() const { return configOptionsValue; }

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
    // L'option de refus a presenter au clavier (Échap), vide s'il n'y en a pas.
    Q_INVOKABLE QString rejectOption() const;

    // Reponse a un questionnaire : « accept » + le contenu rendu conforme au
    // schema, « decline », ou « cancel » (rien n'est obligatoire cote moteur).
    Q_INVOKABLE void respondElicitation(const QString &action, const QVariantMap &content);

    // Repartir d une conversation vierge : on oublie la session gardeée.
    Q_INVOKABLE void startFreshConversation();

    // Regler la session (M3d) : modele, mode ou niveau de pensee. La valeur est
    // celle que le pont a publiee dans configOptions — jamais une valeur inventee,
    // le pont refuse autrement (et un echec se dit dans le statut).
    Q_INVOKABLE void setConfigOption(const QString &configId, const QString &value);
    // La valeur courante d un reglage ("" si le pont ne l a pas encore publiee).
    Q_INVOKABLE QString configValue(const QString &configId) const;
    // Le libelle le plus court qui designe cette valeur (pour l affichage).
    Q_INVOKABLE QString configLabel(const QString &configId) const;

    // Multi-sessions : la liste des sessions du hub, et l'ouverture de l'une
    // d'elles (session/load, l'historique rejoue comme a la reprise).
    Q_INVOKABLE void listSessions();
    Q_INVOKABLE void openSession(const QString &sessionId, const QString &workspacePath);

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
    void sessionsChanged();
    // La conversation change de session : le modele de bulles doit se vider
    // avant que l historique rejoue n arrive.
    void historyReset();
    // Le moteur a publie ses commandes ou ses reglages : l autocompletion doit suivre.
    void commandsChanged();
    void configOptionsChanged();

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
    void showElicitationRequest(const QJsonObject &msg);
    void collectCommands(const QJsonArray &published);
    void collectConfigOptions(const QJsonArray &published);
    // Lectures des reglages publies, pour le reglage lui-meme et l autocompletion.
    QVariantList publishedOptions(const QString &configId) const;
    QString labelFor(const QVariantList &choices, const QString &value) const;
    void rejectRequest(const QJsonObject &msg);
    void settleAsk(const QString &kind, const QString &toolCallId);
    void clearPermissionAsks();
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

    QString cliWorkspace;        // --workspace : le dossier vise par ce lancement
    QString storedSessionValue;  // session vecue, retrouvee dans QSettings
    QString pendingOpenSession;  // session demande par openSession(), a charger
    QString openWorkspacePath;   // le workspace cible de cette ouverture
    QString retryPromptText;    // le tour relance si la session etait perimeee
    QVariantList sessionsValue;
    QVariantList commandsValue;
    QVariantList configOptionsValue;
    int promptRecoveries = 0;

    qint64 nextRpcId = 1;
    // Les demandes en attente, la premiere en tete : c'est elle que la carte montre.
    QVector<AskRequest> permissionAsks;
    QVector<AskRequest> elicitationAsks;
    int sessionNewTries = 0;
    QMap<qint64, QString> pendingMethod;   // id de requete -> methode, pour trier les reponses

    Stage stage = Idle;
    bool ready = false;
    bool generating = false;
    bool voluntaryClose = false;
    bool resumed = false;
};
