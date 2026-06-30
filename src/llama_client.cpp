#include "llama_client.h"
#include "ChatMessage.h"
#include <QUrl>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QDebug>

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
            << "stream=" << m_config.model().stream
            << "tools=" << m_tools.size();
}

void LlamaClient::setTools(const QJsonArray &tools)
{
    m_tools = tools;
    // Construit l'ensemble des noms de tools connus : sert de filtre
    // anti-faux-positif dans extractInlineToolCalls (item 10). Un bloc
    // {"name":...,"arguments":...} inline dont le name n'est pas un tool
    // réel est traité comme de la prose et n'est pas extrait.
    m_knownToolNames.clear();
    for (const QJsonValue &entry : tools) {
        const QJsonObject fn = entry.toObject()
            .value(QStringLiteral("function")).toObject();
        const QString name = fn.value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
            m_knownToolNames.insert(name);
    }
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
    m_pendingToolCalls.clear();
    m_pendingAssistantContent.clear();
    m_streamedReasoning.clear();
    m_streamedContent.clear();
    makeRequest(messages);
}

void LlamaClient::cancel()
{
    if (m_currentReply) {
        m_currentReply->abort();
        if (m_currentReply) {
            m_currentReply->deleteLater();
            m_currentReply = nullptr;
        }
    }
    m_isRequesting = false;
    m_sseBuffer.clear();
    m_pendingToolCalls.clear();
    m_pendingAssistantContent.clear();
    m_streamedReasoning.clear();
    m_streamedContent.clear();
}

void LlamaClient::makeRequest(const QList<ChatMessage> &messages)
{
    QJsonObject json;
    QJsonArray messagesArray;

    // Prompt système (SOUL.md) injecté en tête, role="system".
    // Si des tools sont déclarés, on enrichit le system prompt avec leur
    // description (fallback quand le chat template du GGUF ne gère pas le
    // champ `tools` du payload OpenAI — c'est le cas de qwen36-28b-reap).
    QString sys = m_config.systemPrompt();
    if (!m_tools.isEmpty()) {
        QString toolsPrompt = buildToolsSystemPrompt();
        if (!toolsPrompt.isEmpty()) {
            if (!sys.trimmed().isEmpty())
                sys += QStringLiteral("\n\n");
            sys += toolsPrompt;
        }
    }
    if (!sys.trimmed().isEmpty()) {
        QJsonObject sysObj;
        sysObj["role"] = QStringLiteral("system");
        sysObj["content"] = sys;
        messagesArray.append(sysObj);
    }

    for (const auto &msg : messages) {
        messagesArray.append(msg.toJson());
    }

    json["messages"] = messagesArray;
    json["model"] = m_config.model().name;
    json["temperature"] = m_config.model().temperature;
    json["max_tokens"] = m_config.model().maxTokens;
    json["stream"] = m_config.model().stream;

    // Function calling : on injecte les tools si présents.
    if (!m_tools.isEmpty()) {
        json["tools"] = m_tools;
        json["tool_choice"] = QStringLiteral("auto");
    }

    QString url = m_serverUrl + "chat/completions";
    QUrl requestUrl(url);
    QNetworkRequest qnaRequest(requestUrl);
    qnaRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QByteArray jsonStr = QJsonDocument(json).toJson(QJsonDocument::Compact);
    qInfo() << "[a-ice] → POST" << url << "(stream=" << m_config.model().stream
            << "tools=" << m_tools.size() << ")";
    qInfo().noquote() << "[a-ice] payload:" << jsonStr;

    m_currentReply = m_nam->post(qnaRequest, jsonStr);

    if (m_config.model().stream) {
        connect(m_currentReply, &QIODevice::readyRead, this, [this]() {
            if (!m_currentReply) return;
            parseSseChunk(m_currentReply->readAll());
        });
        connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
            if (!m_currentReply) return;

            if (!m_sseBuffer.isEmpty()) {
                handleStreamLine(m_sseBuffer);
                m_sseBuffer.clear();
            }

            QNetworkReply::NetworkError err = m_currentReply->error();
            int httpCode = m_currentReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QString errorString = m_currentReply->errorString();

            // Libère le reply et reset l'état AVANT d'émettre des signaux :
            // toolCallsReady → onToolCallsReady → executeToolCalls → sendMessages
            // serait appelé synchrone et échouerait avec "Déjà en cours..."
            // si m_isRequesting était encore true.
            m_currentReply->deleteLater();
            m_currentReply = nullptr;
            m_isRequesting = false;

            if (err == QNetworkReply::OperationCanceledError) {
                // Annulation utilisateur : onStopClicked() gère le reset.
            } else if (err != QNetworkReply::NoError) {
                QString msg = QStringLiteral("Réseau: %1 (HTTP %2)")
                                  .arg(errorString)
                                  .arg(httpCode);
                qWarning() << "[a-ice] error:" << msg;
                emit requestError(msg);
            } else {
                qInfo() << "[a-ice] ← flux terminé (HTTP" << httpCode << ")";
                // 1. Tool_calls natifs (delta.tool_calls) — format OpenAI standard.
                if (!m_pendingToolCalls.isEmpty()) {
                    QList<ToolCall> calls = m_pendingToolCalls.values();
                    qInfo() << "[a-ice] ←" << calls.size() << "tool_calls natifs reçus";
                    // Le modèle peut aussi écrire le tool_call en texte dans son
                    // thinking ou son content : on nettoie les deux.
                    QString cleanedThinking = m_streamedReasoning;
                    extractInlineToolCalls(cleanedThinking);
                    if (cleanedThinking != m_streamedReasoning)
                        emit thinkingUpdated(cleanedThinking);
                    QString cleanedContent = m_streamedContent;
                    extractInlineToolCalls(cleanedContent);
                    emit toolCallsReady(calls, cleanedContent.trimmed());
                }
                // 2. Fallback : tool_calls inline dans le reasoning ou le content
                //    (le serveur ne les a pas extraits en delta.tool_calls).
                else {
                    QString cleanedThinking = m_streamedReasoning;
                    QList<ToolCall> reasoningCalls = extractInlineToolCalls(cleanedThinking);
                    QString cleanedContent = m_streamedContent;
                    QList<ToolCall> contentCalls = extractInlineToolCalls(cleanedContent);

                    QList<ToolCall> allCalls;
                    allCalls.append(reasoningCalls);
                    allCalls.append(contentCalls);

                    if (!allCalls.isEmpty()) {
                        qInfo() << "[a-ice] ←" << allCalls.size()
                                << "tool_calls inline extraits (reasoning+content)";
                        if (cleanedThinking != m_streamedReasoning)
                            emit thinkingUpdated(cleanedThinking);
                        emit toolCallsReady(allCalls, cleanedContent.trimmed());
                    } else {
                        emit responseComplete();
                    }
                }
            }
        });
    } else {
        // Mode non-streaming : on lit tout à la fin.
        connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
            if (!m_currentReply) return;

            QNetworkReply::NetworkError err = m_currentReply->error();
            QByteArray body = m_currentReply->readAll();
            int httpCode = m_currentReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QString errorString = m_currentReply->errorString();
            qInfo() << "[a-ice] ← HTTP" << httpCode << "(" << body.size() << "bytes)";

            // Libère le reply et reset l'état AVANT d'émettre des signaux
            // (même raison que le mode streaming).
            m_currentReply->deleteLater();
            m_currentReply = nullptr;
            m_isRequesting = false;

            if (err == QNetworkReply::OperationCanceledError) {
                // Annulation.
            } else if (err != QNetworkReply::NoError) {
                QString msg = QStringLiteral("Réseau: %1").arg(errorString);
                qWarning() << "[a-ice] error:" << msg;
                emit requestError(msg);
            } else {
                QList<ToolCall> toolCalls;
                QString text = parseChatCompletion(body, &toolCalls);
                if (text.isNull() && toolCalls.isEmpty()) {
                    emit requestError(QStringLiteral("Réponse invalide: %1").arg(QString::fromUtf8(body)));
                } else if (!toolCalls.isEmpty()) {
                    qInfo() << "[a-ice] ←" << toolCalls.size() << "tool_calls natifs reçus";
                    emit toolCallsReady(toolCalls, text);
                } else {
                    if (!text.isEmpty()) emit contentChunk(text);
                    emit responseComplete();
                }
            }
        });
    }
}

void LlamaClient::consumeToolCallDelta(const QJsonArray &deltaCalls)
{
    // Chaque item a un "index" (position du tool_call dans la réponse) +
    // optionnellement "id", "function.name", "function.arguments".
    for (const QJsonValue &v : deltaCalls) {
        const QJsonObject c = v.toObject();
        int idx = static_cast<int>(c.value(QStringLiteral("index")).toInt(0));

        ToolCall &tc = m_pendingToolCalls[idx];
        if (c.contains(QStringLiteral("id"))) {
            tc.id = c.value(QStringLiteral("id")).toString();
        }
        const QJsonObject fn = c.value(QStringLiteral("function")).toObject();
        if (fn.contains(QStringLiteral("name"))) {
            tc.name = fn.value(QStringLiteral("name")).toString();
        }
        if (fn.contains(QStringLiteral("arguments"))) {
            // Les arguments arrivent en fragments concaténés.
            tc.arguments += fn.value(QStringLiteral("arguments")).toString();
        }
    }
}

void LlamaClient::parseSseChunk(const QByteArray &data)
{
    m_sseBuffer.append(data);

    int idx;
    while ((idx = m_sseBuffer.indexOf("\n\n")) != -1) {
        QByteArray line = m_sseBuffer.left(idx);
        m_sseBuffer = m_sseBuffer.mid(idx + 2);
        handleStreamLine(line);
    }
}

void LlamaClient::handleStreamLine(const QByteArray &line)
{
    const QByteArray prefix("data: ");
    if (!line.startsWith(prefix)) return;

    QByteArray payload = line.mid(prefix.size()).trimmed();
    if (payload.isEmpty()) return;
    if (payload == "[DONE]") return;

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

    // Tool calls : on accumule. Pendant tool_calls, le modèle n'émet
    // généralement pas de content.
    if (delta.contains(QStringLiteral("tool_calls"))) {
        const QJsonArray tc = delta.value(QStringLiteral("tool_calls")).toArray();
        consumeToolCallDelta(tc);
    }

    // reasoning_content (Qwen3, etc.).
    if (delta.contains(QStringLiteral("reasoning_content"))) {
        QString t = delta.value(QStringLiteral("reasoning_content")).toString();
        if (t.endsWith(QStringLiteral("</think>")))
            t.chop(8);
        if (!t.isEmpty()) {
            m_streamedReasoning += t;
            emit thinkingChunk(t);
        }
    }

    // content (réponse finale, ou <think>...</think> pour vieux serveurs).
    if (delta.contains(QStringLiteral("content"))) {
        QString t = delta.value(QStringLiteral("content")).toString();
        if (t.isEmpty()) return;

        // Accumule le content (pour fallback inline tool_calls).
        m_streamedContent += t;

        // Si on a déjà des tool_calls natifs en cours, le content est du texte
        // accompagnant l'appel (rare). On l'accumule sans l'émettre comme
        // réponse visible (ChatWidget peut choisir de l'afficher ou non).
        if (!m_pendingToolCalls.isEmpty()) {
            m_pendingAssistantContent += t;
            return;
        }

        if (m_inThinkingContent) {
            int closeIdx = t.indexOf(QStringLiteral("</think>"));
            if (closeIdx != -1) {
                if (closeIdx > 0)
                    emit thinkingChunk(t.left(closeIdx));
                m_inThinkingContent = false;
                QString rest = t.mid(closeIdx + 8).trimmed();
                if (!rest.isEmpty()) emit contentChunk(rest);
            } else {
                emit thinkingChunk(t);
            }
        } else if (t.startsWith(QStringLiteral("<think>"))) {
            m_inThinkingContent = true;
            QString after = t.mid(7);
            int closeIdx = after.indexOf(QStringLiteral("</think>"));
            if (closeIdx != -1) {
                if (closeIdx > 0) emit thinkingChunk(after.left(closeIdx));
                m_inThinkingContent = false;
                QString rest = after.mid(closeIdx + 8).trimmed();
                if (!rest.isEmpty()) emit contentChunk(rest);
            } else if (!after.isEmpty()) {
                emit thinkingChunk(after);
            }
        } else {
            if (t.startsWith(QStringLiteral("</think>")))
                t = t.mid(8).trimmed();
            if (!t.isEmpty()) emit contentChunk(t);
        }
    }
}

QString LlamaClient::parseChatCompletion(const QByteArray &body, QList<ToolCall> *outToolCalls)
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

    // Extraction des tool_calls (non-stream).
    if (outToolCalls && message.contains(QStringLiteral("tool_calls"))) {
        const QJsonArray calls = message.value(QStringLiteral("tool_calls")).toArray();
        for (const QJsonValue &v : calls) {
            const QJsonObject c = v.toObject();
            ToolCall tc;
            tc.id = c.value(QStringLiteral("id")).toString();
            const QJsonObject fn = c.value(QStringLiteral("function")).toObject();
            tc.name = fn.value(QStringLiteral("name")).toString();
            tc.arguments = fn.value(QStringLiteral("arguments")).toString();
            outToolCalls->append(tc);
        }
    }

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

    if (!reasoning.isEmpty())
        emit thinkingChunk(reasoning);

    if (content.isEmpty() && !reasoning.isEmpty())
        return reasoning;
    return content;
}

QString LlamaClient::buildToolsSystemPrompt() const
{
    if (m_tools.isEmpty()) return {};

    QString out = QStringLiteral(
        "# Tools\n\n"
        "You have access to tools. To call a tool, emit EXACTLY this format in your "
        "response (no other text around the call):\n\n"
        "{\"name\": \"<tool_name>\", \"arguments\": {\"<param>\": \"<value>\"}}\n\n"
        "You will receive the tool result in the next message (role=tool), then "
        "continue your answer. You can call tools multiple times in sequence. "
        "Only call a tool when it is genuinely useful.\n\n"
        "Available tools:\n");

    for (const QJsonValue &v : m_tools) {
        const QJsonObject fn = v.toObject().value(QStringLiteral("function")).toObject();
        const QString name = fn.value(QStringLiteral("name")).toString();
        const QString desc = fn.value(QStringLiteral("description")).toString();
        const QJsonObject params = fn.value(QStringLiteral("parameters")).toObject();
        const QJsonObject props = params.value(QStringLiteral("properties")).toObject();
        const QJsonArray required = params.value(QStringLiteral("required")).toArray();

        // Liste des params : nom (type, required/optional).
        QStringList paramLines;
        for (auto it = props.constBegin(); it != props.constEnd(); ++it) {
            const QString pname = it.key();
            const QString ptype = it.value().toObject().value(QStringLiteral("type")).toString();
            const bool isReq = std::find(required.begin(), required.end(), pname) != required.end();
            paramLines << QStringLiteral("%1 (%2%3)")
                          .arg(pname, ptype, isReq ? QStringLiteral(", required")
                                                    : QStringLiteral(", optional"));
        }

        out += QStringLiteral("- %1: %2\n").arg(name, desc);
        if (!paramLines.isEmpty())
            out += QStringLiteral("  params: %1\n").arg(paramLines.join(", "));
    }

    return out;
}

// Recherche la fin d'un objet JSON équilibré à partir de `start` (position de '{').
// Retourne l'index de '}' fermante, ou -1 si non équilibré.
static int findJsonObjectEnd(const QString &s, int start)
{
    int depth = 0;
    bool inString = false;
    bool escape = false;
    for (int i = start; i < s.size(); ++i) {
        QChar c = s[i];
        if (escape) { escape = false; continue; }
        if (c == '\\') { escape = true; continue; }
        if (c == '"') { inString = !inString; continue; }
        if (inString) continue;
        if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

QList<ToolCall> LlamaClient::extractInlineToolCalls(QString &text) const
{
    QList<ToolCall> calls;
    int searchFrom = 0;
    while (true) {
        int idx = text.indexOf(QStringLiteral("{\"name\""), searchFrom);
        if (idx == -1) break;

        int end = findJsonObjectEnd(text, idx);
        if (end == -1) {
            searchFrom = idx + 1;
            continue;
        }

        const QString jsonStr = text.mid(idx, end - idx + 1);
        const QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8());
        if (!doc.isObject()) {
            searchFrom = idx + 1;
            continue;
        }

        const QJsonObject obj = doc.object();
        if (!obj.contains(QStringLiteral("name")) || !obj.contains(QStringLiteral("arguments"))) {
            searchFrom = idx + 1;
            continue;
        }

        ToolCall tc;
        tc.id = QStringLiteral("inline_%1").arg(calls.size());
        tc.name = obj.value(QStringLiteral("name")).toString();

        // Filtre anti-faux-positif (item 10) : si le name extrait n'est pas un
        // tool réel, on considère que c'est de la prose (métaphore, exemple,
        // JSON quelconque) et on NE l'extrait pas. On ne retire pas non plus le
        // bloc du texte — c'est du contenu légitime.
        if (!m_knownToolNames.contains(tc.name)) {
            searchFrom = idx + 1;
            continue;
        }

        const QJsonValue args = obj.value(QStringLiteral("arguments"));
        if (args.isObject()) {
            tc.arguments = QString::fromUtf8(
                QJsonDocument(args.toObject()).toJson(QJsonDocument::Compact));
        } else {
            tc.arguments = args.toString();
        }
        calls.append(tc);

        // Retire le bloc du texte (et le whitespace adjacent).
        // On retire aussi un éventuel retour à la ligne avant le bloc.
        int removeStart = idx;
        while (removeStart > 0 && text[removeStart - 1] == '\n')
            removeStart--;
        text.remove(removeStart, end - removeStart + 1);
        searchFrom = removeStart;
    }

    text = text.trimmed();
    return calls;
}