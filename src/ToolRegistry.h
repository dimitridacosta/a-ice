#pragma once

#include <QObject>
#include <QList>
#include <QJsonArray>
#include <QString>
#include <functional>

class Tool;

/**
 * Registre des tools disponibles. Sérialise la liste pour le payload
 * OpenAI (champ "tools") et dispatch les appels entrants vers le bon tool.
 */
class ToolRegistry : public QObject
{
    Q_OBJECT

public:
    explicit ToolRegistry(QObject *parent = nullptr);

    /// Ajoute un tool (le registry prend ownership).
    void add(Tool *tool);

    /// Liste des specs au format OpenAI "tools" array (à injecter dans le payload).
    QJsonArray toJsonArray() const;

    /// true si au moins un tool est enregistré.
    bool isEmpty() const;

    /// Trouve un tool par nom. nullptr si absent.
    Tool *find(const QString &name) const;

    /// Exécute un tool par nom. cb(ok, result). Si le tool n'existe pas →
    /// cb(false, "unknown tool: <name>").
    void execute(const QString &name, const QJsonObject &args,
                 std::function<void(bool ok, QString result)> cb);

private:
    QList<Tool *> m_tools;
};