#include "ChatWidget.h"
#include "Bubble.h"
#include "Glass.h"
#include "llama_client.h"
#include "Config.h"
#include "ToolRegistry.h"
#include "Tool.h"
#include "tools/TerminalTool.h"
#include "tools/TerminalSafety.h"
#include "tools/BraveSearchTool.h"
#include "tools/WebFetchTool.h"
#include <KWindowEffects>
#include <QPainterPath>
#include <QPainter>
#include <QPolygon>
#include <QRegion>
#include <QScrollBar>
#include <QScreen>
#include <QGuiApplication>
#include <QStyle>
#include <QRegularExpression>
#include <QKeyEvent>
#include <QDebug>
#include <QFrame>
#include <QWindow>
#include <QTextBlock>
#include <QTextLayout>
#include <QAbstractTextDocumentLayout>

namespace {
// Region arrondie (polygone issu d'un QPainterPath) — utilisée pour le blur
// derrière chaque bulle/barre.
QRegion roundedRegion(const QRect &r, int radius)
{
    if (r.isEmpty()) return {};
    QPainterPath path;
    path.addRoundedRect(r, radius, radius);
    QPolygonF poly = path.toFillPolygon();
    return QRegion(poly.toPolygon());
}

// Barre de prompt : peinte sur la fenêtre parente (ombre non clippée).
class GlassBar : public QFrame
{
public:
    explicit GlassBar(QWidget *p = nullptr) : QFrame(p)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setObjectName("prompt-bar");
    }
    const QImage &shadowImage()
    {
        const QRect r = rect().adjusted(kGlassInset, kGlassInset, -kGlassInset, -kGlassInset);
        if (m_shadowKey != r.size()) {
            m_shadow = makeShadowImage(r.size(), 22);
            m_shadowKey = r.size();
        }
        return m_shadow;
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QRect r = rect().adjusted(kGlassInset, kGlassInset, -kGlassInset, -kGlassInset);
        paintCard(p, r, 22, QColor(34, 34, 40, 110), QColor(255, 255, 255, 40));
    }
private:
    QImage m_shadow;
    QSize  m_shadowKey;
};

// (Les ombres de toutes les cartes — bulles + barre — sont peintes en une
//  seule passe sur la fenêtre ChatWidget, cf. ChatWidget::paintEvent, pour un
//  alignement parfait et un chevauchement libre.)
} // namespace

ChatWidget::ChatWidget(QWidget *parent)
    : QWidget(parent)
{
    setupUI();
    setupGlass();

    m_blurTimer.setSingleShot(true);
    m_blurTimer.setInterval(80);
    connect(&m_blurTimer, &QTimer::timeout, this, &ChatWidget::updateBlurRegion);
}

ChatWidget::~ChatWidget() = default;

void ChatWidget::setupUI()
{
    setAttribute(Qt::WA_TranslucentBackground);
    setObjectName("chat-root");

    m_client.reset(new LlamaClient(this));
    connect(m_client.data(), &LlamaClient::thinkingChunk,
            this, &ChatWidget::onThinkingChunk);
    connect(m_client.data(), &LlamaClient::contentChunk,
            this, &ChatWidget::onContentChunk);
    connect(m_client.data(), &LlamaClient::responseComplete,
            this, &ChatWidget::onResponseComplete);
    connect(m_client.data(), &LlamaClient::toolCallsReady,
            this, &ChatWidget::onToolCallsReady);
    connect(m_client.data(), &LlamaClient::thinkingUpdated,
            this, &ChatWidget::onThinkingUpdated);
    connect(m_client.data(), &LlamaClient::requestError,
            this, &ChatWidget::onRequestError);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // --- Zone bulles (scrollable, transparente) ---
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setStyleSheet(
        "QScrollArea { background: transparent; border: none; }");

    m_messagesContainer = new QWidget();
    m_messagesContainer->setObjectName("messages-container");
    m_messagesContainer->setStyleSheet("background: transparent;");
    m_messagesLayout = new QVBoxLayout(m_messagesContainer);
    // Top = 13px : laisse place pour l'ombre des bulles (kGlassBlur*2 - offset - inset).
    // Left/right = 8 : alignement horizontal avec la barre de prompt.
    m_messagesLayout->setContentsMargins(8, 13, 8, 8);
    m_messagesLayout->setSpacing(4);
    // Stretch en HAUT : les bulles s'empilent en bas et montent au fur et à
    // mesure qu'elles grandissent (et non depuis le haut).
    m_messagesLayout->insertStretch(0, 1);
    m_scrollArea->setWidget(m_messagesContainer);
    m_scrollArea->setAlignment(Qt::AlignBottom);

    // Le scroll déplace les bulles en coordonnées fenêtre → il faut recalculer
    // la region blur pour qu'elle suive le contenu (sinon décalage blur/contenu).
    connect(m_scrollArea->verticalScrollBar(), &QAbstractSlider::valueChanged,
            this, &ChatWidget::scheduleBlurUpdate);

    root->addWidget(m_scrollArea, 1);

    // --- Barre de prompt (glass pill, fixe en bas) ---
    m_promptBar = new GlassBar(this);
    auto *barLayout = new QHBoxLayout(m_promptBar);
    barLayout->setContentsMargins(18, 8, 10, 8);
    barLayout->setSpacing(6);

    m_promptEdit = new QTextEdit(m_promptBar);
    m_promptEdit->setObjectName("prompt-edit");
    m_promptEdit->setPlaceholderText(QStringLiteral("Ask A-Ice\u2026"));
    m_promptEdit->setFrameShape(QFrame::NoFrame);
    m_promptEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_promptEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_promptEdit->setTabChangesFocus(true);
    m_promptEdit->setAcceptRichText(false);
    m_promptEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_promptEdit->setStyleSheet(
        "QTextEdit#prompt-edit {"
        "  background: transparent; border: none;"
        "  color: #f0f1f3; font-size: 14px; padding: 6px 4px;"
        "  selection-background-color: rgba(52,120,218,180);"
        "}"
        "QTextEdit#prompt-edit::placeholder { color: rgba(239,240,241,110); }");
    m_promptEdit->installEventFilter(this);
    // Prompt auto-resize : utilise documentSize().height() qui donne la
    // hauteur réelle du contenu après mise en page (word-wrap inclus).
    // Guard anti-boucle : setFixedHeight pourrait trigger documentSizeChanged
    // dans certains cas (bien que la largeur ne change pas).
    auto adjustPromptHeight = [this]() {
        static bool guard = false;
        if (guard) return;
        guard = true;

        auto *docLayout = m_promptEdit->document()->documentLayout();
        const int docH = (int)std::ceil(docLayout->documentSize().height());
        // +12 pour les viewport margins (padding QSS : 6px top + 6px bottom)
        const int h = qBound(30, docH + 12, 140);
        if (m_promptEdit->height() != h) {
            m_promptEdit->setFixedHeight(h);
            m_promptBar->updateGeometry();
            scheduleBlurUpdate();
        }
        m_promptEdit->ensureCursorVisible();

        guard = false;
    };
    // contentsChanged : texte modifié (frappe, suppression, coller)
    connect(m_promptEdit->document(), &QTextDocument::contentsChanged,
            this, adjustPromptHeight);
    // documentSizeChanged : mise en page terminée (wrap recalculé)
    connect(m_promptEdit->document()->documentLayout(),
            &QAbstractTextDocumentLayout::documentSizeChanged,
            this, [adjustPromptHeight](const QSizeF &) { adjustPromptHeight(); });
    // Initialisation différée (police QSS appliquée après le premier event loop)
    QTimer::singleShot(0, this, adjustPromptHeight);
    barLayout->addWidget(m_promptEdit, 1);

    m_sendButton = new QPushButton(QStringLiteral("↑"), m_promptBar);
    m_sendButton->setObjectName("send-button");
    m_sendButton->setCursor(Qt::PointingHandCursor);
    m_sendButton->setFixedSize(34, 34);
    m_sendButton->setStyleSheet(
        "QPushButton#send-button {"
        "  background: rgba(52,120,218,200);"
        "  color: #ffffff; border: none; border-radius: 17px;"
        "  font-size: 16px; font-weight: bold;"
        "}"
        "QPushButton#send-button:hover { background: rgba(0,161,236,220); }"
        "QPushButton#send-button:pressed { background: rgba(30,100,200,220); }"
        "QPushButton#send-button#send-button[stop=\"true\"] {"
        "  background: rgba(218,68,83,210);"
        "}"
        "QPushButton#send-button[stop=\"true\"]:hover {"
        "  background: rgba(237,21,21,230);"
        "}");
    connect(m_sendButton, &QPushButton::clicked, this, [this]() {
        if (m_isTyping) onStopClicked(); else onSendClicked();
    });
    barLayout->addWidget(m_sendButton);

    // Bottom = 19px : laisse place pour l'ombre de la barre (kGlassBlur*2 + offset - inset).
    // Top = 6 : petit écart entre la zone scroll et la barre.
    auto *barWrap = new QVBoxLayout();
    barWrap->setContentsMargins(8, 6, 8, 19);
    barWrap->addWidget(m_promptBar);
    root->addLayout(barWrap);
}

void ChatWidget::setupGlass()
{
    // Rien ici : le blur KWin est appliqué dynamiquement derrière les
    // éléments visibles (bulles + barre). La fenêtre elle-même reste
    // transparente → seul le verre apparaît.
}

void ChatWidget::scheduleBlurUpdate()
{
    update(); // fenêtre : redessine toutes les ombres + planifie le blur
    if (!m_blurTimer.isActive()) m_blurTimer.start();
}

void ChatWidget::updateBlurRegion()
{
    // Blur KWin + masque de clic appliqués UNIQUEMENT derrière les éléments
    // visibles (barre + chaque bulle). La fenêtre elle-même reste totalement
    // transparente : pas de grande carte floutée, juste des bulles verre
    // flottantes indépendantes. Le click-through passe hors des bulles.
    QWindow *win = window()->windowHandle();
    if (!win) {
        if (isVisible())
            QTimer::singleShot(100, this, &ChatWidget::updateBlurRegion);
        return;
    }

    QRegion region;

    // Barre de prompt : blur sur la carte (inset 8 px), pas sur le widget entier
    // (sinon le halo d'ombre serait flouté).
    const QRect barCard = m_promptBar->rect().adjusted(kGlassInset, kGlassInset, -kGlassInset, -kGlassInset)
        .translated(m_promptBar->mapTo(window(), QPoint(0, 0)));
    region = region.united(roundedRegion(barCard, 22));

    // Chaque bulle visible dans le viewport (en coordonnées fenêtre) — carte inset 8 px.
    const QRect vp = m_scrollArea->viewport()->rect()
        .translated(m_scrollArea->viewport()->mapTo(window(), QPoint(0, 0)));
    for (int i = 0; i < m_messagesLayout->count(); ++i) {
        QLayoutItem *item = m_messagesLayout->itemAt(i);
        QWidget *w = item ? item->widget() : nullptr;
        if (!w || !w->isVisible()) continue;
        QRect wr = w->rect().adjusted(kGlassInset, kGlassInset,
                              -kGlassInset, -kGlassInset)
                       .translated(w->mapTo(window(), QPoint(0, 0)));
        wr = wr.intersected(vp);
        if (wr.width() > 4 && wr.height() > 4)
            region = region.united(roundedRegion(wr, 16));
    }

    region = region.intersected(QRect(0, 0, width(), height()));

    // Anti-clignotement : ne rien reapply si la region n'a pas changé.
    if (region == m_lastRegion)
        return;
    m_lastRegion = region;

    // Pas de setMask : il provoque un re-reshape de la fenêtre à chaque update
    // → clignotement. La fenêtre reste transparente hors des bulles ; les clics
    // dans la colonne sont captés (compromis acceptable pour la stabilité).
    KWindowEffects::enableBlurBehind(win, true, region);

    // CRUCIAL : enableBlurBehind ne commit pas la surface Wayland. KWin n'applique
    // le nouveau blur qu'au prochain repaint de la fenêtre (d'où l'effet « bouger
    // la souris / ouvrir le menu KDE pour que ça se redessine »). On force donc un
    // repaint immédiat pour committer la surface et déclencher le re-render du blur.
    if (auto *top = window()) top->repaint();
}

void ChatWidget::paintEvent(QPaintEvent *)
{
    // Toutes les ombres (bulles + barre) peintes sur la fenêtre en une passe →
    // alignement parfait et chevauchement libre (addition des ombres).
    QPainter p(this);

    // Ombres des bulles (coordonnées fenêtre, scroll pris en compte par mapTo).
    for (int i = 0; i < m_messagesLayout->count(); ++i) {
        QLayoutItem *item = m_messagesLayout->itemAt(i);
        auto *b = qobject_cast<Bubble*>(item ? item->widget() : nullptr);
        if (!b) continue;
        const QRect card = QRect(b->mapTo(this, QPoint(0, 0)), b->size())
            .adjusted(kGlassInset, kGlassInset, -kGlassInset, -kGlassInset);
        drawShadow(p, b->shadowImage(), card);
    }

    // Ombre de la barre de prompt.
    if (m_promptBar) {
        const QRect card = m_promptBar->geometry()
            .adjusted(kGlassInset, kGlassInset, -kGlassInset, -kGlassInset);
        drawShadow(p, static_cast<GlassBar*>(m_promptBar)->shadowImage(), card);
    }
}

void ChatWidget::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    scheduleBlurUpdate();
    // Le layout de la barre peut tarder à se stabiliser : on re-déclenche le
    // recalcul du blur à quelques instants pour rattraper la géométrie finale.
    QTimer::singleShot(200, this, &ChatWidget::scheduleBlurUpdate);
    QTimer::singleShot(500, this, &ChatWidget::scheduleBlurUpdate);
}

void ChatWidget::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    // La géométrie a changé : force un recalcul (même si la region « logique »
    // semble identique, les coords fenêtre ont bougé).
    m_lastRegion = QRegion();
    scheduleBlurUpdate();
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
    setupTools(config);
}

void ChatWidget::setupTools(const Config &config)
{
    if (!config.tools().enabled) {
        m_registry.reset();
        m_client->setTools({});
        return;
    }
    m_registry.reset(new ToolRegistry(this));
    m_registry->add(new TerminalTool(config.tools().terminalWorkdir, this));
    m_registry->add(new BraveSearchTool(config.tools().braveApiKey, this));
    m_registry->add(new WebFetchTool(this));
    m_client->setTools(m_registry->toJsonArray());
    qInfo() << "[a-ice] tools registered:" << m_registry->toJsonArray().size();
}

void ChatWidget::onSendClicked()
{
    QString text = m_promptEdit->toPlainText().trimmed();
    if (text.isEmpty()) return;
    m_promptEdit->clear();
    onSendMessage(text);
}

void ChatWidget::onStopClicked()
{
    // Interrompt la requête ET l'execution d'un tool en cours. La bulle reste
    // affichee telle quelle (reflexion + contenu partiels), et on l'enregistre
    // dans l'historique pour le tour suivant. Voir ROADMAP item 8.
    m_interruptRequested = true;
    m_client->cancel();
    if (m_registry)
        m_registry->cancelAll();

    // Si un tool est en cours, on injecte immediatement une reponse role="tool"
    // "cancelled" pour que l'historique reste coherent (sinon l'API rejette au
    // prochain sendMessages : tool_call sans reponse). Le callback du tool, quand
    // il arrivera, verra m_interruptRequested et ne re-appendera pas.
    if (m_toolCallInProgressActive) {
        const QString msg = QStringLiteral("[a-ice] cancelled by user");
        ChatMessage toolMsg;
        toolMsg.role = QStringLiteral("tool");
        toolMsg.toolCallId = m_toolCallInProgressId;
        toolMsg.toolName = m_toolCallInProgressName;
        toolMsg.content = sanitizeToolResult(msg);
        m_messages.append(toolMsg);
        if (m_activeTool)
            m_activeTool->setResult(msg, false);
        m_activeTool = nullptr;
        m_toolCallInProgressActive = false;
        m_toolCallInProgressId.clear();
        m_toolCallInProgressName.clear();
    }

    m_isTyping = false;

    if (m_currentBubble) {
        // Retire le placeholder d'attente si jamais aucun chunk n'est arrivé.
        clearWaitingBlock();
        // Ferme la réflexion en cours (si ouverte) avant de figer la carte.
        if (m_activeThinking) m_activeThinking->collapse();
        ChatMessage assistantMsg;
        assistantMsg.role = "assistant";
        assistantMsg.content = m_currentContent;
        m_messages.append(assistantMsg);
        m_currentBubble = nullptr;
    }
    m_activeThinking = nullptr;
    m_activeContent  = nullptr;
    m_activeTool     = nullptr;
    m_currentContent.clear();
    setGenerating(false);
    scheduleBlurUpdate();
}

void ChatWidget::setGenerating(bool generating)
{
    m_sendButton->setText(generating ? QStringLiteral("■") : QStringLiteral("↑"));
    m_sendButton->setProperty("stop", generating ? QStringLiteral("true")
                                                : QStringLiteral("false"));
    m_sendButton->style()->unpolish(m_sendButton);
    m_sendButton->style()->polish(m_sendButton);
    m_sendButton->setToolTip(generating ? QStringLiteral("Stop generation")
                                         : QStringLiteral("Send"));
}

void ChatWidget::onSendMessage(const QString &text)
{
    // Bulle utilisateur (droite).
    ChatMessage userMsg;
    userMsg.role = "user";
    userMsg.content = text;
    m_messages.append(userMsg);

    auto *bubble = new Bubble(Bubble::Role::User, this);
    bubble->appendContent(text);
    bubble->fitToContent(m_scrollArea->viewport()->width() - 16);
    connect(bubble, &Bubble::geometryChanged, this, &ChatWidget::scheduleBlurUpdate);
    m_messagesLayout->addWidget(bubble, 0, Qt::AlignRight);

    // Prépare la bulle assistant (gauche, réflexion en direct).
    m_isTyping = true;
    m_toolIterations = 0;
    m_interruptRequested = false;
    setGenerating(true);
    startAssistantBubble();

    m_client->sendMessages(m_messages);
    scrollToBottom();
    scheduleBlurUpdate();
}

void ChatWidget::startAssistantBubble()
{
    m_currentContent.clear();
    m_activeThinking = nullptr;
    m_activeContent  = nullptr;
    m_activeTool     = nullptr;
    m_currentBubble = new Bubble(Bubble::Role::Assistant, this);
    connect(m_currentBubble, &Bubble::geometryChanged,
            this, &ChatWidget::scheduleBlurUpdate);
    m_messagesLayout->addWidget(m_currentBubble);
    m_currentBubble->show();
    // Placeholder d'attente : évite une carte vide (amateur) entre le
    // sendMessages et le premier chunk. Retiré dès qu'un vrai bloc arrive.
    m_waitingBlock = m_currentBubble->addWaitingBlock();
    scrollToBottom();
    scheduleBlurUpdate();
}

void ChatWidget::clearWaitingBlock()
{
    if (!m_waitingBlock)
        return;
    m_waitingBlock->stopShimmer();
    m_waitingBlock->deleteLater();
    m_waitingBlock = nullptr;
    scheduleBlurUpdate();
}

void ChatWidget::onThinkingChunk(const QString &text)
{
    // Une carte par réponse agent ; chaque phase de thinking = un nouveau bloc
    // empilé dans la carte. Si pas de bloc thinking actif (nouveau tour), on
    // en crée un. Voir ROADMAP item 6 (révisé : une carte + blocs empilés).
    if (!m_currentBubble)
        startAssistantBubble();
    clearWaitingBlock();
    if (!m_activeThinking)
        m_activeThinking = m_currentBubble->addThinkingBlock();
    m_activeThinking->append(text);
    scrollToBottom();
    scheduleBlurUpdate();
}

void ChatWidget::onThinkingUpdated(const QString &cleanedThinking)
{
    // Remplace le thinking affiché par la version nettoyée (sans les blocs
    // tool_calls inline que le modèle a écrit en réfléchissant).
    if (m_activeThinking)
        m_activeThinking->set(cleanedThinking);
    scrollToBottom();
    scheduleBlurUpdate();
}

void ChatWidget::onContentChunk(const QString &text)
{
    if (!m_currentBubble)
        startAssistantBubble();
    clearWaitingBlock();
    // Nouveau bloc content (nouveau tour) → on reset l'accumulation API.
    if (!m_activeContent) {
        m_currentContent.clear();
        m_activeContent = m_currentBubble->addContentBlock();
        // Premier contenu du tour → on réduit la réflexion (gardée dépliable).
        if (m_activeThinking) m_activeThinking->collapse();
    }
    m_currentContent.append(text);
    m_activeContent->append(text);
    scrollToBottom();
    scheduleBlurUpdate();
}

void ChatWidget::onResponseComplete()
{
    // Enregistre la réponse dans l'historique pour le prochain tour.
    ChatMessage assistantMsg;
    assistantMsg.role = "assistant";
    assistantMsg.content = m_currentContent;
    m_messages.append(assistantMsg);

    m_isTyping = false;
    setGenerating(false);
    m_currentBubble = nullptr;
    m_activeThinking = nullptr;
    m_activeContent  = nullptr;
    m_activeTool     = nullptr;
    m_waitingBlock   = nullptr;
    m_currentContent.clear();
    m_interruptRequested = false;

    emit messageReceived(assistantMsg.content);
    scheduleBlurUpdate();
}

void ChatWidget::onToolCallsReady(const QList<ToolCall> &calls, const QString &assistantContent)
{
    clearWaitingBlock();
    // 1. Enregistre le message assistant (avec tool_calls) dans l'historique.
    //    On le fait AVANT le check de limite : si on stoppe, l'historique doit
    //    rester cohérent (assistant + tool_calls suivi de leurs réponses role="tool").
    ChatMessage asstMsg;
    asstMsg.role = QStringLiteral("assistant");
    asstMsg.content = assistantContent;
    asstMsg.toolCalls = calls;
    m_messages.append(asstMsg);

    // Finalise les blocs du tour courant : content nettoyé (sans les blocs
    // tool_calls extraits par LlamaClient) + repli de la réflexion. On détache
    // les blocs actifs : le prochain tour créera de nouveaux blocs dans la
    // MÊME carte (une carte = toute la réponse agent).
    m_currentContent = assistantContent;
    if (m_activeContent)
        m_activeContent->set(assistantContent);
    if (m_activeThinking)
        m_activeThinking->collapse();
    m_activeThinking = nullptr;
    m_activeContent  = nullptr;

    // 2. Tools désactivés : on ne peut rien exécuter. On répond quand même
    //    role="tool" à chaque tool_call (sinon l'API rejette le prochain send).
    if (!m_registry) {
        for (const ToolCall &tc : calls)
            appendToolResult(tc, QStringLiteral("Tools are disabled. Do NOT call tools."));
        onRequestError(QStringLiteral("Tool call received but tools are disabled."));
        return;
    }

    // 3. Limite anti-boucle : on tolère 8 tours de tools par message utilisateur.
    if (++m_toolIterations > 8) {
        // L'API OpenAI-compatible exige une réponse role="tool" pour chaque
        // tool_call_id. On en injecte une "limit reached" par call avant de
        // stopper, sinon le prochain sendMessages sera rejeté.
        for (const ToolCall &tc : calls)
            appendToolResult(tc, QStringLiteral("Tool iteration limit reached (8). Do NOT retry."));
        if (m_currentBubble) {
            auto *tb = m_currentBubble->addToolBlock(QStringLiteral("limit"));
            tb->setResult(QStringLiteral("Tool iteration limit reached (8). Do NOT retry."), false);
        }
        onRequestError(QStringLiteral("Too many tool iterations (limit 8)."));
        return;
    }

    // 4. Lance l'exécution validée.
    executeToolCallsValidated(calls, 0);
}

void ChatWidget::appendToolResult(const ToolCall &tc, const QString &result)
{
    ChatMessage toolMsg;
    toolMsg.role = QStringLiteral("tool");
    toolMsg.toolCallId = tc.id;
    toolMsg.toolName = tc.name;
    toolMsg.content = sanitizeToolResult(result);
    m_messages.append(toolMsg);
}

void ChatWidget::executeToolCallsValidated(const QList<ToolCall> &calls, int index)
{
    if (m_interruptRequested)
        return;  // Stop demande : on ne lance plus rien.

    if (index >= calls.size()) {
        // Tous les tools ont répondu : on relance le modèle avec l'historique
        // mis à jour (incluant les messages role="tool"). La carte reste la
        // même : le prochain tour ajoutera de nouveaux blocs (thinking/content/
        // tool) empilés dans la même carte. Une carte = une réponse agent.
        m_client->sendMessages(m_messages);
        return;
    }

    const ToolCall &tc = calls.at(index);

    // (a) Tool inconnu : on n'exécute pas, on répond au modèle avec la liste des
    //     tools disponibles, puis on passe au tool_call suivant (ne pas stopper).
    if (!m_registry->find(tc.name)) {
        const QString msg = QStringLiteral(
            "Unknown tool '%1'. Available tools: %2. Do NOT retry with this name.")
            .arg(tc.name, m_registry->availableNames());
        appendToolResult(tc, msg);
        if (m_currentBubble) {
            auto *tb = m_currentBubble->addToolBlock(tc.name);
            tb->setParams(tc.arguments);
            tb->setResult(msg, false);
        }
        executeToolCallsValidated(calls, index + 1);
        return;
    }

    // (b) Arguments invalides : args non vide mais non parsable en JSON.
    if (!tc.arguments.isEmpty() && tc.parsedArgs().isEmpty()) {
        const bool truncated = !(tc.arguments.endsWith(QLatin1Char('}'))
                                 || tc.arguments.endsWith(QLatin1Char(']')));
        if (truncated) {
            // Tronqué → on arrête tout (le modèle a buggé sur le streaming).
            const QString msg = QStringLiteral(
                "Arguments truncated (incomplete JSON): '%1'. Do NOT retry.")
                .arg(tc.arguments);
            appendToolResult(tc, msg);
            if (m_currentBubble) {
                auto *tb = m_currentBubble->addToolBlock(tc.name);
                tb->setParams(tc.arguments);
                tb->setResult(msg, false);
            }
            onRequestError(msg);
            return;
        }
        // JSON invalide mais complet → on demande au modèle de réessayer.
        const QString msg = QStringLiteral(
            "Invalid JSON arguments: '%1'. Provide valid JSON, then retry.")
            .arg(tc.arguments);
        appendToolResult(tc, msg);
        if (m_currentBubble) {
            auto *tb = m_currentBubble->addToolBlock(tc.name);
            tb->setParams(tc.arguments);
            tb->setResult(msg, false);
        }
        executeToolCallsValidated(calls, index + 1);
        return;
    }

    // (c) Cas nominal : tool connu + JSON valide.
    const QJsonObject args = tc.parsedArgs();

    // Sécurité — approval policy (mutations + paths sensibles + dangerous).
    // Uniquement pour le terminal : on check la commande via checkApprovalRequired.
    // Tout ce qui n'est pas lecture pure (ou qui touche une zone sensible) demande
    // approbation. Si pas encore approuvé en session, on suspend la boucle et on
    // demande l'approbation inline (bulle en cours).
    if (tc.name == QLatin1String("terminal")) {
        const QString cmd = args.value(QStringLiteral("command")).toString();
        const QString reason = checkApprovalRequired(cmd);
        if (!reason.isEmpty() && !m_sessionApprovedPatterns.contains(reason)) {
            if (m_currentBubble) {
                // Bloc tool en attente d'approbation + bandeau inline.
                m_activeTool = m_currentBubble->addToolBlock(tc.name);
                m_activeTool->setParams(tc.arguments);
                m_activeTool->setStatus(QStringLiteral("\u26a0"));
                m_currentBubble->showApproval(cmd, reason);
                scrollToBottom();
                scheduleBlurUpdate();
                // Connexion one-shot : reprend la boucle au clic.
                connect(m_currentBubble, &Bubble::approvalResult, this,
                    [this, calls, index, reason](int choice) {
                        const ToolCall &tc2 = calls.at(index);
                        switch (choice) {
                        case Bubble::AllowSession:
                            m_sessionApprovedPatterns.insert(reason);
                            if (m_currentBubble) m_currentBubble->clearApproval();
                            executeToolCallNow(calls, index);
                            break;
                        case Bubble::AllowOnce:
                            if (m_currentBubble) m_currentBubble->clearApproval();
                            executeToolCallNow(calls, index);
                            break;
                        case Bubble::Deny: {
                            const QString msg = QStringLiteral(
                                "User denied this command requiring approval "
                                "(matched '%1'). Do NOT retry this command - the "
                                "user has explicitly rejected it.").arg(reason);
                            appendToolResult(tc2, msg);
                            if (m_activeTool)
                                m_activeTool->setResult(msg, false);
                            m_activeTool = nullptr;
                            if (m_currentBubble) m_currentBubble->clearApproval();
                            executeToolCallsValidated(calls, index + 1);
                            break;
                        }
                        }
                    }, Qt::SingleShotConnection);
            }
            return; // suspendu en attente d'approbation
        }
    }

    executeToolCallNow(calls, index);
}

void ChatWidget::executeToolCallNow(const QList<ToolCall> &calls, int index)
{
    if (index >= calls.size()) {
        m_client->sendMessages(m_messages);
        return;
    }
    const ToolCall &tc = calls.at(index);
    const QJsonObject args = tc.parsedArgs();

    // Bloc tool (créé ici pour le chemin nominal ; le chemin approval a déjà
    // pré-créé m_activeTool → on le réutilise pour ne pas dupliquer).
    if (!m_activeTool && m_currentBubble)
        m_activeTool = m_currentBubble->addToolBlock(tc.name);
    if (m_activeTool) {
        m_activeTool->setParams(tc.arguments);
        m_activeTool->setStatus(QStringLiteral("\u23f3")); // running
    }
    scrollToBottom();
    scheduleBlurUpdate();

    // Tracker : si l'utilisateur appuie sur Stop pendant l'exec, onStopClicked
    // utilise ces infos pour injecter un role="tool" "cancelled" coherent.
    m_toolCallInProgressId = tc.id;
    m_toolCallInProgressName = tc.name;
    m_toolCallInProgressActive = true;

    m_registry->execute(tc.name, args, [this, calls, index](bool ok, QString result) {
        // Stop demande pendant l'exec : onStopClicked a deja injecte le
        // role="tool" "cancelled" (si m_toolCallInProgressActive etait true).
        // On finalise juste le visuel si ce n'etait pas deja fait, et on
        // n'enchaîne SURTOUT pas sur le tool suivant.
        if (m_interruptRequested) {
            if (m_toolCallInProgressActive) {
                // Le tool n'avait pas encore repond quand Stop a ete clique,
                // mais onStopClicked n'a pas injecte (race rare) — on le fait
                // ici pour rester coherent.
                if (m_activeTool)
                    m_activeTool->setResult(result, ok);
                appendToolResult(calls.at(index), result);
                m_activeTool = nullptr;
                m_toolCallInProgressActive = false;
                m_toolCallInProgressId.clear();
                m_toolCallInProgressName.clear();
            }
            return;
        }

        if (m_activeTool)
            m_activeTool->setResult(result, ok);

        appendToolResult(calls.at(index), result);
        m_activeTool = nullptr;
        m_toolCallInProgressActive = false;
        m_toolCallInProgressId.clear();
        m_toolCallInProgressName.clear();

        scrollToBottom();
        scheduleBlurUpdate();

        // Tool suivant (ou relance du client si dernier).
        executeToolCallsValidated(calls, index + 1);
    });
}

void ChatWidget::onRequestError(const QString &error)
{
    qWarning() << "[a-ice] UI error:" << error;
    if (!m_currentBubble)
        startAssistantBubble();
    clearWaitingBlock();
    if (m_activeThinking) m_activeThinking->collapse();
    if (m_currentBubble) {
        auto *cb = m_currentBubble->addContentBlock();
        cb->set(QStringLiteral("\u26a0\ufe0f ") + error);
    }
    m_isTyping = false;
    setGenerating(false);
    m_currentBubble = nullptr;
    m_activeThinking = nullptr;
    m_activeContent  = nullptr;
    m_activeTool     = nullptr;
    m_waitingBlock   = nullptr;
    m_interruptRequested = false;
    scheduleBlurUpdate();
}

void ChatWidget::scrollToBottom()
{
    QScrollBar *bar = m_scrollArea->verticalScrollBar();
    bar->setValue(bar->maximum());
}

bool ChatWidget::eventFilter(QObject *watched, QEvent *event)
{
    // Clic sur le prompt : on demande explicitement le focus clavier (surface
    // layer-shell en mode OnDemand). Au clic ailleurs (autre appli), KWin rend
    // le focus à cette appli → on peut taper dans d'autres fenêtres.
    if (watched == m_promptEdit && event->type() == QEvent::MouseButtonPress) {
        if (auto *w = window()->windowHandle()) w->requestActivate();
    }

    // Entrée = envoyer ; Maj+Entrée = nouvelle ligne (QTextEdit insère
    // lui-même le retour ligne quand on ne consomme pas l'événement).
    if (watched == m_promptEdit && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
            if (!(ke->modifiers() & Qt::ShiftModifier)) {
                onSendClicked();
                return true; // consommé : pas de newline
            }
            // Shift+Entrée : on laisse passer → newline insérée par défaut
        }
    }
    return QWidget::eventFilter(watched, event);
}