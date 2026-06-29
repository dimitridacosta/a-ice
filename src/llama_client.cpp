#include "llama_client.h"
#include <QUrl>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QDebug>
#include "ChatMessage.h"

LlamaClient::LlamaClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

void LlamaClient::setServerUrl(const QString &url)
{
    m_serverUrl = url;
    if (!m_serverUrl.isEmpty() && !m_serverUrl.endsWith('/')) {
        m_serverUrl += '/';
    }
}

void LlamaClient::setConfig(const Config &config)
{
    m_config = config;
    setServerUrl(config.apiUrl());
    qInfo() << "[a-ice] config: provider=" << m_config.provider().name
            << "api_url=" << m_config.apiUrl()
            << "model=" << m_config.model().name
            << "temp=" << m_config.model().temperature
            << "max_tokens=" << m_config.model().maxTokens
            << "stream=" << m_config.model().stream;
}

void LlamaClient::sendMessages(const QList<ChatMessage> &messages)
{
    if (m_isRequesting) {
        emit requestError(QStringLiteral("Déjà en cours..."));
        return;
    }
    m_isRequesting = true;
    m_sseBuffer.clear();
    m_inThinkingContent = false;
    makeRequest(messages);
}

void LlamaClient::cancel()
{
    if (m_currentReply) {
        m_currentReply->abort();
        // abort() peut déclencher finished() de manière synchrone : le lambda
        // connecté met alors m_currentReply à nullptr. On re-vérifie avant
        // d'appeler deleteLater() pour éviter un null-deref → crash.
        if (m_currentReply) {
            m_currentReply->deleteLater();
            m_currentReply = nullptr;
        }
    }
    m_isRequesting = false;
    m_sseBuffer.clear();
}

void LlamaClient::makeRequest(const QList<ChatMessage> &messages)
{
    QJsonObject json;
    QJsonArray messagesArray;

    // Prompt système (SOUL.md) injecté en tête, role="system".
    const QString sys = m_config.systemPrompt();
    if (!sys.trimmed().isEmpty()) {
        QJsonObject sysObj;
        sysObj["role"] = QStringLiteral("system");
        sysObj["content"] = sys;
        messagesArray.append(sysObj);
    }

    for (const auto &msg : messages) {
        QJsonObject msgObj;
        msgObj["role"] = msg.role;
        msgObj["content"] = msg.content;
        messagesArray.append(msgObj);
    }

    json["messages"] = messagesArray;
    json["model"] = m_config.model().name;
    json["temperature"] = m_config.model().temperature;
    json["max_tokens"] = m_config.model().maxTokens;
    json["stream"] = m_config.model().stream;

    QString url = m_serverUrl + "chat/completions";
    QUrl requestUrl(url);
    QNetworkRequest qnaRequest(requestUrl);
    qnaRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QByteArray jsonStr = QJsonDocument(json).toJson(QJsonDocument::Compact);
    qInfo() << "[a-ice] → POST" << url << "(stream=" << m_config.model().stream << ")";
    qInfo().noquote() << "[a-ice] payload:" << jsonStr;

    m_currentReply = m_nam->post(qnaRequest, jsonStr);

    if (m_config.model().stream) {
        // Streaming SSE : on consomme au fil de l'eau via readyRead.
        connect(m_currentReply, &QIODevice::readyRead, this, [this]() {
            if (!m_currentReply) return;
            parseSseChunk(m_currentReply->readAll());
        });
        connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
            if (!m_currentReply) return;

            // Flush du buffer restant (au cas où le dernier chunk n'avait pas de \n\n).
            if (!m_sseBuffer.isEmpty()) {
                handleStreamLine(m_sseBuffer);
                m_sseBuffer.clear();
            }

            QNetworkReply::NetworkError err = m_currentReply->error();
            int httpCode = m_currentReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

            if (err == QNetworkReply::OperationCanceledError) {
                // Annulation utilisateur : onStopClicked() gère le reset, on
                // n'émet rien (évite double reset + crash si synchrone).
            } else if (err != QNetworkReply::NoError) {
                QString msg = QStringLiteral("Réseau: %1 (HTTP %2)")
                                  .arg(m_currentReply->errorString())
                                  .arg(httpCode);
                qWarning() << "[a-ice] error:" << msg;
                emit requestError(msg);
            } else {
                qInfo() << "[a-ice] ← flux terminé (HTTP" << httpCode << ")";
                emit responseComplete();
            }

            m_currentReply->deleteLater();
            m_currentReply = nullptr;
            m_isRequesting = false;
        });
    } else {
        // Mode non-streaming : on lit tout à la fin (comportement historique).
        connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
            if (!m_currentReply) return;

            QNetworkReply::NetworkError err = m_currentReply->error();
            QByteArray body = m_currentReply->readAll();
            int httpCode = m_currentReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            qInfo() << "[a-ice] ← HTTP" << httpCode << "(" << body.size() << "bytes)";

            if (err == QNetworkReply::OperationCanceledError) {
                // Annulation utilisateur : on ne fait rien.
            } else if (err != QNetworkReply::NoError) {
                QString msg = QStringLiteral("Réseau: %1").arg(m_currentReply->errorString());
                qWarning() << "[a-ice] error:" << msg;
                emit requestError(msg);
            } else {
                QString text = parseChatCompletion(body);
                if (text.isNull()) {
                    emit requestError(QStringLiteral("Réponse invalide: %1").arg(QString::fromUtf8(body)));
                } else {
                    if (!text.isEmpty()) emit contentChunk(text);
                    emit responseComplete();
                }
            }

            m_currentReply->deleteLater();
            m_currentReply = nullptr;
            m_isRequesting = false;
        });
    }
}

void LlamaClient::parseSseChunk(const QByteArray &data)
{
    m_sseBuffer.append(data);

    // Un événement SSE se termine par "\n\n". On découpe le buffer en lignnes
    // complètes et on garde le reste en buffer.
    int idx;
    while ((idx = m_sseBuffer.indexOf("\n\n")) != -1) {
        QByteArray line = m_sseBuffer.left(idx);
        m_sseBuffer = m_sseBuffer.mid(idx + 2);
        handleStreamLine(line);
    }
}

void LlamaClient::handleStreamLine(const QByteArray &line)
{
    // Format: "data: {...}"  ou  "data: [DONE]"
    const QByteArray prefix("data: ");
    if (!line.startsWith(prefix)) {
        return;
    }

    QByteArray payload = line.mid(prefix.size());
    payload = payload.trimmed();
    if (payload.isEmpty()) return;

    if (payload == "[DONE]") {
        return; // le finished() s'occupera de responseComplete()
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "[a-ice] chunk JSON invalide:" << parseError.errorString();
        return;
    }

    const QJsonObject root = doc.object();
    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) return;

    const QJsonObject delta = choices.at(0).toObject()
                                  .value(QStringLiteral("delta")).toObject();

    // --- reasoning_content (champ dédié, ex. Qwen3 via OpenRouter / llama.cpp récent) ---
    if (delta.contains(QStringLiteral("reasoning_content"))) {
        QString t = delta.value(QStringLiteral("reasoning_content")).toString();
        // Certains serveurs envoient </think> dans reasoning_content comme séparateur.
        if (t.endsWith(QStringLiteral("</think>")))
            t.chop(8);
        if (!t.isEmpty()) emit thinkingChunk(t);
    }

    // --- content (réponse finale, ou <think>...</think> pour les serveurs anciens) ---
    if (delta.contains(QStringLiteral("content"))) {
        QString t = delta.value(QStringLiteral("content")).toString();
        if (t.isEmpty()) return;

        if (m_inThinkingContent) {
            // On était dans un bloc <think> transmis via content.
            int closeIdx = t.indexOf(QStringLiteral("</think>"));
            if (closeIdx != -1) {
                // Partie avant </think> : encore du thinking.
                if (closeIdx > 0)
                    emit thinkingChunk(t.left(closeIdx));
                m_inThinkingContent = false;
                // Partie après </think> : vraie réponse.
                QString rest = t.mid(closeIdx + 8).trimmed();
                if (!rest.isEmpty()) emit contentChunk(rest);
            } else {
                emit thinkingChunk(t);
            }
        } else if (t.startsWith(QStringLiteral("<think>"))) {
            // Nouveau bloc thinking démarré dans content.
            m_inThinkingContent = true;
            QString after = t.mid(7);
            int closeIdx = after.indexOf(QStringLiteral("</think>"));
            if (closeIdx != -1) {
                // Bloc complet en un seul chunk.
                if (closeIdx > 0) emit thinkingChunk(after.left(closeIdx));
                m_inThinkingContent = false;
                QString rest = after.mid(closeIdx + 8).trimmed();
                if (!rest.isEmpty()) emit contentChunk(rest);
            } else if (!after.isEmpty()) {
                emit thinkingChunk(after);
            }
        } else {
            // Contenu normal : on filtre les </think> parasites
            // (certains serveurs les envoient comme premier chunk de content).
            if (t.startsWith(QStringLiteral("</think>")))
                t = t.mid(8).trimmed();
            if (!t.isEmpty()) emit contentChunk(t);
        }
    }
}

QString LlamaClient::parseChatCompletion(const QByteArray &body)
{
    // Utilisé uniquement en mode non-streaming.
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "[a-ice] JSON invalide:" << parseError.errorString();
        return {};
    }

    const QJsonObject root = doc.object();
    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        qWarning() << "[a-ice] pas de choices";
        return {};
    }

    const QJsonObject message = choices.at(0).toObject()
                                    .value(QStringLiteral("message")).toObject();

    QString content   = message.value(QStringLiteral("content")).toString();
    QString reasoning = message.value(QStringLiteral("reasoning_content")).toString();

    // Si reasoning_content est absent, on tente d'extraire <think>...</think>
    // du champ content (llama.cpp sans champ dédié).
    if (reasoning.isEmpty() && content.contains(QStringLiteral("<think>"))) {
        static const QRegularExpression thinkRe(
            QStringLiteral("<think>(.*?)</think>\\s*"),
            QRegularExpression::DotMatchesEverythingOption);
        const auto match = thinkRe.match(content);
        if (match.hasMatch()) {
            reasoning = match.captured(1).trimmed();
            content   = content.mid(match.capturedEnd(0)).trimmed();
        }
    }

    // Émet le thinking si présent (non-streaming : un seul gros chunk).
    if (!reasoning.isEmpty())
        emit thinkingChunk(reasoning);

    if (content.isEmpty() && !reasoning.isEmpty())
        return reasoning; // modèle thinking-only
    return content;
}