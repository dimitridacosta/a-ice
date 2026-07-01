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
    if (!m_userLabel || m_role != Role::User) return;

    // Marges internes : layout (16+16 H, 12+12 V) uniquement.
    const int hPad = 16 + 16; // 32
    const int vPad = 12 + 12; // 24
    const int maxTextW = maxBubbleWidth - hPad;
    if (maxTextW <= 0) return;

    QFont f = m_userLabel->font();
    f.setPixelSize(13);

    // Mesure via QTextDocument (rendu Markdown) pour coller au rendu reel.
    const QSize sz = measureMarkdown(m_userContent, maxTextW, f);

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

    m_userLabel = new QLabel(this);
    m_userLabel->setObjectName("content-label");
    m_userLabel->setStyleSheet(kContentStyle);
    m_userLabel->setWordWrap(true);
    m_userLabel->setTextFormat(Qt::RichText);
    m_userLabel->setTextInteractionFlags(kLinkFlags);
    m_userLabel->setOpenExternalLinks(true);
    m_userLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->addWidget(m_userLabel);
}

void Bubble::appendContent(const QString &text)
{
    // Rôle user uniquement : accumulate + rendu Markdown.
    m_userContent.append(text);
    if (m_userLabel) m_userLabel->setText(renderMarkdown(m_userContent));
    emit geometryChanged();
}

void Bubble::buildAssistant()
{
    // Carte assistant : un layout vertical qui empile les blocs au fur et à
    // mesure (ThinkingBlock / ToolBlock / ContentBlock). Une seule carte pour
    // toute la réponse agent (voir ROADMAP item 6, version révisée).
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(16, 12, 16, 12);
    m_layout->setSpacing(6);
}

// --- Pile de blocs (rôle assistant) ---
ThinkingBlock *Bubble::addThinkingBlock()
{
    auto *b = new ThinkingBlock(this);
    connect(b, &ThinkingBlock::geometryChanged, this, &Bubble::geometryChanged);
    m_layout->addWidget(b);
    b->show();
    emit geometryChanged();
    return b;
}

ToolBlock *Bubble::addToolBlock(const QString &name)
{
    auto *b = new ToolBlock(name, this);
    connect(b, &ToolBlock::geometryChanged, this, &Bubble::geometryChanged);
    m_layout->addWidget(b);
    b->show();
    emit geometryChanged();
    return b;
}

ContentBlock *Bubble::addContentBlock()
{
    auto *b = new ContentBlock(this);
    connect(b, &ContentBlock::geometryChanged, this, &Bubble::geometryChanged);
    m_layout->addWidget(b);
    b->show();
    emit geometryChanged();
    return b;
}

WaitingBlock *Bubble::addWaitingBlock()
{
    auto *b = new WaitingBlock(this);
    connect(b, &WaitingBlock::geometryChanged, this, &Bubble::geometryChanged);
    m_layout->addWidget(b);
    b->show();
    emit geometryChanged();
    return b;
}

// --- WaitingBlock ---
WaitingBlock::WaitingBlock(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_label = new ShimmerButton(QStringLiteral("Reading"), this);
    m_label->setObjectName("waiting-toggle");
    m_label->setFlat(true);
    m_label->setEnabled(false);  // non-cliquable (pas un toggle)
    m_label->setCursor(Qt::ArrowCursor);
    m_label->setStyleSheet(
        "QPushButton#waiting-toggle {"
        "  background: transparent; border: none;"
        "  color: rgba(239,240,241,200);"
        "  font-size: 11px; font-style: italic;"
        "  padding: 2px 4px; text-align: left;"
        "}");
    m_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(m_label);

    // Points animés qui défilent (0→1→2→3→0) ~3fps : donne une vraie
    // sensation d'activité pendant que le LLM infère.
    m_dotsTimer = new QTimer(this);
    m_dotsTimer->setInterval(350);
    connect(m_dotsTimer, &QTimer::timeout, this, [this]() {
        m_dots = (m_dots + 1) % 4;
        QString dots;
        dots.fill(QChar(u'.'), m_dots);
        m_label->setText(QStringLiteral("Reading") + dots);
    });
    m_dotsTimer->start();

    startShimmer();
}

void WaitingBlock::startShimmer() { m_label->startShimmer(); }
void WaitingBlock::stopShimmer()
{
    m_label->stopShimmer();
    if (m_dotsTimer) m_dotsTimer->stop();
}

// --- ThinkingBlock ---
ThinkingBlock::ThinkingBlock(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    m_toggle = new ShimmerButton(QStringLiteral("Reasoning  \u25b8"), this);
    m_toggle->setObjectName("thinking-toggle");
    m_toggle->setFlat(true);
    m_toggle->setCursor(Qt::PointingHandCursor);
    m_toggle->setStyleSheet(
        "QPushButton#thinking-toggle {"
        "  background: transparent; border: none;"
        "  color: rgba(239,240,241,200);"
        "  font-size: 11px; font-style: italic;"
        "  padding: 2px 4px; text-align: left;"
        "}"
        "QPushButton#thinking-toggle:hover {"
        "  color: rgba(239,240,241,255);"
        "}");
    m_toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_toggle, &QPushButton::clicked, this, [this]() {
        if (m_expanded) collapse(); else expand();
    });
    layout->addWidget(m_toggle);

    m_label = new QLabel(this);
    m_label->setObjectName("thinking-label");
    m_label->setStyleSheet(kContentStyle);
    m_label->setWordWrap(true);
    m_label->setTextFormat(Qt::RichText);
    m_label->setOpenExternalLinks(true);
    m_label->setTextInteractionFlags(kLinkFlags);
    m_label->setVisible(false); // clos par defaut
    layout->addWidget(m_label);
}

void ThinkingBlock::refresh()
{
    m_label->setText(renderMarkdown(m_text));
}

void ThinkingBlock::append(const QString &text)
{
    m_text.append(text);
    refresh();
    m_toggle->startShimmer();
    emit geometryChanged();
}

void ThinkingBlock::set(const QString &text)
{
    m_text = text;
    refresh();
    emit geometryChanged();
}

void ThinkingBlock::collapse()
{
    m_expanded = false;
    m_label->setVisible(false);
    m_toggle->setText(QStringLiteral("Reasoning  \u25b8"));
    m_toggle->stopShimmer();
    emit geometryChanged();
}

void ThinkingBlock::expand()
{
    m_expanded = true;
    m_label->setVisible(true);
    m_toggle->setText(QStringLiteral("Reasoning  \u25be"));
    emit geometryChanged();
}

void ThinkingBlock::startShimmer() { m_toggle->startShimmer(); }
void ThinkingBlock::stopShimmer()  { m_toggle->stopShimmer(); }

// --- ToolBlock ---
ToolBlock::ToolBlock(const QString &name, QWidget *parent)
    : QWidget(parent)
    , m_name(name)
    , m_statusIcon(QStringLiteral("\u23f3")) // sablier tant qu'aucun résultat
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    m_header = new QPushButton(this);
    m_header->setObjectName("tool-header");
    m_header->setFlat(true);
    m_header->setCursor(Qt::PointingHandCursor);
    m_header->setStyleSheet(
        "QPushButton#tool-header {"
        "  background: rgba(255,255,255,18); border: 1px solid rgba(255,255,255,40);"
        "  border-radius: 6px; color: #e9eaee; font-size: 12px;"
        "  padding: 4px 8px; text-align: left;"
        "}"
        "QPushButton#tool-header:hover { background: rgba(255,255,255,32); }");
    m_header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_header, &QPushButton::clicked, this, [this]() { toggle(); });
    layout->addWidget(m_header);

    m_detail = new QLabel(this);
    m_detail->setObjectName("tool-detail");
    m_detail->setStyleSheet(
        "QLabel#tool-detail { color: rgba(239,240,241,180); font-size: 11px;"
        "  font-family: monospace; background: rgba(255,255,255,12);"
        "  border-radius: 6px; padding: 6px 8px; }");
    m_detail->setWordWrap(true);
    m_detail->setTextFormat(Qt::RichText);
    m_detail->setTextInteractionFlags(kSelectFlags);
    m_detail->setVisible(false);
    layout->addWidget(m_detail);

    refreshHeader();
}

void ToolBlock::refreshHeader()
{
    const QChar arrow = m_expanded ? QChar(0x25be) : QChar(0x25b8);
    m_header->setText(QStringLiteral("%1  %2  %3")
                              .arg(m_statusIcon, m_name, QString(arrow)));
}

void ToolBlock::setParams(const QString &json)
{
    m_params = json;
    refreshHeader();
    emit geometryChanged();
}

void ToolBlock::setResult(const QString &result, bool ok)
{
    m_result = result;
    m_statusIcon = ok ? QStringLiteral("\u2713") : QStringLiteral("\u26a0");
    refreshHeader();
    QString html = QStringLiteral("<b>args:</b> %1<br><b>result:</b> %2")
                       .arg(m_params.toHtmlEscaped(), m_result.toHtmlEscaped());
    m_detail->setText(html);
    emit geometryChanged();
}

void ToolBlock::setStatus(const QString &icon)
{
    m_statusIcon = icon;
    refreshHeader();
    emit geometryChanged();
}

void ToolBlock::toggle()
{
    m_expanded = !m_expanded;
    m_detail->setVisible(m_expanded);
    refreshHeader();
    emit geometryChanged();
}

// --- ContentBlock ---
ContentBlock::ContentBlock(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_label = new QLabel(this);
    m_label->setObjectName("content-label");
    m_label->setStyleSheet(kContentStyle);
    m_label->setWordWrap(true);
    m_label->setTextFormat(Qt::RichText);
    m_label->setTextInteractionFlags(kLinkFlags);
    m_label->setOpenExternalLinks(true);
    m_label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_label->setVisible(false);
    layout->addWidget(m_label);
}

void ContentBlock::refresh()
{
    m_label->setText(renderMarkdown(m_text));
    m_label->setVisible(!m_text.isEmpty());
}

void ContentBlock::append(const QString &text)
{
    m_text.append(text);
    refresh();
    emit geometryChanged();
}

void ContentBlock::set(const QString &text)
{
    m_text = text;
    refresh();
    emit geometryChanged();
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
    if (m_layout) m_layout->addWidget(frame);
    emit geometryChanged();
}

void Bubble::clearApproval()
{
    if (m_approvalWidget) {
        m_approvalWidget->deleteLater();
        m_approvalWidget = nullptr;
        emit geometryChanged();
    }
}