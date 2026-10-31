#include "AiceApplet.h"
#include "ChatWidget.h"
#include "llama_client.h"
#include <QVBoxLayout>
#include <QScreen>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QApplication>
#include <QShowEvent>
#include <QShortcut>
#include <QAction>
#include <QDebug>
#include <functional>
#include <KGlobalAccel>
#include <LayerShellQt/Window>

namespace {
class FramelessOverlay : public QMainWindow
{
public:
    std::function<void()> onClick;
    explicit FramelessOverlay(QWidget *central)
    {
        setObjectName("aice-window");
        // Layer-shell gere le placement/stacking : on reste frameless + translucide,
        // sans Qt::Tool/StaysOnTop (qui conflictent avec layer-shell).
        setWindowFlags(Qt::FramelessWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setCentralWidget(central);
    }
protected:
    bool eventFilter(QObject *w, QEvent *e) override
    {
        // Clic n importe ou sur le chat -> ramene devant (si on etait en arriere-plan).
        if (e->type() == QEvent::MouseButtonPress && onClick)
            onClick();
        return QMainWindow::eventFilter(w, e);
    }
};
} // namespace

AiceApplet::AiceApplet() {}
AiceApplet::~AiceApplet() {}

void AiceApplet::init()
{
    setupUI();
    setupConnections();
    setupGlobalShortcut();
}

void AiceApplet::setupUI()
{
    m_chatWidget = std::make_unique<ChatWidget>();
    m_chatWidget->setObjectName("chat-root");

    m_window = std::make_unique<FramelessOverlay>(m_chatWidget.get());
    m_window->setWindowTitle(QStringLiteral("A-Ice"));
    m_window->resize(460, 600);
    // Clic sur le chat -> ramene l overlay devant (utile si raccourci global
    // indisponible : la fenetre reste visible derriere et cliquable).
    static_cast<FramelessOverlay *>(m_window.get())->onClick = [this]() {
        if (m_hidden) bringToFront();
    };
    m_chatWidget->installEventFilter(m_window.get());

    // Raccourcis globaux à la fenêtre.
    auto *quitSc = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Q")),
                                 m_window.get());
    QObject::connect(quitSc, &QShortcut::activated, qApp,
                     []() { QApplication::quit(); });
    auto *hideSc = new QShortcut(QKeySequence(QStringLiteral("Esc")),
                                 m_window.get());
    QObject::connect(hideSc, &QShortcut::activated, m_window.get(),
                     [this]() { toggleVisibility(); });
}

void AiceApplet::setupGlobalShortcut()
{
    // Raccourci global KDE : fonctionne depuis n'importe quelle app ayant le
    // focus, meme si a-ice est cachee hors ecran. Mecanisme standard KDE
    // (KRunner, Spectacle...). Voir ROADMAP overlay OS-level.
    auto *toggleAct = new QAction(m_window.get());
    toggleAct->setObjectName(QStringLiteral("toggle-overlay"));
    toggleAct->setText(QStringLiteral("Toggle A-Ice overlay"));
    // Ctrl+Win+Espace : modificateurs Control + Meta + touche Espace.
    const QKeySequence ks(Qt::META | Qt::Key_Return);
    // NoAutoloading : FORCE le raccourci passe, sans recharger la config KDE
    // persistee (sinon Autoloading ignore notre valeur et garde l'ancien
    // binding = bug apres plusieurs changements de raccourci pendant le dev).
    const bool ok1 = KGlobalAccel::self()->setDefaultShortcut(
        toggleAct, {ks}, KGlobalAccel::NoAutoloading);
    const bool ok2 = KGlobalAccel::self()->setShortcut(
        toggleAct, {ks}, KGlobalAccel::NoAutoloading);
    qInfo() << "[a-ice] raccourci global enregistre :" << ks.toString()
             << "(component=" << QCoreApplication::applicationName()
             << ") default=" << ok1 << "current=" << ok2;
    QObject::connect(toggleAct, &QAction::triggered, m_window.get(),
                     [this]() {
                         qInfo() << "[a-ice] global shortcut triggered";
                         toggleVisibility();
                     });
}

void AiceApplet::setupConnections()
{
    // Le ChatWidget gère ses propres événements.
}

void AiceApplet::loadConfig(const QString &path)
{
    m_config.load(path);
    m_chatWidget->applyConfig(m_config);
}

void AiceApplet::overrideServerUrl(const QString &url)
{
    m_chatWidget->setServerUrl(url);
}

void AiceApplet::applyMargins()
{
    if (!m_shell)
        return;
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen) screen = QGuiApplication::screens().value(0);
    if (!screen) return;

    const QRect screenGeo = screen->geometry();
    // Hauteur fixe du panel KDE (48px, mesuree via DBus Plasma). On ne
    // suit plus availableGeometry en live : le resultat etait instable.
    const int top = 0;
    const int right = 0;
    const int bottom = 48;

    QMainWindow *w = m_window.get();
    w->resize(460, screenGeo.height() - top - bottom);
    m_shell->setMargins(QMargins(0, top, right, bottom));
}

void AiceApplet::placeWindow()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen) screen = QGuiApplication::screens().value(0);
    if (!screen) return;

    QMainWindow *w = m_window.get();
    // Force la création du QWindow natif pour attacher le layer-shell avant map.
    (void)w->winId();
    QWindow *qw = w->windowHandle();
    if (auto *shell = LayerShellQt::Window::get(qw)) {
        shell->setLayer(LayerShellQt::Window::LayerTop);
        const auto anchors = static_cast<LayerShellQt::Window::Anchor>(
            LayerShellQt::Window::AnchorRight
            | LayerShellQt::Window::AnchorTop
            | LayerShellQt::Window::AnchorBottom);
        shell->setAnchors(LayerShellQt::Window::Anchors(anchors));
        shell->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
        shell->setActivateOnShow(false);
        shell->setScope(QStringLiteral("a-ice"));
        m_shell = shell;
        applyMargins();
    }
}

void AiceApplet::show()
{
    placeWindow();
    m_window->show();
}

void AiceApplet::closeApp()
{
    m_window->close();
}

void AiceApplet::toggleVisibility()
{
    if (!m_shell)
        return;  // placeWindow pas encore appele.

    // Toggle to back : alterne LayerTop (devant) / LayerBottom (derriere).
    // La fenetre reste VISIBLE (derriere, comme un wallpaper) -> on peut
    // la ramener en cliquant dessus (eventFilter sur toute la fenetre).
    // On n'utilise PAS hide() : sans raccourci global fiable, hide = app
    // perdue. setLayer garde la surface mappee et cliquable.
    if (!m_hidden) {
        m_shell->setLayer(LayerShellQt::Window::LayerBottom);
        m_shell->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        m_hidden = true;
    } else {
        bringToFront();
    }
}

void AiceApplet::bringToFront()
{
    if (!m_shell)
        return;
    m_shell->setLayer(LayerShellQt::Window::LayerTop);
    m_shell->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
    if (auto *qw = m_window->windowHandle())
        qw->requestActivate();
    m_window->raise();
    m_chatWidget->refocusPrompt();
    m_hidden = false;
}

bool AiceApplet::handleShortcut(QKeyEvent *e)
{
    Q_UNUSED(e)
    return false;
}