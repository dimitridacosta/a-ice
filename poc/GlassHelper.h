#pragma once

#include <QObject>
#include <QRegion>
#include <QtGlobal>

class QQuickItem;
class QQuickWindow;

/**
 * Colle C++ du POC M1 : le verre (flou KWin + region de clic) et la mesure de
 * frames, pour une fenetre Qt Quick.
 *
 * Idee cle, identique a la v1 : la fenetre est totalement translucide, seul le
 * verre existe. Le flou KWin et la region qui capte les clics ne couvrent QUE
 * les cartes dessinees (bulles + barre de prompt), pas la fenetre entiere.
 * On va donc cherche les cartes directement dans l'arbre QML : tout item qui
 * declare `property bool glassCard: true` (et `glassRadius`) entre dans la
 * region.
 */
class GlassHelper : public QObject
{
    Q_OBJECT
    /// Temps de frame moyen (moyissante exponentielle), en ms.
    Q_PROPERTY(double frameMs READ frameMs NOTIFY frameMsChanged)
    /// Pic de temps de frame observe depuis le lancement, en ms.
    Q_PROPERTY(double frameMax READ frameMax NOTIFY frameMsChanged)
    /// Total de frames affichees (indicatif).
    Q_PROPERTY(qlonglong frames READ frames NOTIFY frameMsChanged)
    /// Nombre de cartes actuellement incluses dans le verre (bulles visibles + barre).
    Q_PROPERTY(int cards READ cards NOTIFY cardsChanged)
    /// true si le deplacement natif du composeur a fonctionne, false si le
    /// fallback QML (deplacement par deltas) a pris le relais.
    Q_PROPERTY(bool moveNative READ moveNative NOTIFY moveNativeChanged)
    /// Mode stress : 2000 bulles factices (AICE_STRESS=1).
    Q_PROPERTY(bool stress READ stress CONSTANT)
    /// Bourrage de l'affichage : redessine en continu pour mesurer le frame
    /// time sans interaction (AICE_PUMP=1).
    Q_PROPERTY(bool pump READ pump CONSTANT)
    /// Affichage des mesures dans la fenetre (AICE_HUD=0 pour cacher).
    Q_PROPERTY(bool hud READ hud CONSTANT)

public:
    explicit GlassHelper(QObject *parent = nullptr);

    /// Branche le verre sur la fenetre QML : titre, overlay eventuel,
    /// persistance de la position, mesure des frames, premiere region de verre.
    Q_INVOKABLE void attach(QQuickWindow *window);

    /// Attrape la fenetre : deplacement natif KWin/Wayland si le composeur le
    /// sait faire. Renvoie false si c'est au QML de deplacer a la main.
    Q_INVOKABLE bool grabWindow();

    /// Les cartes filles de cet item sont retranchees de sa zone (le ListView) :
    /// une bulle sortie du viewport ne doit ni flouter ni capter les clics.
    Q_INVOKABLE void setClipItem(QQuickItem *item);

    /// Recalcule la region de verre (les appels multiples sont fusionnes).
    Q_INVOKABLE void scheduleRefresh();

    double frameMs() const { return m_frameMs; }
    double frameMax() const { return m_frameMax; }
    qlonglong frames() const { return m_frames; }
    int cards() const { return m_cards; }
    bool moveNative() const { return m_moveNative; }
    bool stress() const { return m_stress; }
    bool pump() const { return m_pump; }
    bool hud() const { return m_hud; }

signals:
    void frameMsChanged();
    void cardsChanged();
    void moveNativeChanged();

private:
    struct Card {
        QRect rect;
        int radius;
    };

    void applyRegions();
    void collect(QQuickItem *item, QQuickItem *sceneRoot, QVector<Card> &out) const;
    static bool isInside(const QQuickItem *item, const QQuickItem *ancestor);
    void persistPosition();
    void onFrameSwapped();

    QQuickWindow *m_window = nullptr;
    QQuickItem *m_clip = nullptr;      // viewport a respecter (le ListView)
    QRegion m_lastRegion;              // pour ne rien reappliquer si identique
    bool m_pending = false;            // refresh deja programme
    bool m_layered = false;            // overlay layer-shell (AICE_LAYER=1)
    bool m_moveNative = false;
    bool m_stress = false;
    bool m_pump = false;
    bool m_hud = true;
    int m_cards = 0;

    // Mesure des frames.
    qlonglong m_frames = 0;
    qint64 m_lastSwap = 0;
    double m_frameMs = 0.0;
    double m_frameMax = 0.0;
};
