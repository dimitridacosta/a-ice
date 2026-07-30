#pragma once

#include <QObject>
#include <QDateTime>
#include <QNetworkReply>
#include <QString>
#include <QList>
#include <QMap>
#include <functional>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QByteArray>
#include <QSet>
#include "Config.h"

class ChatMessage;
class QJsonArray;
struct ToolCall;

/**
 * Client OpenAI-compatible vers llama.cpp.
 *
 * Supporte le streaming (SSE) : émet thinkingChunk() et contentChunk() au fil
 * de l'eau, puis responseComplete() ou requestError() à la fin.
 *
 * Supporte aussi le function-calling : si la réponse contient des tool_calls,
 * toolCallsReady() est émis à la place de responseComplete() (le caller doit
 * exécuter les tools puis relancer sendMessages avec les résultats).
 */
class LlamaClient : public QObject
{
    Q_OBJECT

public:
    explicit LlamaClient(QObject *parent = nullptr);
    ~LlamaClient() override = default;

    void setServerUrl(const QString &url);
    QString serverUrl() const { return m_serverUrl; }

    void setConfig(const Config &config);
    const Config &config() const { return m_config; }

    /// Définit la liste des tools (JSON array OpenAI). Vide = pas de function calling.
    void setTools(const QJsonArray &tools);
    QJsonArray tools() const { return m_tools; }

    void sendMessages(const QList<ChatMessage> &messages);
    void cancel();

    /// Demande une compaction du contexte : envoie `messages` (typiquement
    /// [system=prompt de compaction, user=conversation à résumer]) en mode
    /// non-stream, SANS le SOUL/tools/contextSummary, et émet summaryReady()
    /// avec le résumé produit. Utilisé par /compact.
    void requestSummary(const QList<ChatMessage> &messages);
    /// Résumé de contexte compacté à injecter en system prompt (après le SOUL)
    /// pour les requêtes normales. Vide = pas de contexte compacté.
    void setContextSummary(const QString &s) { m_contextSummary = s; }
    QString contextSummary() const { return m_contextSummary; }

    /// Estime le nombre de tokens du contexte qui serait envoyé au modèle pour
    /// `messages` : system prompt (SOUL + tools desc + contextSummary) + tous
    /// les messages. Heuristique chars/4 (pas de tokenizer embarqué) — utile
    /// pour l'aperçu /context, pas une mesure exacte.
    int estimateContextTokens(const QList<ChatMessage> &messages) const;

    /// Met à jour le contexte temporel (date/heure) injecté en system prompt,
    /// mais seulement si > 15 min depuis le dernier rappel (ou jour changé, ou
    /// premier envoi). Minimise les tokens : juste l'heure, date si le jour a
    /// changé. Évite de réinjecter l'heure à chaque message.
    void refreshTimeContext();

signals:
    /// Morceau de raisonnement (modèles thinking type Qwen3).
    void thinkingChunk(const QString &text);
    /// Morceau de réponse finale.
    void contentChunk(const QString &text);
    /// Flux terminé normalement (pas de tool_calls).
    void responseComplete();
    /// La réponse contient des tool_calls à exécuter. `assistantContent` est
    /// le texte éventuel (souvent vide) accompagnant les tool_calls.
    void toolCallsReady(const QList<ToolCall> &calls, const QString &assistantContent);
    /// Met à jour le texte de la thinking (après extraction de tool_calls inline
    /// du reasoning : on retire les blocs tool_call du thinking affiché).
    void thinkingUpdated(const QString &cleanedThinking);
    /// Erreur réseau / HTTP / parsing.
    void requestError(const QString &error);
    /// Résumé de contexte produit par requestSummary() (commande /compact).
    void summaryReady(const QString &summary);

private:
    void makeRequest(const QList<ChatMessage> &messages);
    void parseSseChunk(const QByteArray &data);
    void handleStreamLine(const QByteArray &line);
    QString parseChatCompletion(const QByteArray &body, QList<ToolCall> *outToolCalls = nullptr);
    /// Accumule un morceau de tool_call streaming (delta.tool_calls[]).
    void consumeToolCallDelta(const QJsonArray &deltaCalls);
    /// Génère une description textuelle des tools pour le system prompt
    /// (aide le modèle à savoir qu'il a des tools, complément du champ `tools`).
    QString buildToolsSystemPrompt() const;
    /// Extrait les tool_calls inline d'un texte (reasoning ou content).
    /// Retourne les tool_calls trouvés et modifie `text` pour retirer les blocs.
    QList<ToolCall> extractInlineToolCalls(QString &text) const;

    QString m_serverUrl;
    Config m_config;
    QJsonArray m_tools;
    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_currentReply = nullptr;
    bool m_isRequesting = false;
    bool m_inThinkingContent = false;
    bool m_summarizing = false;     // mode compaction : non-stream, émet summaryReady
    QString m_contextSummary;      // résumé compacté injecté en system prompt
    QDateTime m_lastTimeReminder;   // dernier rappel date/heure (anti-réinjection < 15min)
    QString m_timeContext;          // contexte temporel courant injecté en system

    // Buffer SSE : les chunks peuvent arriver coupés au milieu d'une ligne.
    QByteArray m_sseBuffer;

    // Accumulation des tool_calls pendant le stream (index → ToolCall).
    QMap<int, ToolCall> m_pendingToolCalls;
    // Contenu assistant éventuel cumulé pendant le stream avec tool_calls.
    QString m_pendingAssistantContent;
    // Accumulation du reasoning_content pendant le stream (pour détecter
    // les tool_calls inline que certains modèles émettent dans leur thinking).
    QString m_streamedReasoning;
    // Accumulation du content pendant le stream (pour détecter les tool_calls
    // inline que certains modèles émettent dans leur réponse texte).
    QString m_streamedContent;

    // Noms de tools connus (rempli dans setTools). Sert à filtrer les faux
    // positifs d'extraction inline : un bloc {"name":...,"arguments":...}
    // dont le name n'est pas un tool réel est traité comme de la prose et
    // n'est PAS extrait. Voir ROADMAP item 10.
    QSet<QString> m_knownToolNames;
};