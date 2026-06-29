#pragma once

#include <QObject>
#include <QNetworkReply>
#include <QString>
#include <QList>
#include <functional>
#include <QJsonObject>
#include <QJsonDocument>
#include <QByteArray>
#include "Config.h"

class ChatMessage;

/**
 * Client OpenAI-compatible vers llama.cpp.
 *
 * Supporte le streaming (SSE) : émet thinkingChunk() et contentChunk() au fil
 * de l'eau, puis responseComplete() ou requestError() à la fin.
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

    void sendMessages(const QList<ChatMessage> &messages);
    void cancel();

signals:
    /// Morceau de raisonnement (modèles thinking type Qwen3).
    void thinkingChunk(const QString &text);
    /// Morceau de réponse finale.
    void contentChunk(const QString &text);
    /// Flux terminé normalement.
    void responseComplete();
    /// Erreur réseau / HTTP / parsing.
    void requestError(const QString &error);

private:
    void makeRequest(const QList<ChatMessage> &messages);
    void parseSseChunk(const QByteArray &data);
    void handleStreamLine(const QByteArray &line);
    QString parseChatCompletion(const QByteArray &body) const;

    QString m_serverUrl;
    Config m_config;
    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_currentReply = nullptr;
    bool m_isRequesting = false;

    // Buffer SSE : les chunks peuvent arriver coupés au milieu d'une ligne.
    QByteArray m_sseBuffer;
};