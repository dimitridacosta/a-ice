#pragma once

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <functional>

/**
 * Interface abstraite pour un "tool" appelable par le modèle via
 * function-calling OpenAI-compatible.
 *
 * Chaque tool expose :
 *   - un nom + description + JSON-schema de paramètres (envoyé au modèle)
 *   - une méthode execute() asynchrone (callback) qui renvoie un string
 *     (le résultat injecté en role="tool" dans la conversation).
 */
class Tool : public QObject
{
    Q_OBJECT

public:
    struct Spec {
        QString name;         // ex: "terminal"
        QString description;  // court, vu par le modèle
        QJsonObject parameters; // JSON-schema OpenAI
    };

    explicit Tool(QObject *parent = nullptr) : QObject(parent) {}
    ~Tool() override = default;

    virtual Spec spec() const = 0;

    /// Exécute le tool avec les args (déjà parsées depuis JSON du modèle).
    /// `cb(ok, result)` doit être invoqué exactement une fois.
    ///   ok=true  → result = contenu à injecter en role="tool"
    ///   ok=false → result = message d'erreur (injecté quand même pour que
    ///              le modèle sache que ça a échoué)
    virtual void execute(const QJsonObject &args,
                         std::function<void(bool ok, QString result)> cb) = 0;

    /// Interruption demandee par l'utilisateur (bouton Stop). Default no-op ;
    /// un tool qui lance des ressources async (QProcess, reseau...) override
    /// pour terminer ces ressources. Voir ROADMAP item 8.
    virtual void cancel() {}
};