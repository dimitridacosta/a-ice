#pragma once

#include <QWidget>
#include <QScrollArea>
#include <QTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QList>
#include <QScopedPointer>
#include <QTimer>
#include <QString>
#include "ChatMessage.h"

class Bubble;
class LlamaClient;
class Config;
class ToolRegistry;
struct ToolCall;

/**
 * Overlay verre (glass) : pas de fenêtre classique.
 *
 * - La barre de prompt est fixe en bas (la fenêtre est positionnée
 *   bas-droite au-dessus du panel KDE par AiceApplet).
 * - Les bulles (une par tour) montent dans une zone scrollable transparente,
 *   les plus récentes en bas.
 * - La réflexion s'affiche en direct, puis se réduit quand la réponse
 *   commence à streamer (re-dépliable).
 * - Le blur KWin est appliqué uniquement derrière les bulles + la barre,
 *   le reste de la fenêtre est transparent → vrai glassmorphism.
 */
class ChatWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ChatWidget(QWidget *parent = nullptr);
    ~ChatWidget();

    void addMessage(const ChatMessage &msg);
    QString getServerUrl() const;
    void setServerUrl(const QString &url);
    void applyConfig(const Config &config);

signals:
    void messageSent(const QString &text);
    void messageReceived(const QString &text);

protected:
    void showEvent(QShowEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void paintEvent(QPaintEvent *) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onSendClicked();
    void onStopClicked();
    void onThinkingChunk(const QString &text);
    void onContentChunk(const QString &text);
    void onResponseComplete();
    void onRequestError(const QString &error);
    void onToolCallsReady(const QList<ToolCall> &calls, const QString &assistantContent);
    void onThinkingUpdated(const QString &cleanedThinking);

private:
    void setupUI();
    void setupGlass();
    void onSendMessage(const QString &text);
    void startAssistantBubble();
    void scrollToBottom();
    /// Construit le ToolRegistry selon la config (si tools.enabled).
    void setupTools(const Config &config);
    /// Exécute séquentiellement une liste de tool_calls, avec validation
    /// préalable (tool inconnu / JSON invalide / tronqué), puis relance le
    /// client. Voir ROADMAP item 3.
    void executeToolCallsValidated(const QList<ToolCall> &calls, int index);

    /// Ajoute un message role="tool" à l'historique (résultat de tool).
    /// Factorise l'injection des tool results (incl. cas d'erreur).
    void appendToolResult(const ToolCall &tc, const QString &result);

    /// Bascule le bouton entre “envoyer” et “stop” pendant la génération.
    void setGenerating(bool generating);

    /// Recalcule et applique la region blur KWin (union bulles + barre).
    void updateBlurRegion();
    /// Re-planifie un recalcul de blur (throttled).
    void scheduleBlurUpdate();

    /// Region blur/mask actuelle (pour ne rien reapply si inchangée → anti-clignote).
    QRegion m_lastRegion;

    QScopedPointer<LlamaClient> m_client;
    QScopedPointer<ToolRegistry> m_registry;
    QScrollArea *m_scrollArea = nullptr;
    QWidget    *m_messagesContainer = nullptr;
    QVBoxLayout *m_messagesLayout = nullptr;

    QFrame     *m_promptBar = nullptr;
    QTextEdit *m_promptEdit = nullptr;
    QPushButton *m_sendButton = nullptr;

    // Bulle assistant en cours de génération.
    Bubble *m_currentBubble = nullptr;
    QString m_currentContent; // contenu de la réponse en cours (pour l'historique)

    QTimer m_blurTimer;

    bool m_isTyping = false;
    int m_toolIterations = 0;
    QList<ChatMessage> m_messages;
};