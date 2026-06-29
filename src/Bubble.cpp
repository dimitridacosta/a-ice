#include "Bubble.h"
#include "Glass.h"
#include <QVBoxLayout>
#include <QPushButton>
#include <QPainter>
#include <QResizeEvent>
#include <QTimer>
#include <QLinearGradient>

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
    // kGlassInset n'affecte que le paintEvent de la carte, pas le label.
    const int hPad = 16 + 16; // 32
    const int vPad = 12 + 12; // 24
    const int maxTextW = maxBubbleWidth - hPad;
    if (maxTextW <= 0) return;

    // Police identique au stylesheet (font-size: 13px)
    QFont f = m_contentLabel->font();
    f.setPixelSize(13);
    QFontMetrics fm(f);

    // Mesure le texte avec word-wrap à la largeur max
    QRect br = fm.boundingRect(QRect(0, 0, maxTextW, 99999),
                               Qt::TextWordWrap, m_contentLabel->text());

    int bubbleW = qMin(br.width() + hPad, maxBubbleWidth);
    int bubbleH = br.height() + vPad;

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
    m_contentLabel->setTextFormat(Qt::PlainText);
    m_contentLabel->setTextInteractionFlags(kSelectFlags);
    m_contentLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
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
    m_thinkingLabel->setTextFormat(Qt::PlainText);
    m_thinkingLabel->setTextInteractionFlags(kSelectFlags);
    m_thinkingLabel->setVisible(false); // clos par défaut
    layout->addWidget(m_thinkingLabel);

    // Contenu de la réponse finale.
    m_contentLabel = new QLabel(this);
    m_contentLabel->setObjectName("content-label");
    m_contentLabel->setStyleSheet(kContentStyle);
    m_contentLabel->setWordWrap(true);
    m_contentLabel->setTextFormat(Qt::PlainText);
    m_contentLabel->setTextInteractionFlags(kSelectFlags);
    m_contentLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    layout->addWidget(m_contentLabel);

    m_contentLabel->setVisible(false);
}

void Bubble::refreshThinkingLabel()
{
    if (!m_thinkingLabel) return;
    // PlainText : pas d'escaping HTML, les \n sont affichés directement.
    m_thinkingLabel->setText(m_thinking);
}

void Bubble::refreshContentLabel()
{
    if (!m_contentLabel) return;
    m_contentLabel->setText(m_content);
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