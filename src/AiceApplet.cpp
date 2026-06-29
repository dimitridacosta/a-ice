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
#include <QDebug>
#include <LayerShellQt/Window>

namespace {
class FramelessOverlay : public QMainWindow
{
public:
    explicit FramelessOverlay(QWidget *central)
    {
        setObjectName("aice-window");
        // Layer-shell gère le placement/stacking : on reste frameless + translucide,
        // sans Qt::Tool/StaysOnTop (qui conflictent avec layer-shell).
        setWindowFlags(Qt::FramelessWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setCentralWidget(central);
    }
};
} // namespace

AiceApplet::AiceApplet() {}
AiceApplet::~AiceApplet() {}

void AiceApplet::init()
{
    setupUI();
    setupConnections();
}

void AiceApplet::setupUI()
{
    m_chatWidget = std::make_unique<ChatWidget>();
    m_chatWidget->setObjectName("chat-root");

    m_window = std::make_unique<FramelessOverlay>(m_chatWidget.get());
    m_window->setWindowTitle(QStringLiteral("A-Ice"));
    m_window->resize(460, 600);

    // Raccourcis globaux à la fenêtre.
    auto *quitSc = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Q")),
                                 m_window.get());
    QObject::connect(quitSc, &QShortcut::activated, qApp,
                     []() { QApplication::quit(); });
    auto *hideSc = new QShortcut(QKeySequence(QStringLiteral("Esc")),
                                 m_window.get());
    QObject::connect(hideSc, &QShortcut::activated, m_window.get(),
                     [this]() { m_window->hide(); });
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

void AiceApplet::placeWindow()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen) screen = QGuiApplication::screens().value(0);
    if (!screen) return;

    const QRect screenGeo = screen->geometry();
    const QRect avail = screen->availableGeometry(); // exclut le panel KDE
    // Hauteur du panel (supposé en bas) = espace entre le bas dispo et le bas écran.
    const int panelHeight = qMax(0, screenGeo.bottom() - avail.bottom());

    const int top = 0; // pas de marge layer-shell en haut : la marge interne
                     // du layout messages (13px) laisse déjà place pour l'ombre
    const int right = 6;
    const int bottom = panelHeight; // colle la fenêtre au panel KDE

    QMainWindow *w = m_window.get();
    // Largeur fixe (hauteur imposée par l'ancrage Top+Bottom).
    w->resize(460, screenGeo.height() - top - bottom);

    // Force la création du QWindow natif pour attacher le layer-shell avant map.
    (void)w->winId();
    QWindow *qw = w->windowHandle();
    if (auto *shell = LayerShellQt::Window::get(qw)) {
        shell->setLayer(LayerShellQt::Window::LayerBottom);
        const auto anchors = static_cast<LayerShellQt::Window::Anchor>(
            LayerShellQt::Window::AnchorRight
            | LayerShellQt::Window::AnchorTop
            | LayerShellQt::Window::AnchorBottom);
        shell->setAnchors(LayerShellQt::Window::Anchors(anchors));
        // QMargins(left, top, right, bottom)
        shell->setMargins(QMargins(0, top, right, bottom));
        shell->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
        shell->setActivateOnShow(false);
        shell->setScope(QStringLiteral("a-ice"));
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

bool AiceApplet::handleShortcut(QKeyEvent *e)
{
    Q_UNUSED(e)
    return false;
}