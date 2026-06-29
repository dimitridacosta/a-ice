#pragma once

#include <QWidget>
#include <QLabel>
#include <QTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QFrame>
#include <QList>
#include <QScopedPointer>
#include "ChatMessage.h"

class LlamaClient;
class Config;

class ChatWidget : public QWidget
{
    Q_OBJECT

public:
        explicit ChatWidget(QWidget *parent = nullptr);
        ~ChatWidget();

    void addMessage(const ChatMessage &msg);
    void clearMessages();
    QString getServerUrl() const;
    void setServerUrl(const QString &url);
    /// Applique la config (provider + modèle) au client.
    void applyConfig(const Config &config);

signals:
    void messageSent(const QString &text);
    void messageReceived(const QString &text);

private slots:
    void onSendClicked();
    void onSendMessage(const QString &text);

    // Streaming : mises à jour live de la bulle en cours.
    void onThinkingChunk(const QString &text);
    void onContentChunk(const QString &text);
    void onResponseComplete();
    void onRequestError(const QString &error);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setupUI();
    void addMessageBubble(const ChatMessage &msg);
    QString formatMessage(const ChatMessage &msg);
    void showTypingIndicator();
    void hideTypingIndicator();
    void showError(const QString &error);

    /// Crée la bulle assistant en cours (avec zones thinking + content).
    void startAssistantBubble();
    /// Ajoute du texte à la zone thinking de la bulle en cours.
    void appendThinking(const QString &text);
    /// Ajoute du texte à la zone content de la bulle en cours.
    void appendContent(const QString &text);
    /// Scroll automatique vers le bas.
    void scrollToBottom();

    QScopedPointer<LlamaClient> m_client;
    QScrollArea *m_scrollArea = nullptr;
    QWidget *m_messagesContainer = nullptr;
    QFrame *m_typingIndicator = nullptr;

    // Bulle assistant en cours de génération.
    QFrame *m_currentBubble = nullptr;
    QLabel *m_currentThinkingLabel = nullptr;
    QLabel *m_currentContentLabel = nullptr;
    QString m_currentThinking;
    QString m_currentContent;

    QTextEdit *m_messageTextEdit = nullptr;
    QPushButton *m_sendButton = nullptr;

    bool m_isTyping = false;
    QList<ChatMessage> m_messages;
};