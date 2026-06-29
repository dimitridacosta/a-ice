#include "ChatWidget.h"
#include "llama_client.h"
#include "Config.h"
#include <QLabel>
#include <QPalette>
#include <QFont>
#include <QSizePolicy>
#include <QScrollBar>
#include <QTimer>
#include <QKeyEvent>
#include <QDebug>

ChatWidget::ChatWidget(QWidget *parent)
    : QWidget(parent)
    , m_isTyping(false)
{
    setupUI();
}

ChatWidget::~ChatWidget() = default;

void ChatWidget::setupUI()
{
    m_client.reset(new LlamaClient(this));

    // Connexions streaming : la bulle assistant se remplit en live.
    connect(m_client.data(), &LlamaClient::thinkingChunk,
            this, &ChatWidget::onThinkingChunk);
    connect(m_client.data(), &LlamaClient::contentChunk,
            this, &ChatWidget::onContentChunk);
    connect(m_client.data(), &LlamaClient::responseComplete,
            this, &ChatWidget::onResponseComplete);
    connect(m_client.data(), &LlamaClient::requestError,
            this, &ChatWidget::onRequestError);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    // Zone de messages scrollable
    m_scrollArea = new QScrollArea();
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setMinimumHeight(250);

    m_messagesContainer = new QWidget();
    m_messagesContainer->setObjectName("messages-container");
    auto *msgLayout = new QVBoxLayout(m_messagesContainer);
    msgLayout->setSpacing(8);
    msgLayout->setContentsMargins(4, 4, 4, 4);
    msgLayout->addStretch(1); // messages alignés en haut
    m_scrollArea->setWidget(m_messagesContainer);

    // Indicateur de "en train d'écrire"
    m_typingIndicator = new QFrame();
    m_typingIndicator->setObjectName("typing-indicator");
    m_typingIndicator->setMinimumSize(60, 24);
    m_typingIndicator->setVisible(false);

    // Input area
    auto *inputLayout = new QHBoxLayout();
    inputLayout->setSpacing(4);

    m_messageTextEdit = new QTextEdit();
    m_messageTextEdit->setObjectName("message-input");
    m_messageTextEdit->setPlaceholderText("Pose ta question à A-ICE...");
    m_messageTextEdit->setMinimumHeight(36);
    m_messageTextEdit->setMaximumHeight(120);
    m_messageTextEdit->setAcceptRichText(false);
    m_messageTextEdit->installEventFilter(this);

    m_sendButton = new QPushButton("Envoyer");
    m_sendButton->setObjectName("send-button");
    m_sendButton->setMinimumSize(70, 36);
    connect(m_sendButton, &QPushButton::clicked, this, &ChatWidget::onSendClicked);

    inputLayout->addWidget(m_messageTextEdit, 1);
    inputLayout->addWidget(m_sendButton);

    auto *inputWidget = new QWidget();
    inputWidget->setLayout(inputLayout);

    layout->addWidget(m_scrollArea);
    layout->addWidget(inputWidget);

    setStyleSheet(
        "#aice-window { background: qlineargradient(spread:pad, x1:0, y1:0, x2:0, y2:1, "
        "stop:0 rgba(30,30,35,255), stop:1 rgba(20,20,25,255)); }"
        "#messages-container { background: transparent; }"
        "#message-input { background: rgba(40,40,50,200); color: #e0e0e0; border-radius: 6px; }"
        "#send-button { background: #4a8c5a; color: #fff; border-radius: 6px; }"
        "#send-button:hover { background: #5a9c6a; }"
        "#typing-indicator { background: rgba(40,40,50,150); border-radius: 10px; }"
    );
}

void ChatWidget::addMessage(const ChatMessage &msg)
{
    m_messages.append(msg);
    addMessageBubble(msg);
}

void ChatWidget::clearMessages()
{
    m_messages.clear();
}

QString ChatWidget::getServerUrl() const
{
    return m_client->serverUrl();
}

void ChatWidget::setServerUrl(const QString &url)
{
    m_client->setServerUrl(url);
}

void ChatWidget::applyConfig(const Config &config)
{
    m_client->setConfig(config);
}

void ChatWidget::onSendClicked()
{
    QString text = m_messageTextEdit->toPlainText().trimmed();
    if (!text.isEmpty()) {
        onSendMessage(text);
        m_messageTextEdit->clear();
    }
}

void ChatWidget::onSendMessage(const QString &text)
{
    // Message utilisateur
    ChatMessage userMsg;
    userMsg.role = "user";
    userMsg.content = text;
    addMessage(userMsg);

    // Prépare la bulle assistant en streaming
    m_isTyping = true;
    showTypingIndicator();
    startAssistantBubble();

    // Lance la requête
    m_client->sendMessages(m_messages);
}

void ChatWidget::startAssistantBubble()
{
    m_currentBubble = new QFrame();
    m_currentBubble->setObjectName("assistant-bubble");
    m_currentBubble->setStyleSheet(
        "QFrame#assistant-bubble { background: rgba(40,40,50,180); border-radius: 10px; }");

    auto *layout = new QVBoxLayout(m_currentBubble);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(4);

    // Zone thinking (cachée tant qu'il n'y a rien)
    m_currentThinkingLabel = new QLabel();
    m_currentThinkingLabel->setObjectName("thinking-zone");
    m_currentThinkingLabel->setWordWrap(true);
    m_currentThinkingLabel->setOpenExternalLinks(false);
    m_currentThinkingLabel->setTextFormat(Qt::RichText);
    m_currentThinkingLabel->setStyleSheet(
        "color: #8a8a98; font-style: italic; font-size: 11px; "
        "background: rgba(20,20,28,120); border-radius: 6px; padding: 4px;");
    m_currentThinkingLabel->setVisible(false);
    layout->addWidget(m_currentThinkingLabel);

    // Zone content (réponse finale)
    m_currentContentLabel = new QLabel();
    m_currentContentLabel->setObjectName("content-zone");
    m_currentContentLabel->setWordWrap(true);
    m_currentContentLabel->setOpenExternalLinks(false);
    m_currentContentLabel->setTextFormat(Qt::RichText);
    m_currentContentLabel->setStyleSheet(
        "color: #d0d8c0; font-family: monospace; font-size: 12px;");
    layout->addWidget(m_currentContentLabel);

    m_currentThinking.clear();
    m_currentContent.clear();

    auto *msgLayout = qobject_cast<QVBoxLayout*>(m_messagesContainer->layout());
    // On insère avant le stretch (dernier item).
    msgLayout->insertWidget(msgLayout->count() - 1, m_currentBubble);

    scrollToBottom();
}

void ChatWidget::appendThinking(const QString &text)
{
    m_currentThinking.append(text);
    if (m_currentThinkingLabel) {
        // Échappe le HTML puis rend les newlines visibles.
        QString safe = m_currentThinking.toHtmlEscaped();
        safe.replace("\n", "<br>");
        m_currentThinkingLabel->setText(
            QString("<b>💭 Réflexion</b><br>%1").arg(safe));
        m_currentThinkingLabel->setVisible(true);
    }
    scrollToBottom();
}

void ChatWidget::appendContent(const QString &text)
{
    m_currentContent.append(text);
    if (m_currentContentLabel) {
        QString safe = m_currentContent.toHtmlEscaped();
        safe.replace("\n", "<br>");
        m_currentContentLabel->setText(safe);
        m_currentContentLabel->setVisible(true);
    }
    scrollToBottom();
}

void ChatWidget::onThinkingChunk(const QString &text)
{
    appendThinking(text);
}

void ChatWidget::onContentChunk(const QString &text)
{
    appendContent(text);
}

void ChatWidget::onResponseComplete()
{
    // Une fois la réponse terminée, on l'ajoute à l'historique.
    ChatMessage assistantMsg;
    assistantMsg.role = "assistant";
    assistantMsg.content = m_currentContent.isEmpty() ? m_currentThinking
                                                       : m_currentContent;
    m_messages.append(assistantMsg);

    m_isTyping = false;
    hideTypingIndicator();

    // Nettoyage des pointeurs de bulle en cours (le widget reste affiché).
    m_currentBubble = nullptr;
    m_currentThinkingLabel = nullptr;
    m_currentContentLabel = nullptr;
    m_currentThinking.clear();
    m_currentContent.clear();

    emit messageReceived(assistantMsg.content);
}

void ChatWidget::onRequestError(const QString &error)
{
    qWarning() << "[a-ice] UI error:" << error;
    showError(error);

    // Nettoyage de la bulle en cours si elle est vide.
    if (m_currentBubble && m_currentContent.isEmpty() && m_currentThinking.isEmpty()) {
        m_currentBubble->deleteLater();
    }
    m_currentBubble = nullptr;
    m_currentThinkingLabel = nullptr;
    m_currentContentLabel = nullptr;
    m_currentThinking.clear();
    m_currentContent.clear();

    m_isTyping = false;
    hideTypingIndicator();
}

void ChatWidget::scrollToBottom()
{
    QScrollBar *bar = m_scrollArea->verticalScrollBar();
    bar->setValue(bar->maximum());
}

void ChatWidget::addMessageBubble(const ChatMessage &msg)
{
    QString html = formatMessage(msg);

    auto *bubble = new QFrame();
    bubble->setObjectName(msg.role == "user" ? "user-bubble" : "assistant-bubble");
    bubble->setStyleSheet("background: rgba(45,48,58,180); border-radius: 10px; padding: 6px;");

    auto *content = new QLabel();
    content->setObjectName("bubble-content");
    content->setWordWrap(true);
    content->setOpenExternalLinks(false);
    content->setTextFormat(Qt::RichText);
    content->setText(html);

    auto *layout = new QVBoxLayout(bubble);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(content);

    if (msg.role == "user") {
        content->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    } else {
        content->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    }

    auto *msgLayout = qobject_cast<QVBoxLayout*>(m_messagesContainer->layout());
    msgLayout->insertWidget(msgLayout->count() - 1, bubble);

    scrollToBottom();
}

QString ChatWidget::formatMessage(const ChatMessage &msg)
{
    QString formatted = msg.content.toHtmlEscaped();
    formatted.replace("\n", "<br>");

    if (msg.role == "user") {
        return QString("<div style='color: #c8e8d0; font-family: sans-serif; font-size: 13px;'>%1</div>")
                   .arg(formatted);
    } else {
        return QString("<div style='color: #d0d8c0; font-family: monospace; font-size: 12px;'>%1</div>")
                   .arg(formatted);
    }
}

void ChatWidget::showTypingIndicator()
{
    // L'indicateur visuel est désormais la bulle streaming elle-même.
    // On garde la méthode pour compat, mais on ne crée plus l'ancien indicateur.
}

void ChatWidget::hideTypingIndicator()
{
    // Idem : plus rien à cacher.
}

void ChatWidget::showError(const QString &error)
{
    ChatMessage errorMsg;
    errorMsg.role = "assistant";
    errorMsg.content = QString("⚠️ %1").arg(error);
    addMessage(errorMsg);
}

bool ChatWidget::eventFilter(QObject *watched, QEvent *event)
{
    // Enter (sans Shift) = envoyer. Shift+Enter = newline.
    // Implémenté ici plutôt que dans setupUI pour garder setupUI lisible.
    if (watched == m_messageTextEdit && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
            if (!(ke->modifiers() & Qt::ShiftModifier)) {
                onSendClicked();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}