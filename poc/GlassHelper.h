#pragma once

#include <QObject>
#include <QRegion>
#include <QVector>
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
    /// true si le redimensionnement natif du composeur a fonctionne, false si le
    /// fallback QML (ajustement par deltas) a pris le relais.
    Q_PROPERTY(bool resizeNative READ resizeNative NOTIFY resizeNativeChanged)
    /// Mode stress : 2000 bulles factices (AICE_STRESS=1).
    Q_PROPERTY(bool stress READ stress CONSTANT)
    /// Bourrage de l'affichage : redessine en continu pour mesurer le frame
    /// time sans interaction (AICE_PUMP=1).
    Q_PROPERTY(bool pump READ pump CONSTANT)
    /// Affichage des mesures dans la fenetre (AICE_HUD=0 pour cacher).
    Q_PROPERTY(bool hud READ hud CONSTANT)
    /// Ombres de carte activee au demarrage (AICE_SHADOW=0 pour les couper, et
    /// mesurer ainsi ce qu'elles coutent). Se bascule aussi depuis le badge.
    Q_PROPERTY(bool shadow READ shadow CONSTANT)
    /// Mode de fenetre : "fenetre" (ordinaire, au gre du composeur), "au-dessus"
    /// (gardee devant les autres fenetres), "fixe" (au-dessus de tout et posee sur
    /// tous les bureaux, comme la v1). Regle par /fenetre, persiste, et 
    /// AICE_WINDOW_MODE=<mode> au lancement.
    Q_PROPERTY(QString windowMode READ windowMode NOTIFY windowModeChanged)
    /// Le composeur decide de la nature de la surface a sa creation : passer d'une
    /// fenetre ordinaire a une couche (et l'inverse) ne se peut pas en cours de
    /// route. Ce drapeau dit que le reglage est pris, mais pour la prochaine ouverture.
    Q_PROPERTY(bool windowModeRestart READ windowModeRestart NOTIFY windowModeChanged)

public:
    explicit GlassHelper(QObject *parent = nullptr);

    /// Branche le verre sur la fenetre QML : titre, overlay eventuel,
    /// persistance de la position, mesure des frames, premiere region de verre.
    Q_INVOKABLE void attach(QQuickWindow *window);

    /// Attrape la fenetre : deplacement natif KWin/Wayland si le composeur le
    /// sait faire. Renvoie false si c'est au QML de deplacer a la main.
    /// (Nomme startMove et non grabWindow : QQuickWindow a deja un grabWindow()
    /// qui, lui, capture une image.)
    Q_INVOKABLE bool startMove();

    /// Redimensionne la fenetre depuis les bords donnes (Qt::Edges meldes en
    /// entier), du cote ou la fenetre grandit : le bord droit et/ou le bas.
    /// Renvoie false si c'est au QML d'ajuster la taille a la main.
    Q_INVOKABLE bool startResize(int edges);

    /// Les cartes filles de cet item sont retranchees de sa zone (le ListView) :
    /// une bulle sortie du viewport ne doit ni flouter ni capter les clics.
    Q_INVOKABLE void setClipItem(QQuickItem *item);

    /// Recalcule la region de verre (les appels multiples sont fusionnes).
    Q_INVOKABLE void scheduleRefresh();

    /// Capture hors ecran de la scene, posee sur un fond de controle, et ecrite
    /// dans le fichier donne (AICE_SHOT=/chemin.png). Sert a regarder le rendu
    /// (degrade d'ombre, fade) sans avoir les yeux sur l'ecran.
    Q_INVOKABLE void grabShot(const QString &path);

    /// Le mode de fenetre demande ("fenetre", "au-dessus" ou "fixe"), applique
    /// quand le composeur sait le faire sans changer la surface, et persiste de
    /// toute facon : il vaudra au prochain lancement.
    Q_INVOKABLE void setWindowMode(const QString &mode);

    double frameMs() const { return m_frameMs; }
    double frameMax() const { return m_frameMax; }
    qlonglong frames() const { return m_frames; }
    int cards() const { return m_cards; }
    bool moveNative() const { return m_moveNative; }
    bool resizeNative() const { return m_resizeNative; }
    bool stress() const { return m_stress; }
    bool pump() const { return m_pump; }
    bool hud() const { return m_hud; }
    bool shadow() const { return m_shadow; }
    QString windowMode() const { return m_mode; }
    bool windowModeRestart() const { return m_modeRestart; }

signals:
    void frameMsChanged();
    void cardsChanged();
    void moveNativeChanged();
    void resizeNativeChanged();
    void windowModeChanged();

private:
    struct Card {
        QRect rect;
        int radius;
        qreal alpha = 1.0;   // alpha effectif, parents inclus (le fade est porte par la delegate)
    };

    void applyRegions();
    void collect(QQuickItem *item, QQuickItem *sceneRoot, QVector<Card> &out, int &excluded,
                 int &candidates) const;
    static bool isInside(const QQuickItem *item, const QQuickItem *ancestor);
    void persistPosition();
    void onFrameSwapped();

    QQuickWindow *m_window = nullptr;
    QQuickItem *m_clip = nullptr;      // viewport a respecter (le ListView)
    QVector<Card> m_lastCards;         // les cartes du dernier verre (debug/ captures)
    QRegion m_lastRegion;              // pour ne rien reappliquer si identique
    bool m_pending = false;            // refresh deja programme
    bool m_layered = false;            // overlay layer-shell (AICE_LAYER=1)
    bool m_moveNative = false;
    bool m_resizeNative = false;
    bool m_stress = false;
    bool m_pump = false;
    bool m_hud = true;
    bool m_shadow = true;
    QString m_mode;                    // "fenetre", "au-dessus" ou "fixe"
    bool m_modeRestart = false;        // le reglage attend la prochaine ouverture
    int m_excludedShown = -1;              // dernier compte d'exclusions affiche (debug)
    long m_verreShown = -1;               // dernier etat du verre affiche (debug)
    int m_cards = 0;

    // Mesure des frames.
    qlonglong m_frames = 0;
    qint64 m_lastSwap = 0;
    double m_frameMs = 0.0;
    double m_frameMax = 0.0;
};
