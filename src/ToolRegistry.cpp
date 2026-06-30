#include "ToolRegistry.h"
#include "Tool.h"
#include <QTimer>
#include <memory>

ToolRegistry::ToolRegistry(QObject *parent) : QObject(parent) {}

void ToolRegistry::add(Tool *tool)
{
    if (!tool) return;
    tool->setParent(this);
    m_tools.append(tool);
}

QJsonArray ToolRegistry::toJsonArray() const
{
    QJsonArray arr;
    for (Tool *t : m_tools) {
        const Tool::Spec s = t->spec();
        QJsonObject entry;
        entry["type"] = QStringLiteral("function");
        QJsonObject fn;
        fn["name"] = s.name;
        fn["description"] = s.description;
        fn["parameters"] = s.parameters;
        entry["function"] = fn;
        arr.append(entry);
    }
    return arr;
}

bool ToolRegistry::isEmpty() const { return m_tools.isEmpty(); }

Tool *ToolRegistry::find(const QString &name) const
{
    for (Tool *t : m_tools) {
        if (t->spec().name == name) return t;
    }
    return nullptr;
}

void ToolRegistry::execute(const QString &name, const QJsonObject &args,
                           std::function<void(bool ok, QString result)> cb)
{
    Tool *t = find(name);
    if (!t) {
        cb(false, QStringLiteral("Unknown tool: %1").arg(name));
        return;
    }

    // Guard : garantit que cb est invoquée exactement une fois, même si le tool
    // oublie son callback (process bloqué, bug, double-appel…). Sans ça, un
    // tool qui oublie cb fige silencieusement la boucle agentique.
    auto called = std::make_shared<bool>(false);
    auto guard = [cb, called](bool ok, QString result) {
        if (*called) return;
        *called = true;
        cb(ok, std::move(result));
    };

    // Filet de sécurité : si le tool n'a pas rappelé cb après 130s, on déclenche
    // une erreur pour ne pas rester bloqué indéfiniment.
    QTimer::singleShot(130000, this, [called, guard]() {
        if (*called) return;
        *called = true;
        guard(false, QStringLiteral("tool timed out (no callback after 130s)"));
    });

    try {
        t->execute(args, guard);
    } catch (...) {
        guard(false, QStringLiteral("tool crashed (exception in execute)"));
    }
}