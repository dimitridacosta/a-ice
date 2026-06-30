#include "ChatMessage.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonParseError>

QJsonObject ToolCall::parsedArgs() const
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(arguments.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return {};
    return doc.object();
}

QJsonObject ChatMessage::toJson() const
{
    QJsonObject obj;
    obj["role"] = role;

    if (role == "tool") {
        // Message de résultat de tool : role, content, tool_call_id, name.
        obj["content"] = content;
        if (!toolCallId.isEmpty())
            obj["tool_call_id"] = toolCallId;
        if (!toolName.isEmpty())
            obj["name"] = toolName;
        return obj;
    }

    if (role == "assistant" && !toolCalls.isEmpty()) {
        // Assistant qui a émis des tool_calls. Le champ content peut être vide.
        if (!content.isEmpty())
            obj["content"] = content;
        else
            obj["content"] = QJsonValue(); // null explicit
        QJsonArray calls;
        for (const ToolCall &tc : toolCalls) {
            QJsonObject c;
            c["id"] = tc.id;
            c["type"] = QStringLiteral("function");
            QJsonObject fn;
            fn["name"] = tc.name;
            fn["arguments"] = tc.arguments;
            c["function"] = fn;
            calls.append(c);
        }
        obj["tool_calls"] = calls;
        return obj;
    }

    // user / assistant normal
    obj["content"] = content;
    return obj;
}