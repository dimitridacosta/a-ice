#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QFrame>
#include <QVBoxLayout>
#include <QString>
#include <QImage>
#include <QList>

class QMouseEvent;
class ShimmerButton;
class WaitingBlock;

/**
 * Bloc d'attente : placeholder affiché dans la carte assistant entre le
 * sendMessages et le premier chunk (thinking/content/tool). Même style
 * visuel que ThinkingBlock (shimmer) + points animés qui défilent pour
 * donner une vraie sensation d'activité (le PC infère). NON expandable.
 * Retiré des qu'un vrai bloc arrive. Evite une carte vide/amateur.
 */
class WaitingBlock : public QWidget
{
    Q_OBJECT
public:
    explicit WaitingBlock(QWidget *parent = nullptr);
    void startShimmer();
    void stopShimmer();
signals:
    void geometryChanged();
private:
    ShimmerButton *m_label;
    QTimer *m_dotsTimer = nullptr;
    int m_dots = 0;  // 0..3 : nombre de points affichés
};

/**
 * Bloc de réflexion (collapsible). Un par phase de thinking.
 * Shimmer sur le toggle pendant le streaming, auto-collapse quand le
 * contenu arrive.
 */
class ThinkingBlock : public QWidget
{
    Q_OBJECT
public:
    explicit ThinkingBlock(QWidget *parent = nullptr);

    void append(const QString &text);
    void set(const QString &text);
    void collapse();
    void expand();
    bool expanded() const { return m_expanded; }
    void startShimmer();
    void stopShimmer();

signals:
    void geometryChanged();

private:
    void refresh();
    ShimmerButton *m_toggle;
    QLabel *m_label;
    QString m_text;
    bool m_expanded = false;
};

/**
 * Bloc tool call (collapsible). Header "🔧 name ▸" avec icône de statut
 * (⏳ running / ✓ ok / ⚠ error / ✗ denied). Dépliable → params + résultat.
 */
class ToolBlock : public QWidget
{
    Q_OBJECT
public:
    explicit ToolBlock(const QString &name, QWidget *parent = nullptr);

    void setParams(const QString &json);
    void setResult(const QString &result, bool ok);
    void setStatus(const QString &icon);

signals:
    void geometryChanged();

private:
    void toggle();
    void refreshHeader();
    /// Extrait un resume court du JSON de params (command/url/query/...) pour
    /// l'afficher dans le header minifie. Tronque avec ellipsis si trop long.
    static QString extractSummary(const QString &name, const QString &json);
    QLabel *m_header;        // rich text : nom bold + desc claire + arrow
    QLabel *m_detail;
    QString m_name;
    QString m_params;
    QString m_result;
    QString m_statusIcon;
    QString m_summary;  // resume affiche dans le header (ex: debut de commande)
    bool m_expanded = false;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};

/**
 * Bloc de texte (message assistant, intermédiaire ou final — même traitement).
 * Rendu Markdown, visible (pas collapsible).
 */
class ContentBlock : public QWidget
{
    Q_OBJECT
public:
    explicit ContentBlock(QWidget *parent = nullptr);

    void append(const QString &text);
    void set(const QString &text);

signals:
    void geometryChanged();

private:
    void refresh();
    QLabel *m_label;
    QString m_text;
};

/**
 * Bulle verre (glass) : une par tour de l'agent.
 *
 * - Rôle "user" : bulle alignée à droite, contenu simple.
 * - Rôle "assistant" : carte contenant une pile verticale de blocs
 *   (ThinkingBlock / ToolBlock / ContentBlock) dans l'ordre chronologique
 *   d'arrivée. Une seule carte pour toute la réponse agent (du message
 *   utilisateur jusqu'à la réponse finale, en incluant la boucle tools).
 *   Voir ROADMAP item 6 (révisé : une carte + blocs empilés).
 */
class Bubble : public QWidget
{
    Q_OBJECT

public:
    enum class Role { User, Assistant };

    explicit Bubble(Role role, QWidget *parent = nullptr);

    // --- Rôle user ---
    void appendContent(const QString &text);
    void fitToContent(int maxWidth);

    // --- Rôle assistant : pile de blocs ---
    ThinkingBlock *addThinkingBlock();
    ToolBlock *addToolBlock(const QString &name);
    ContentBlock *addContentBlock();
    /// Placeholder d'attente (avant le premier chunk). Non-expandable.
    WaitingBlock *addWaitingBlock();

    Role role() const { return m_role; }

    /// Image d'ombre floutée (cache par taille de carte). Dessinée par le parent
    /// (conteneur) pour pouvoir déborder et se chevaucher entre bulles.
    const QImage &shadowImage();

    // --- Approval inline (dangerous command) ---
    enum ApprovalChoice { AllowOnce = 0, AllowSession = 1, Deny = 2 };
    /// Affiche un bandeau d'approbation inline (non-modal) dans la carte :
    /// warning + commande + 3 boutons. Émet approvalResult() au clic.
    void showApproval(const QString &command, const QString &description);
    /// Retire le bandeau d'approbation (après résolution).
    void clearApproval();

signals:
    /// Émis quand la géométrie/visibilité change (pour recalculer le blur).
    void geometryChanged();
    /// Émis au clic sur un bouton d'approbation. choice = ApprovalChoice.
    void approvalResult(int choice);

protected:
    void resizeEvent(QResizeEvent *) override { emit geometryChanged(); }
    void paintEvent(QPaintEvent *) override;
    int heightForWidth(int w) const override;

private:
    void buildAssistant();
    void buildUser();

    Role m_role;

    // Assistant uniquement : layout vertical qui empile les blocs.
    QVBoxLayout *m_layout = nullptr;
    QWidget *m_approvalWidget = nullptr;

    // User uniquement : label de contenu.
    QLabel *m_userLabel = nullptr;
    QString m_userContent;

    // Cache de l'ombre (ne dépend que de la taille de la carte).
    QImage m_shadow;
    QSize  m_shadowKey;
};