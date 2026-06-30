#pragma once

#include "../Tool.h"
#include <QString>
#include <QDir>

class TerminalTool : public Tool
{
    Q_OBJECT
public:
    explicit TerminalTool(QObject *parent = nullptr)
        : Tool(parent), m_workdir(QDir::homePath()) {}
    explicit TerminalTool(const QString &workdir, QObject *parent = nullptr)
        : Tool(parent), m_workdir(workdir.isEmpty() ? QDir::homePath() : workdir) {}

    Spec spec() const override;
    void execute(const QJsonObject &args,
                 std::function<void(bool ok, QString result)> cb) override;

private:
    QString m_workdir;
};