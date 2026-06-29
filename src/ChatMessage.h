#pragma once

#include <QString>

struct ChatMessage {
    QString role;       // "user" ou "assistant"
    QString content;
};
