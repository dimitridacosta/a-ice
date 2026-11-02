#include "GlassHelper.h"

#include <KWindowEffects>
#include <KX11Extras>
#include <netwm_def.h>
#include <LayerShellQt/window.h>

#include <cstdio>

#include <QDateTime>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QString>
#include <QStringList>
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

/*
 * Les trois modes de fenetre du corps :
 *   fenetre   : une fenetre ordinaire, qui peut se faire doubler et ne vit que sur
 *               le bureau ou elle a ete posee — ce que la v2 fait aujourd'hui ;
 *   au-dessus : gardee devant les autres fenetres ;
 *   fixe      : au-dessus de tout, et posee sur tous les bureaux — le geste de la
 *               v1 (couche du composeur, anchors, hors taskbar).
 */
const QStringList kModeNoms = { QStringLiteral("fenetre"),
                                QStringLiteral("au-dessus"),
                                QStringLiteral("fixe") };

bool estCouche(const QString &mode)
{
    return !mode.isEmpty() && mode != kModeNoms.at(0);
}

/// Le mode tel que le corps le comprend, vide si ce n'est pas un mode connu.
/// Les variantes acceptees sont celles qu'on pourrait taper de bonne foi.
QString normaliserMode(const QString &brut)
{
    const QString a = brut.trimmed().toLower();
    if (a.isEmpty())
        return {};
    if (a == QLatin1String("fenetre") || a == QLatin1String("normal")
        || a == QLatin1String("0"))
        return kModeNoms.at(0);
    if (a == QLatin1String("au-dessus") || a == QLatin1String("au dessus")
        || a == QLatin1String("dessus") || a == QLatin1String("above")
        || a == QLatin1String("1"))
        return kModeNoms.at(1);
    if (a == QLatin1String("fixe") || a == QLatin1String("fix")
        || a == QLatin1String("sticky") || a == QLatin1String("pin")
        || a == QLatin1String("layer") || a == QLatin1String("2"))
        return kModeNoms.at(2);
    return {};
}

/// Fond de controle pour les captures : damier gris clair + bandes. Une ombre est
/// vue sur le bureau floute par KWin a l'usage ; hors ecran, ce fond sert de
/// reference pour lire le degrade (ou sa coupure) a l'oeil, et rendre visible
/// une carte translucide.
void paintBackdrop(QPainter &painter, const QSize &size)
{
    const int cell = 80;
    for (int y = 0; y < size.height(); y += cell) {
        for (int x = 0; x < size.width(); x += cell) {
            const bool lit = ((x / cell) + (y / cell)) % 2 == 0;
            painter.fillRect(QRect(x, y, cell, cell),
                             QColor(lit ? QStringLiteral("#9fb0c0")
                                        : QStringLiteral("#7c8d9d")));
        }
    }

    // Deux bandes tres claires : une ombre mal graduee y apparait en marches.
    painter.fillRect(QRect(0, 60, size.width(), 14), QColor(QStringLiteral("#f2f6fa")));
    painter.fillRect(QRect(0, size.height() - 150, size.width(), 14),
                     QColor(QStringLiteral("#f2f6fa")));
}

} // namespace

GlassHelper::GlassHelper(QObject *parent)
    : QObject(parent)
{
    m_stress = qEnvironmentVariableIsSet("AICE_STRESS")
        && qEnvironmentVariable("AICE_STRESS") != "0";
    m_pump = qEnvironmentVariableIsSet("AICE_PUMP")
        && qEnvironmentVariable("AICE_PUMP") != "0";
    m_hud = qEnvironmentVariable("AICE_HUD") != "0";
    m_shadow = qEnvironmentVariable("AICE_SHADOW") != "0";

    /*
     * Le mode de fenetre se regle par /fenetre dans la barre (ou un clic dans la
     * carte du hub) et survit aux lancements : la meme idee que voice/enabled.
     * AICE_WINDOW_MODE=<mode> force le mode sans le persister (knob de test).
     */
    QString regle = qEnvironmentVariable("AICE_WINDOW_MODE");
    if (regle.trimmed().isEmpty()) {
        // AICE_LAYER=1 etait le bouton experimental du POC : il vaut « fixe ».
        regle = qEnvironmentVariable("AICE_LAYER") == "1"
            ? QStringLiteral("fixe")
            : QSettings(QStringLiteral("a-ice"), QStringLiteral("a-ice"))
                  .value(QStringLiteral("window/mode"))
                  .toString();
    }
    m_mode = normaliserMode(regle);
    if (m_mode.isEmpty())
        m_mode = kModeNoms.at(0);
}

void GlassHelper::attach(QQuickWindow *window)
{
    if (m_window || !window)
        return;
    m_window = window;
    window->setTitle(QStringLiteral("a-ice 2.0 — M1 : verre, bulles, barre grab"));

    const QString platformName = QGuiApplication::platformName();

    /*
     * Les modes qui ne sont pas une fenetre ordinaire passent par une couche du
     * composeur (le geste de la v1) : la surface est posee par le composeur, hors
     * taskbar, et ne depend plus d'un bureau. Sans ancrage : c'est la position qui
     * decide, ce qui laisse la porte ouverte au deplacement.
     *
     * « au-dessus » = couche au-dessus des fenetres ; « fixe » = couche par-dessus
     * tout, y compris les applis en plein ecran.
     */
    if (platformName == QLatin1String("wayland") && estCouche(m_mode)) {
        auto *shell = LayerShellQt::Window::get(window);
        shell->setLayer(m_mode == kModeNoms.at(2) ? LayerShellQt::Window::LayerOverlay
                                                  : LayerShellQt::Window::LayerTop);
        shell->setAnchors(LayerShellQt::Window::Anchors()); // aucun ancrage
        shell->setExclusiveZone(-1);                        // ne reserve aucune place
        shell->setKeyboardInteractivity(
            LayerShellQt::Window::KeyboardInteractivityOnDemand);
        shell->setScope(QStringLiteral("a-ice"));
        m_layered = true;
    } else if (platformName == QLatin1String("xcb")) {
        // X11 : pas de couche, mais KeepAbove et — pour « fixe » — Sticky (posee
        // sur tous les bureaux), plus SkipTaskbar dans les trois modes.
        NET::States etats = NET::SkipTaskbar;
        if (estCouche(m_mode))
            etats |= NET::KeepAbove;
        if (m_mode == kModeNoms.at(2))
            etats |= NET::Sticky;
        KX11Extras::setState(window->winId(), etats);
    }

    // Ce que le composeur a vraiment pose : ce que Dimitri regarde, et ce que les
    // logs relancent quand un mode ne fait pas ce qu'on attendait.
    fprintf(stderr, "a-ice M1 : fenetre en mode « %s » sur %s — %s\n",
            qUtf8Printable(m_mode), qPrintable(platformName),
            m_layered ? "couche du composeur" : "fenetre ordinaire");

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

    // Capture de controle (AICE_SHOT=/chemin.png) : permet de voir le rendu sans
    // etre devant l'ecran. AICE_SHOT_QUIT=1 ferme l'appli juste apres.
    // AICE_SHOT_MS=<millisecondes> fixe le delai : utile pour capturer pendant un
    // tour du hub, la ou la reponse est encore en train de couler.
    if (qEnvironmentVariableIsSet("AICE_SHOT")) {
        const QString path = qEnvironmentVariable("AICE_SHOT");
        int delay = qEnvironmentVariableIntValue("AICE_SHOT_MS");
        if (delay <= 0)
            delay = 900;
        QTimer::singleShot(delay, this, [this, path]() {
            grabShot(path);
            if (qEnvironmentVariable("AICE_SHOT_QUIT") == "1")
                QGuiApplication::quit();
        });
    }
}

/*
 * Le mode de fenetre demande. Il est persiste dans tous les cas : c'est le reglage
 * qui vaudra au prochain lancement.
 *
 * Ce qui se peut en cours de route : changer de rang de couche (au-dessus -> fixe),
 * et reposer les etats X11 d'une fenetre ordinaire. Ce qui ne se peut pas : virer
 * une fenetre ordinaire en couche du composeur, ni l'inverse — le composeur choisit
 * la surface a sa creation, pas apres. La, on laisse windowModeRestart le dire.
 */
void GlassHelper::setWindowMode(const QString &mode)
{
    const QString prochain = normaliserMode(mode);
    if (prochain.isEmpty()) {
        fprintf(stderr, "a-ice M1 : mode de fenetre « %s » inconnu (attendu : %s)\n",
                qUtf8Printable(mode.trimmed()),
                qUtf8Printable(kModeNoms.join(QStringLiteral(", "))));
        return;
    }
    if (prochain == m_mode)
        return;

    const bool coucheAvant = estCouche(m_mode);
    const bool coucheApres = estCouche(prochain);
    m_mode = prochain;
    QSettings(QStringLiteral("a-ice"), QStringLiteral("a-ice"))
        .setValue(QStringLiteral("window/mode"), m_mode);

    if (m_window) {
        if (coucheAvant != coucheApres) {
            m_modeRestart = true;
            fprintf(stderr, "a-ice M1 : mode « %s » retenu — relancer le corps pour l'appliquer\n",
                    qUtf8Printable(m_mode));
        } else if (coucheApres && m_layered) {
            // Deux couches : le composeur sait changer de rang sans recreer la surface.
            LayerShellQt::Window::get(m_window)->setLayer(
                m_mode == kModeNoms.at(2) ? LayerShellQt::Window::LayerOverlay
                                          : LayerShellQt::Window::LayerTop);
            m_modeRestart = false;
        } else if (QGuiApplication::platformName() == QLatin1String("xcb")) {
            NET::States etats = NET::SkipTaskbar;
            if (coucheApres)
                etats |= NET::KeepAbove;
            if (m_mode == kModeNoms.at(2))
                etats |= NET::Sticky;
            KX11Extras::setState(m_window->winId(), etats);
            m_modeRestart = false;
        }
    }

    fprintf(stderr, "a-ice M1 : fenetre en mode « %s »\n", qUtf8Printable(m_mode));
    emit windowModeChanged();
}

bool GlassHelper::startMove()
{
    if (!m_window)
        return false;
    m_moveNative = m_window->startSystemMove();
    emit moveNativeChanged();
    return m_moveNative;
}

bool GlassHelper::startResize(int edges)
{
    if (!m_window)
        return false;
    // Le composeur ne sait redimensionner que depuis des bords qu'on lui designe :
    // ici ceux qui grandissent vers la droite et vers le bas, la ou se trouve la
    // poignee. Les bords haut et gauche restent fixes (la barre ne bouge pas).
    m_resizeNative = m_window->startSystemResize(Qt::Edges(edges));
    emit resizeNativeChanged();
    return m_resizeNative;
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

void GlassHelper::collect(QQuickItem *item, QQuickItem *sceneRoot, QVector<Card> &out,
                          int &excluded, int &candidates) const
{
    if (!item)
        return;

    const QRectF clip = m_clip
        ? QRectF(m_clip->mapToItem(sceneRoot, QPointF(0.0, 0.0)),
                 QSizeF(m_clip->width(), m_clip->height()))
        : QRectF();

    for (QQuickItem *child : item->childItems()) {
        if (child->isVisible() && child->property("glassCard").toBool()) {
            ++candidates;
        }

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
                // L'alpha effectif, en remontant jusqu'a la scene : une carte que
                // le fade rend invisible ne dessine rien, donc elle n'a rien a
                // faire dans le verre (ni flou, ni captation des clics).
                qreal alpha = child->opacity();
                for (const QQuickItem *p = child->parentItem(); p && p != sceneRoot; p = p->parentItem())
                    alpha *= p->opacity();

                if (alpha < 0.01) {
                    // Rien de dessine : rien dans le verre (ni flou, ni clics).
                    ++excluded;
                    continue;
                }

                Card c;
                c.rect = r;
                c.radius = child->property("glassRadius").toInt();
                c.alpha = alpha;
                out.push_back(c);
            }
        }
        collect(child, sceneRoot, out, excluded, candidates);
    }
}

void GlassHelper::applyRegions()
{
    if (!m_window)
        return;

    QQuickItem *sceneRoot = m_window->contentItem();
    QVector<Card> cards;
    cards.reserve(64);
    int excluded = 0;
    int candidates = 0;
    collect(sceneRoot, sceneRoot, cards, excluded, candidates);

    QRegion region;
    for (const Card &c : cards)
        region = region.united(roundedRegion(c.rect, c.radius));
    region = region.intersected(QRect(0, 0, m_window->width(), m_window->height()));

    m_lastCards = cards;

    // Le compte d'exclusions est une information distincte : elle change sans que
    // le nombre de cartes change (une bulle que le fade eteint ne fait plus partie
    // du verre, mais le verre en compte toujours d'autres).
    if (qEnvironmentVariable("AICE_DEBUG") == "1" && excluded != m_excludedShown) {
        m_excludedShown = excluded;
        fprintf(stderr, "a-ice M1 : bulles exclues du verre (alpha trop faible) : %d\n", excluded);
    }

    if (m_cards != int(cards.size())) {
        m_cards = int(cards.size());
        emit cardsChanged();
    }

    // Diagnostic : l etat complet du verre. Le compte de cartes ne suffit pas —
    // une carte ecartee pour sa taille, son clip ou son alpha laisse alors
    // aucune trace. Le candidat, c est ce qui se dit carte de verre.
    const long signature = long(candidates) * 1000000L + long(excluded) * 1000L
                           + long(cards.size());
    if (qEnvironmentVariable("AICE_DEBUG") == "1" && signature != m_verreShown) {
        m_verreShown = signature;
        fprintf(stderr, "a-ice M1 : verre %d carte(s) sur %d candidate(s), %d excluse(s)\n",
                int(cards.size()), candidates, excluded);
        for (const Card &c : cards)
            fprintf(stderr, "a-ice M1 : carte verre %dx%d @ %d,%d r=%d alpha=%.2f\n",
                    c.rect.width(), c.rect.height(), c.rect.x(), c.rect.y(), c.radius, c.alpha);
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

void GlassHelper::grabShot(const QString &path)
{
    if (!m_window)
        return;

    const QSize size(int(m_window->width()), int(m_window->height()));
    const QImage scene = m_window->grabWindow();

    QImage out(size, QImage::Format_ARGB32);
    QPainter painter(&out);
    paintBackdrop(painter, size);
    if (!scene.isNull())
        painter.drawImage(0, 0, scene);

    // AICE_DEBUG=1 : les cartes de verre sont entourees, pour lire ou sont les
    // cartes et comment l'ombre deborde (ou pas) au-dela d'elles.
    if (qEnvironmentVariable("AICE_DEBUG") == "1") {
        painter.setPen(QPen(QColor(QStringLiteral("#ff2fd0")), 1));
        painter.setBrush(Qt::NoBrush);
        for (const Card &c : m_lastCards)
            painter.drawRoundedRect(c.rect, c.radius, c.radius);

        // La capture est le moment ou lire l etat du verre : sans ces lignes on ne
        // sait pas si les bulles y entrent vraiment.
        fprintf(stderr, "a-ice M1 : capture sur %d carte(s) de verre\n", int(m_lastCards.size()));
        for (const Card &c : m_lastCards)
            fprintf(stderr, "a-ice M1 : carte verre capture %dx%d @ %d,%d r=%d alpha=%.2f\n",
                    c.rect.width(), c.rect.height(), c.rect.x(), c.rect.y(), c.radius, c.alpha);
    }
    painter.end();

    if (out.save(path))
        fprintf(stderr, "a-ice M1 : capture %s\n", qUtf8Printable(path));
    else
        fprintf(stderr, "a-ice M1 : capture impossible dans %s\n", qUtf8Printable(path));
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
