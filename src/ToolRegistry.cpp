#include "ToolRegistry.h"
#include "Tool.h"

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
    t->execute(args, cb);
}