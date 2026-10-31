#include <QCommandLineParser>
#include <QDateTime>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>

#include <KWindowEffects>
#include <KX11Extras>
#include <netwm.h>
#include <LayerShellQt/window.h>

// Colle C++ du POC : déplacement système, blur KWin, position persistée, frame time.
class PocHelper : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double frameMs READ frameMs NOTIFY frameMsChanged)
    Q_PROPERTY(int windowWidth READ windowWidth CONSTANT)
    Q_PROPERTY(int windowHeight READ windowHeight CONSTANT)

public:
    explicit PocHelper(int heightOverride = 0, QObject *parent = nullptr)
        : QObject(parent)
    {
        QSettings s;
        m_x = s.value(QStringLiteral("window/x"), -1).toInt();
        m_y = s.value(QStringLiteral("window/y"), -1).toInt();
        m_windowWidth = s.value(QStringLiteral("window/width"), 900).toInt();
        m_windowHeight = s.value(QStringLiteral("window/height"), 0).toInt(); // 0 = plein écran
        if (heightOverride > 0)
            m_windowHeight = heightOverride;
    }

    double frameMs() const { return m_frameMs; }
    int windowWidth() const { return m_windowWidth; }
    int windowHeight() const { return m_windowHeight; }

    // Appelé depuis QML une fois la fenêtre créée (Component.onCompleted).
    Q_INVOKABLE void attach(QQuickWindow *window)
    {
        if (m_attached || !window)
            return;
        m_attached = true;

        QSettings s;
        if (m_x >= 0)
            window->setPosition(m_x, m_y);
        window->setTitle(QStringLiteral("a-ice 2.0 — POC"));

        if (qEnvironmentVariable("AICE_LAYER") == "1") {
            // Couche overlay via wlr-layer-shell (Wayland/KWin) : top, ancré aux 4 bords, clavier à la demande.
            LayerShellQt::Window::Anchors anchors;
            anchors.setFlag(LayerShellQt::Window::AnchorTop);
            anchors.setFlag(LayerShellQt::Window::AnchorBottom);
            anchors.setFlag(LayerShellQt::Window::AnchorLeft);
            anchors.setFlag(LayerShellQt::Window::AnchorRight);
            auto *layer = LayerShellQt::Window::get(window);
            layer->setLayer(LayerShellQt::Window::LayerTop);
            layer->setAnchors(anchors);
            layer->setExclusiveZone(-1);
            layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
            layer->setScope(QStringLiteral("a-ice-poc"));
        } else if (QGuiApplication::platformName() == QStringLiteral("xcb")) {
            // Sous X11, KeepAbove/SkipTaskbar via l'API X11 de KF6 (sous Wayland,
            // l'overlay passe par le layer shell : AICE_LAYER=1).
            KX11Extras::setState(window->winId(), NET::KeepAbove);
            KX11Extras::setState(window->winId(), NET::SkipTaskbar);
        }
        KWindowEffects::enableBlurBehind(window, true);

        // Persistance de la position à chaque déplacement/resize.
        connect(window, &QWindow::xChanged, this, [this, window] { persist(window); });
        connect(window, &QWindow::yChanged, this, [this, window] { persist(window); });
        connect(window, &QWindow::widthChanged, this, [this, window] { persist(window); });
        connect(window, &QWindow::heightChanged, this, [this, window] { persist(window); });

        // Frame time : moyenne glissante sur 180 frames, log stderr toutes les 600 frames.
        connect(window, &QQuickWindow::frameSwapped, this, &PocHelper::onFrameSwapped);
    }

    Q_INVOKABLE void startSystemMove(QQuickWindow *window)
    {
        if (window)
            window->startSystemMove();
    }

signals:
    void frameMsChanged();

private:
    void persist(QQuickWindow *window)
    {
        QSettings s;
        s.setValue(QStringLiteral("window/x"), window->x());
        s.setValue(QStringLiteral("window/y"), window->y());
        s.setValue(QStringLiteral("window/width"), window->width());
        s.setValue(QStringLiteral("window/height"), window->height());
    }

    void onFrameSwapped()
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_lastFrameSwap > 0) {
            const double dt = double(now - m_lastFrameSwap);
            if (m_count == 0) {
                m_min = m_max = m_sum = dt;
            } else {
                m_min = qMin(m_min, dt);
                m_max = qMax(m_max, dt);
                m_sum += dt;
            }
            if (++m_count >= 180) {
                m_frameMs = m_sum / m_count;
                emit frameMsChanged();
                if (++m_logCycles == 600) {
                    qInfo("frame time: avg %.2f ms, min %.2f ms, max %.2f ms",
                          m_frameMs,
                          m_min,
                          m_max);
                    m_logCycles = 0;
                }
                m_count = 0;
                m_lastFrameSwap = now;
                return;
            }
        }
        m_lastFrameSwap = now;
    }

    bool m_attached = false;
    int m_x = -1;
    int m_y = -1;
    int m_windowWidth = 900;
    int m_windowHeight = 0;
    double m_frameMs = 0.0;
    double m_min = 0.0;
    double m_max = 0.0;
    double m_sum = 0.0;
    int m_count = 0;
    int m_logCycles = 0;
    qint64 m_lastFrameSwap = 0;
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("a-ice"));
    QGuiApplication::setApplicationName(QStringLiteral("poc-v2"));

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption heightOpt(QStringLiteral("height"),
                                 QStringLiteral("Hauteur de la zone en px (absent = plein écran)"),
                                 QStringLiteral("h"),
                                 QString());
    parser.addOption(heightOpt);
    parser.process(app);

    PocHelper helper(parser.value(heightOpt).toInt());

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("helper"), &helper);
    engine.load(QUrl(QStringLiteral("qrc:/poc/qml/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;

    // helper.attach(window) est appelé depuis QML (Component.onCompleted).
    return app.exec();
}

#include "main.moc"