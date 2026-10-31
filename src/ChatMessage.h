#pragma once

#include <QString>
#include <QList>
#include <QJsonObject>

/// Un appel de tool émis par l'assistant dans sa réponse.
struct ToolCall {
    QString id;          // "call_abc..."
    QString name;        // "terminal", "brave_search", ...
    QString arguments;   // string JSON brut (cumulé pendant le streaming)
    QJsonObject parsedArgs() const; // parse arguments → QJsonObject (vide si invalide)
};

struct ChatMessage {
    QString role;       // "user" | "assistant" | "tool"
    QString content;

    // role="assistant" : tool_calls émis par le modèle (peut être vide).
    QList<ToolCall> toolCalls;

    // role="tool" : id de l'appel qu'on répond + nom du tool.
    QString toolCallId;
    QString toolName;

    /// Sérialise ce message vers le JSON OpenAI-compatible (à injecter dans
    /// le payload `messages`).
    QJsonObject toJson() const;
};