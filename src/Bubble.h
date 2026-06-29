#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QFrame>
#include <QString>
#include <QImage>

class QMouseEvent;
class QPushButton;
class ShimmerButton;

/**
 * Bulle verre (glass) : une par tour.
 *
 * - Rôle "user"   : bulle alignée à droite, contenu simple.
 * - Rôle "assistant" : bulle alignée à gauche avec une zone "Réflexion"
 *   repliable (visible en direct pendant le thinking, puis auto-réduite
 *   quand la réponse commence à arriver — re-dépliable par clic).
 */
class Bubble : public QWidget
{
    Q_OBJECT

public:
    enum class Role { User, Assistant };

    explicit Bubble(Role role, QWidget *parent = nullptr);

    /// Ajoute un morceau de réflexion (thinking) — zone visible en direct.
    void appendThinking(const QString &text);
    /// Ajoute un morceau de réponse finale.
    void appendContent(const QString &text);
    /// Réduit la zone réflexion (après l'arrivée du contenu).
    /// Le texte complet reste accessible par clic sur l'en-tête.
    void collapseThinking();
    /// Force l'affichage complet de la réflexion.
    void expandThinking();
    /// Replie/déplie la réflexion (toggle utilisateur).
    void toggleThinking();
    bool thinkingExpanded() const { return m_thinkingExpanded; }

    Role role() const { return m_role; }

    /// Image d'ombre floutée (cache par taille de carte). Dessinée par le parent
    /// (conteneur) pour pouvoir déborder et se chevaucher entre bulles.
    const QImage &shadowImage();

    /// (User) Mesure le texte et fixe la taille exacte de la bulle :
    /// largeur = plus longue ligne (capée à maxWidth), hauteur = toutes les
    /// lignes rendues (y compris word-wrap). Appeler après appendContent().
    void fitToContent(int maxWidth);

signals:
    /// Émis quand la géométrie/visibilité change (pour recalculer le blur).
    void geometryChanged();

protected:
    void resizeEvent(QResizeEvent *) override { emit geometryChanged(); }
    void paintEvent(QPaintEvent *) override;
    int heightForWidth(int w) const override;



private:
    void buildAssistant();
    void buildUser();
    void refreshThinkingLabel();
    void refreshContentLabel();

    Role m_role;

    // Assistant uniquement :
    ShimmerButton *m_thinkingToggle = nullptr;
    QLabel      *m_contentLabel   = nullptr;
    QLabel      *m_thinkingLabel  = nullptr;
    QString      m_thinking;
    QString      m_content;
    bool         m_thinkingExpanded = false;

    // Cache de l'ombre (ne dépend que de la taille de la carte).
    QImage m_shadow;
    QSize  m_shadowKey;
};