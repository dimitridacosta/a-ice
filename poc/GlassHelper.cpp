#include "GlassHelper.h"

#include <KWindowEffects>
#include <KX11Extras>
#include <netwm_def.h>
#include <LayerShellQt/window.h>

#include <cstdio>

#include <QDateTime>
#include <QGuiApplication>
#include <QPainterPath>
#include <QPolygonF>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTimer>
#include <QVector>

namespace {

/// Region arrondie (polygone issu d'un QPainterPath), comme en v1 : le flou
/// epouse la carte, pas le rectangle de la fenetre.
QRegion roundedRegion(const QRect &r, int radius)
{
    if (r.isEmpty())
        return {};
    QPainterPath path;
    path.addRoundedRect(r, radius, radius);
    return QRegion(path.toFillPolygon().toPolygon());
}

/// Marge de securite autour d'une carte avant de la considerer presente.
constexpr int kMinCardSide = 4;

/// Fenetre de log des mesures de frames (nombre de frames).
constexpr qlonglong kLogEvery = 240;

} // namespace

GlassHelper::GlassHelper(QObject *parent)
    : QObject(parent)
{
    m_stress = qEnvironmentVariableIsSet("AICE_STRESS")
        && qEnvironmentVariable("AICE_STRESS") != "0";
    m_pump = qEnvironmentVariableIsSet("AICE_PUMP")
        && qEnvironmentVariable("AICE_PUMP") != "0";
    m_hud = qEnvironmentVariable("AICE_HUD") != "0";
}

void GlassHelper::attach(QQuickWindow *window)
{
    if (m_window || !window)
        return;
    m_window = window;
    window->setTitle(QStringLiteral("a-ice 2.0 — M1 : verre, bulles, barre grab"));

    const QString platformName = QGuiApplication::platformName();

    // Overlay layer-shell (experimental, AICE_LAYER=1) : la fenetre devient une
    // couche du composeur, hors taskbar et au-dessus de tout. Sans ancrage :
    // c'est la position qui decide, ce qui laisse la porte ouverte au deplacement.
    if (platformName == QLatin1String("wayland") && qEnvironmentVariable("AICE_LAYER") == "1") {
        auto *shell = LayerShellQt::Window::get(window);
        shell->setLayer(LayerShellQt::Window::LayerOverlay);
        shell->setAnchors(LayerShellQt::Window::Anchors()); // aucun ancrage
        shell->setExclusiveZone(-1);                        // ne reserve aucune place
        shell->setKeyboardInteractivity(
            LayerShellQt::Window::KeyboardInteractivityOnDemand);
        shell->setScope(QStringLiteral("a-ice-poc"));
        m_layered = true;
    } else if (platformName == QLatin1String("xcb")) {
        // X11 : KeepAbove + pas de taskbar via l'API X11 de KF6.
        KX11Extras::setState(window->winId(), NET::KeepAbove | NET::SkipTaskbar);
    }

    // Position retrouvee de la session precedente (inutile en layer-shell :
    // c'est le composeur qui place la couche).
    if (!m_layered) {
        QSettings s;
        const int x = s.value(QStringLiteral("window/x"), -1).toInt();
        const int y = s.value(QStringLiteral("window/y"), -1).toInt();
        if (x >= 0 && y >= 0)
            window->setPosition(x, y);

        connect(window, &QWindow::xChanged, this, &GlassHelper::persistPosition);
        connect(window, &QWindow::yChanged, this, &GlassHelper::persistPosition);
    }

    connect(window, &QQuickWindow::frameSwapped, this, &GlassHelper::onFrameSwapped);

    // Le verre doit suivre la scene : on le recalcule a chaque frame produite,
    // comme le faisait paintEvent en v1. Les appels sont fusionnes, et rien
    // n'est repose quand la region est identique (anti-clignotement).
    connect(window, &QQuickWindow::frameSwapped, this, &GlassHelper::scheduleRefresh);

    applyRegions();
}

bool GlassHelper::grabWindow()
{
    if (!m_window)
        return false;
    m_moveNative = m_window->startSystemMove();
    emit moveNativeChanged();
    return m_moveNative;
}

void GlassHelper::setClipItem(QQuickItem *item)
{
    m_clip = item;
    scheduleRefresh();
}

void GlassHelper::scheduleRefresh()
{
    if (m_pending || !m_window)
        return;
    m_pending = true;
    // Fusionne les geometries changees de la meme frame en un seul recalcul,
    // juste avant le rendu de la frame suivante.
    QTimer::singleShot(0, this, [this]() {
        m_pending = false;
        applyRegions();
    });
}

bool GlassHelper::isInside(const QQuickItem *item, const QQuickItem *ancestor)
{
    for (const QQuickItem *it = item; it; it = it->parentItem())
        if (it == ancestor)
            return true;
    return false;
}

void GlassHelper::collect(QQuickItem *item, QQuickItem *sceneRoot, QVector<Card> &out) const
{
    if (!item)
        return;

    const QRectF clip = m_clip
        ? QRectF(m_clip->mapToItem(sceneRoot, QPointF(0.0, 0.0)),
                 QSizeF(m_clip->width(), m_clip->height()))
        : QRectF();

    for (QQuickItem *child : item->childItems()) {
        if (child->isVisible() && child->property("glassCard").toBool()
            && child->width() > kMinCardSide && child->height() > kMinCardSide) {

            const QPointF at = child->mapToItem(sceneRoot, QPointF(0.0, 0.0));
            QRect r(QPoint(int(std::lround(at.x())), int(std::lround(at.y()))),
                    QSize(int(std::lround(child->width())), int(std::lround(child->height()))));

            // Une bulle poussee hors de la zone de conversation ne doit pas
            // flouter ni capter les clics pour rien.
            if (m_clip && isInside(child, m_clip))
                r = r.intersected(clip.toRect());

            if (r.width() > kMinCardSide && r.height() > kMinCardSide) {
                Card c;
                c.rect = r;
                c.radius = child->property("glassRadius").toInt();
                out.push_back(c);
            }
        }
        collect(child, sceneRoot, out);
    }
}

void GlassHelper::applyRegions()
{
    if (!m_window)
        return;

    QQuickItem *sceneRoot = m_window->contentItem();
    QVector<Card> cards;
    cards.reserve(64);
    collect(sceneRoot, sceneRoot, cards);

    QRegion region;
    for (const Card &c : cards)
        region = region.united(roundedRegion(c.rect, c.radius));
    region = region.intersected(QRect(0, 0, m_window->width(), m_window->height()));

    if (m_cards != int(cards.size())) {
        m_cards = int(cards.size());
        emit cardsChanged();
        if (qEnvironmentVariable("AICE_DEBUG") == "1") {
            for (const Card &c : cards)
                fprintf(stderr, "a-ice M1 : carte verre %dx%d @ %d,%d r=%d\n",
                        c.rect.width(), c.rect.height(), c.rect.x(), c.rect.y(), c.radius);
        }
    }

    // Anti-clignotement : rien appliquer si la region est identique.
    if (region == m_lastRegion)
        return;
    m_lastRegion = region;

    // Sous Wayland, QWindow::setMask() definit la region de clic du surface :
    // hors des cartes, les clics traversent la fenetre (indispensable en overlay).
    m_window->setMask(region);
    KWindowEffects::enableBlurBehind(m_window, true, region);
}

void GlassHelper::persistPosition()
{
    if (!m_window || m_layered)
        return;
    QSettings s;
    s.setValue(QStringLiteral("window/x"), m_window->x());
    s.setValue(QStringLiteral("window/y"), m_window->y());
}

void GlassHelper::onFrameSwapped()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastSwap == 0) {
        m_lastSwap = now;
        return;
    }

    // Qt Quick ne rend que ce qui change : un ecran fige n'emet pas de frame.
    // On ne mesure donc que l'intervalle entre deux frames reelles.
    const double dt = double(now - m_lastSwap);
    m_lastSwap = now;

    ++m_frames;
    m_frameMs = (m_frames == 1) ? dt : (0.85 * m_frameMs + 0.15 * dt);
    m_frameMax = qMax(m_frameMax, dt);

    if (m_frames % kLogEvery == 0) {
        // fprintf et pas qInfo : qInfo est au niveau debug, filtre par defaut,
        // et ces chiffres doivent sortir dans le terminal sans reglages.
        fprintf(stderr,
                "a-ice M1 : frame time moyen %.2f ms, pic %.2f ms, frames %lld, "
                "cartes dans le verre %d (%dx%d)\n",
                m_frameMs, m_frameMax, m_frames, m_cards,
                m_window ? m_window->width() : 0, m_window ? m_window->height() : 0);
    }
    emit frameMsChanged();
}
