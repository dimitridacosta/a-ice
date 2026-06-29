#include "ChatWidget.h"
#include "Bubble.h"
#include "Glass.h"
#include "llama_client.h"
#include "Config.h"
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
    // Interrompt la requête en cours. La bulle en cours reste affichée telle
    // quelle (réflexion + contenu partiels), et on l'enregistre dans
    // l'historique pour le tour suivant.
    m_client->cancel();
    m_isTyping = false;

    if (m_currentBubble) {
        ChatMessage assistantMsg;
        assistantMsg.role = "assistant";
        assistantMsg.content = m_currentContent;
        m_messages.append(assistantMsg);
        m_currentBubble = nullptr;
    }
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
    setGenerating(true);
    startAssistantBubble();

    m_client->sendMessages(m_messages);
    scrollToBottom();
    scheduleBlurUpdate();
}

void ChatWidget::startAssistantBubble()
{
    m_currentContent.clear();
    m_currentBubble = new Bubble(Bubble::Role::Assistant, this);
    connect(m_currentBubble, &Bubble::geometryChanged,
            this, &ChatWidget::scheduleBlurUpdate);
    m_messagesLayout->addWidget(m_currentBubble);
    m_currentBubble->show();
}

void ChatWidget::onThinkingChunk(const QString &text)
{
    if (m_currentBubble) {
        m_currentBubble->appendThinking(text);
    }
    scrollToBottom();
    scheduleBlurUpdate();
}

void ChatWidget::onContentChunk(const QString &text)
{
    m_currentContent.append(text);
    if (m_currentBubble) {
        // Premier contenu → on réduit la réflexion (gardée re-dépliable).
        if (m_currentBubble->thinkingExpanded()) {
            m_currentBubble->collapseThinking();
        }
        m_currentBubble->appendContent(text);
    }
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
    m_currentContent.clear();

    emit messageReceived(assistantMsg.content);
    scheduleBlurUpdate();
}

void ChatWidget::onRequestError(const QString &error)
{
    qWarning() << "[a-ice] UI error:" << error;
    if (m_currentBubble) {
        // Affiche l'erreur dans la bulle en cours (zone contenu).
        if (m_currentBubble->thinkingExpanded())
            m_currentBubble->collapseThinking();
        m_currentBubble->appendContent(QStringLiteral("⚠️ ") + error);
    } else {
        // Pas de bulle en cours : on en crée une dédiée.
        startAssistantBubble();
        if (m_currentBubble)
            m_currentBubble->appendContent(QStringLiteral("⚠️ ") + error);
    }
    m_isTyping = false;
    setGenerating(false);
    m_currentBubble = nullptr;
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