#include "Bubble.h"
#include "Glass.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QPushButton>
#include <QPainter>
#include <QResizeEvent>
#include <QTimer>
#include <QLinearGradient>
#include <QTextDocument>
#include <QTextCursor>
#include <QTextBlock>
#include <QFontInfo>
#include <QRegularExpression>
#include <cmath>

// --- Rendu Markdown -> HTML (Qt6 natif) ---
// QTextDocument::setMarkdown() produit un document riche qu'on serialise en HTML
// pour le QLabel. On force la police, la couleur du theme et un peu d'air
// (marges titres/paragraphes) pour que le rendu colle au glass.
static const char *kMdTextColor = "#e9eaee";

// CSS injecte dans le <style> emis par Qt : styling visuel uniquement.
// NB : les marges p/h1-h6 sont ecrasees par le style inline emis par Qt
// (margin-top:0; etc.), donc on les cuit dans les QTextBlockFormat via
// applyMdSpacing() ci-dessous.
static const QString kMdCss = QStringLiteral(
    "blockquote { margin: 6px 0; padding-left: 10px; color: rgba(239,240,241,170); border-left: 2px solid rgba(255,255,255,60); }"
    "code { background: rgba(255,255,255,22); padding: 1px 4px; border-radius: 4px; }"
    "pre  { background: rgba(255,255,255,18); padding: 8px; border-radius: 6px; margin: 6px 0; }"
    "table { border-collapse: collapse; margin: 8px 0; }"
    "th, td { border: 1px solid rgba(255,255,255,70); padding: 4px 8px; }"
    "th { background: rgba(255,255,255,28); font-weight: 600; }"
    "hr  { border: none; border-top: 1px solid rgba(255,255,255,50); margin: 8px 0; }"
);

// Cuit les marges (air) dans les QTextBlockFormat : Qt emet ces marges en inline
// dans le HTML final, donc elles sont toujours honorees par le QLabel.
// - Paragraphes/listes : 5px top/bottom
// - Titres (police plus grande que la base) : 14px top, 7px bottom
// - Premier bloc : top=0 ; dernier bloc : bottom=0 (bulle nette aux bords)
static void applyMdSpacing(QTextDocument &doc)
{
    const qreal base = 13.0;
    QTextBlock first = doc.firstBlock();
    QTextBlock last  = doc.lastBlock();
    for (QTextBlock b = doc.firstBlock(); b.isValid(); b = b.next()) {
        const QTextCharFormat cf = b.charFormat();
        const int px = QFontInfo(cf.font()).pixelSize();
        const bool heading = px > int(base) + 1; // h1-h4 sont plus grands que 13px
        qreal top = heading ? 14.0 : 5.0;
        qreal bot = heading ? 7.0  : 5.0;
        if (b == first) top = 0.0;
        if (b == last)  bot = 0.0;
        QTextBlockFormat bf = b.blockFormat();
        bf.setTopMargin(top);
        bf.setBottomMargin(bot);
        QTextCursor c(b);
        c.setBlockFormat(bf);
    }
}

static QString renderMarkdown(const QString &md)
{
    if (md.isEmpty()) return {};
    QTextDocument doc;
    QFont f;
    f.setPixelSize(13);
    doc.setDefaultFont(f);
    // Dialecte GitHub = CommonMark + tables + fenced code + task lists.
    doc.setMarkdown(md, QTextDocument::MarkdownDialectGitHub);
    applyMdSpacing(doc);
    QString html = doc.toHtml();
    // Qt hardcode body { color:#000000; ... } -> couleur du theme.
    html.replace(QRegularExpression(QStringLiteral("color:#000000;")),
                 QStringLiteral("color:%1;").arg(QString::fromLatin1(kMdTextColor)));
    html.replace(QRegularExpression(QStringLiteral("color:#000000 ")),
                 QStringLiteral("color:%1 ").arg(QString::fromLatin1(kMdTextColor)));
    // Injecte le CSS visuel avant la fin du bloc <style>.
    html.replace(QStringLiteral("</style>"), kMdCss + QStringLiteral("</style>"));
    return html;
}

// Mesure le rendu Markdown a une largeur de wrap donnee.
// NB : QLabel utilise documentMargin=0 en interne pour son QTextDocument,
// donc on met aussi 0 ici pour que la largeur mesuree colle au rendu reel
// (sinon idealWidth contient 8px de marge -> bulle trop large -> padding
// gauche plus grand que droite a cause de l'alignement droite).
static QSize measureMarkdown(const QString &md, int textW, const QFont &font)
{
    QTextDocument doc;
    doc.setDefaultFont(font);
    doc.setDocumentMargin(0);
    doc.setMarkdown(md, QTextDocument::MarkdownDialectGitHub);
    applyMdSpacing(doc);
    const qreal idealW = doc.idealWidth();
    const qreal w = qMin(idealW, qreal(textW));
    doc.setTextWidth(w);
    return QSize(int(std::ceil(w)), int(std::ceil(doc.size().height())));
}

static const char *kContentStyle =
    "QLabel#content-label {"
    "  color: #e9eaee;"
    "  font-size: 13px;"
    "}"
    "QLabel#thinking-label {"
    "  color: rgba(239,240,241,165);"
    "  font-style: italic; font-size: 11px;"
    "}";

// Flags communs pour rendre un QLabel sélectionnable au clavier et à la souris.
static constexpr Qt::TextInteractionFlags kSelectFlags =
    Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard;
// Idem + liens cliquables (ouvrent le navigateur via setOpenExternalLinks).
static constexpr Qt::TextInteractionFlags kLinkFlags =
    kSelectFlags | Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard;

// --- ShimmerButton : QPushButton dont le texte a un dégradé animé ---
// Une vague lumineuse fine traverse le texte de gauche à droite en continu.
// Subtil et rapide : indique que la section « Reasoning » est vivante.
class ShimmerButton : public QPushButton {
public:
    explicit ShimmerButton(const QString &t, QWidget *p = nullptr)
        : QPushButton(t, p)
    {
        m_timer = new QTimer(this);
        m_timer->setInterval(25); // ~40 fps, rapide
        connect(m_timer, &QTimer::timeout, this, [this]() {
            m_phase += 6;
            update();
        });
    }
    /// Démarre le shimmer (IA en train de réfléchir).
    void startShimmer() { if (!m_timer->isActive()) m_timer->start(); }
    /// Arrête le shimmer et passe en couleur fixe (réflexion terminée).
    void stopShimmer()  { m_timer->stop(); update(); }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setFont(font());
        p.setRenderHint(QPainter::TextAntialiasing);

        // Padding du QSS : 2px top, 4px left
        const QRect r = rect().adjusted(4, 2, -4, -2);

        if (!m_timer->isActive()) {
            // Shimmer arrêté : couleur fixe
            p.setPen(QColor(239, 240, 241, 200));
            p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, text());
            return;
        }

        const int w = width();
        if (w <= 0) return;

        // Une seule vague fine, de gauche à droite
        const double cycle = w * 1.5;
        const double offset = std::fmod(m_phase, cycle);

        QLinearGradient grad(-offset, 0, cycle - offset, 0);
        const QColor dim(239, 240, 241, 130);
        const QColor bright(239, 240, 241, 210);
        grad.setColorAt(0.00, dim);
        grad.setColorAt(0.35, bright);
        grad.setColorAt(0.55, dim);
        grad.setColorAt(1.00, dim);

        p.setPen(QPen(QBrush(grad), 1));
        p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, text());
    }
private:
    int     m_phase = 0;
    QTimer *m_timer;
};

Bubble::Bubble(Role role, QWidget *parent)
    : QWidget(parent)
    , m_role(role)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setObjectName(role == Role::User ? "user-bubble" : "assistant-bubble");
    // heightForWidth pour les bulles assistant (contenu dynamique qui wrap).
    auto sp = sizePolicy();
    sp.setHeightForWidth(true);
    setSizePolicy(sp);

    if (role == Role::Assistant) buildAssistant();
    else                          buildUser();
}

int Bubble::heightForWidth(int w) const
{
    if (layout())
        return layout()->heightForWidth(w);
    return -1;
}

void Bubble::fitToContent(int maxBubbleWidth)
{
    if (!m_contentLabel || m_role != Role::User) return;

    // Marges internes : layout (16+16 H, 12+12 V) uniquement.
    const int hPad = 16 + 16; // 32
    const int vPad = 12 + 12; // 24
    const int maxTextW = maxBubbleWidth - hPad;
    if (maxTextW <= 0) return;

    QFont f = m_contentLabel->font();
    f.setPixelSize(13);

    // Mesure via QTextDocument (rendu Markdown) pour coller au rendu reel.
    const QSize sz = measureMarkdown(m_content, maxTextW, f);

    int bubbleW = qMin(sz.width() + hPad, maxBubbleWidth);
    int bubbleH = sz.height() + vPad;

    setFixedSize(bubbleW, bubbleH);
}

void Bubble::paintEvent(QPaintEvent *)
{
    // Carte uniquement : l'ombre est peinte par le parent (conteneur) afin de
    // pouvoir déborder et se chevaucher avec les bulles voisines.
    QPainter p(this);
    const QRect r = rect().adjusted(kGlassInset, kGlassInset, -kGlassInset, -kGlassInset);
    const QColor fill = (m_role == Role::User)
        ? QColor(52, 120, 218, 90)
        : QColor(34, 34, 40, 100);
    paintCard(p, r, 16, fill, QColor(255, 255, 255, 36));
}

const QImage &Bubble::shadowImage()
{
    const QRect r = rect().adjusted(kGlassInset, kGlassInset, -kGlassInset, -kGlassInset);
    if (m_shadowKey != r.size()) {
        m_shadow = makeShadowImage(r.size(), 16);
        m_shadowKey = r.size();
    }
    return m_shadow;
}

void Bubble::buildUser()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(0);

    m_contentLabel = new QLabel(this);
    m_contentLabel->setObjectName("content-label");
    m_contentLabel->setStyleSheet(kContentStyle);
    m_contentLabel->setWordWrap(true);
    m_contentLabel->setTextFormat(Qt::RichText);
    m_contentLabel->setTextInteractionFlags(kLinkFlags);
    m_contentLabel->setOpenExternalLinks(true);
    m_contentLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->addWidget(m_contentLabel);
}

void Bubble::buildAssistant()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(6);

    // En-tête cliquable « Réflexion ▾/▸ » (replie/déplie le texte de réflexion),
    // aligné à GAUCHE de la bulle. Clos par défaut.
    m_thinkingToggle = new ShimmerButton(QStringLiteral("Reasoning  \u25b8"), this);
    m_thinkingToggle->setObjectName("thinking-toggle");
    m_thinkingToggle->setFlat(true);
    m_thinkingToggle->setCursor(Qt::PointingHandCursor);
    m_thinkingToggle->setStyleSheet(
        "QPushButton#thinking-toggle {"
        "  background: transparent; border: none;"
        "  color: rgba(239,240,241,200);"
        "  font-size: 11px; font-style: italic;"
        "  padding: 2px 4px; text-align: left;"
        "}"
        "QPushButton#thinking-toggle:hover {"
        "  color: rgba(239,240,241,255);"
        "}");
    m_thinkingToggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_thinkingToggle, &QPushButton::clicked, this, &Bubble::toggleThinking);
    layout->addWidget(m_thinkingToggle);

    // Texte de réflexion (repliable) — QLabel standard, pas d'animation.
    // Clos par défaut : l'utilisateur déplie en cliquant sur le shimmer.
    m_thinkingLabel = new QLabel(this);
    m_thinkingLabel->setObjectName("thinking-label");
    m_thinkingLabel->setStyleSheet(kContentStyle);
    m_thinkingLabel->setWordWrap(true);
    m_thinkingLabel->setTextFormat(Qt::RichText);
    m_thinkingLabel->setOpenExternalLinks(true);
    m_thinkingLabel->setTextInteractionFlags(kLinkFlags);
    m_thinkingLabel->setVisible(false); // clos par defaut
    layout->addWidget(m_thinkingLabel);

    // Contenu de la reponse finale (rendu Markdown).
    m_contentLabel = new QLabel(this);
    m_contentLabel->setObjectName("content-label");
    m_contentLabel->setStyleSheet(kContentStyle);
    m_contentLabel->setWordWrap(true);
    m_contentLabel->setTextFormat(Qt::RichText);
    m_contentLabel->setTextInteractionFlags(kLinkFlags);
    m_contentLabel->setOpenExternalLinks(true);
    m_contentLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    layout->addWidget(m_contentLabel);

    m_contentLabel->setVisible(false);
}

void Bubble::refreshThinkingLabel()
{
    if (!m_thinkingLabel) return;
    // Rendu Markdown (meme moteur que le contenu) -> coherent avec l'usage IA.
    m_thinkingLabel->setText(renderMarkdown(m_thinking));
}

void Bubble::refreshContentLabel()
{
    if (!m_contentLabel) return;
    // Rendu Markdown -> HTML. Re-parse a chaque chunk (streaming) : acceptable
    // pour un chat, et garantit la coherence avec la mesure fitToContent.
    m_contentLabel->setText(renderMarkdown(m_content));
    m_contentLabel->setVisible(!m_content.isEmpty());
}

void Bubble::appendThinking(const QString &text)
{
    m_thinking.append(text);
    refreshThinkingLabel();
    if (m_thinkingToggle) m_thinkingToggle->startShimmer();
    emit geometryChanged();
}

void Bubble::appendContent(const QString &text)
{
    m_content.append(text);
    refreshContentLabel();
    if (m_thinkingToggle) m_thinkingToggle->stopShimmer();
    emit geometryChanged();
}

void Bubble::setContent(const QString &text)
{
    m_content = text;
    refreshContentLabel();
    if (m_thinkingToggle) m_thinkingToggle->stopShimmer();
    emit geometryChanged();
}

void Bubble::setThinking(const QString &text)
{
    m_thinking = text;
    refreshThinkingLabel();
    emit geometryChanged();
}

void Bubble::collapseThinking()
{
    if (!m_thinkingLabel) return;
    m_thinkingExpanded = false;
    m_thinkingLabel->setVisible(false);
    if (m_thinkingToggle)
        m_thinkingToggle->setText(QStringLiteral("Reasoning  ▸"));
    emit geometryChanged();
}

void Bubble::expandThinking()
{
    if (!m_thinkingLabel) return;
    m_thinkingExpanded = true;
    m_thinkingLabel->setVisible(true);
    if (m_thinkingToggle)
        m_thinkingToggle->setText(QStringLiteral("Reasoning  ▾"));
    emit geometryChanged();
}

void Bubble::toggleThinking()
{
    if (m_thinkingExpanded) collapseThinking();
    else                    expandThinking();
}

// --- Approval inline ---------------------------------------------------------
void Bubble::showApproval(const QString &command, const QString &description)
{
    // Remplace un éventuel bandeau précédent (séquence de plusieurs dangerous).
    if (m_approvalWidget) {
        m_approvalWidget->deleteLater();
        m_approvalWidget = nullptr;
    }

    auto *frame = new QFrame(this);
    frame->setObjectName("approval-frame");
    frame->setStyleSheet(
        "QFrame#approval-frame {"
        "  background: rgba(255,170,40,38);"
        "  border: 1px solid rgba(255,170,40,120);"
        "  border-radius: 8px;"
        "}"
        "QLabel#approval-warn { color: #ffcc66; font-size: 12px; }"
        "QLabel#approval-cmd  { color: #e9eaee; font-size: 12px;"
        "  font-family: monospace; background: rgba(255,255,255,18);"
        "  padding: 4px 6px; border-radius: 4px; }"
        "QPushButton#allow-once    { background: rgba(80,200,120,70);"
        "  border: 1px solid rgba(80,200,120,140); border-radius: 6px;"
        "  color: #e9eaee; padding: 4px 10px; }"
        "QPushButton#allow-session { background: rgba(80,200,120,110);"
        "  border: 1px solid rgba(80,200,120,160); border-radius: 6px;"
        "  color: #e9eaee; padding: 4px 10px; }"
        "QPushButton#deny          { background: rgba(220,80,80,90);"
        "  border: 1px solid rgba(220,80,80,150); border-radius: 6px;"
        "  color: #e9eaee; padding: 4px 10px; }"
        "QPushButton:hover { background: rgba(255,255,255,50); }"
    );

    auto *v = new QVBoxLayout(frame);
    v->setContentsMargins(10, 8, 10, 8);
    v->setSpacing(6);

    auto *warn = new QLabel(QStringLiteral("⚠  %1").arg(description), frame);
    warn->setObjectName("approval-warn");
    warn->setWordWrap(true);
    v->addWidget(warn);

    auto *cmd = new QLabel(command, frame);
    cmd->setObjectName("approval-cmd");
    cmd->setWordWrap(true);
    v->addWidget(cmd);

    auto *row = new QHBoxLayout();
    row->setSpacing(8);

    auto mkBtn = [frame](const char *name, const QString &label) {
        auto *b = new QPushButton(label, frame);
        b->setObjectName(name);
        b->setCursor(Qt::PointingHandCursor);
        return b;
    };
    auto *once    = mkBtn("allow-once",    QStringLiteral("Allow once"));
    auto *session = mkBtn("allow-session", QStringLiteral("Allow session"));
    auto *deny    = mkBtn("deny",          QStringLiteral("Deny"));
    row->addWidget(once);
    row->addWidget(session);
    row->addStretch();
    row->addWidget(deny);
    v->addLayout(row);

    connect(once,    &QPushButton::clicked, this, [this]() {
        if (m_approvalWidget) m_approvalWidget->hide();
        emit approvalResult(AllowOnce);
    });
    connect(session, &QPushButton::clicked, this, [this]() {
        if (m_approvalWidget) m_approvalWidget->hide();
        emit approvalResult(AllowSession);
    });
    connect(deny,    &QPushButton::clicked, this, [this]() {
        if (m_approvalWidget) m_approvalWidget->hide();
        emit approvalResult(Deny);
    });

    m_approvalWidget = frame;
    if (layout()) layout()->addWidget(frame);
    emit geometryChanged();
}