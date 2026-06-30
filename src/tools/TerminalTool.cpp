#include "TerminalTool.h"

#include <QDir>
#include <QProcess>
#include <QJsonArray>
#include <algorithm>
#include <memory>

Tool::Spec TerminalTool::spec() const
{
    QJsonObject params;
    params["type"] = QStringLiteral("object");

    QJsonObject props;
    QJsonObject cmd;
    cmd["type"] = QStringLiteral("string");
    cmd["description"] = QStringLiteral("The bash command to execute");
    props["command"] = cmd;

    QJsonObject timeout;
    timeout["type"] = QStringLiteral("integer");
    timeout["description"] = QStringLiteral("Max execution time in ms (default 30000, max 120000)");
    props["timeout_ms"] = timeout;

    params["properties"] = props;
    params["required"] = QJsonArray{QStringLiteral("command")};

    return Spec{
        QStringLiteral("terminal"),
        QStringLiteral("Execute a shell command on the user's machine (bash -c). "
                       "Runs in the user's home directory. Use for: listing files, "
                       "reading files (cat, head), git, file system inspection. "
                       "AVOID destructive commands. Returns combined stdout+stderr "
                       "(truncated to 20000 chars)."),
        params
    };
}

void TerminalTool::execute(const QJsonObject &args,
                           std::function<void(bool ok, QString result)> cb)
{
    const QString command = args.value(QStringLiteral("command")).toString().trimmed();
    if (command.isEmpty()) {
        cb(false, QStringLiteral("[a-ice] terminal: empty command"));
        return;
    }

    int timeoutMs = args.value(QStringLiteral("timeout_ms")).toInt(30000);
    timeoutMs = std::clamp(timeoutMs, 1000, 120000);

    auto *proc = new QProcess(this);
    proc->setWorkingDirectory(m_workdir);
    proc->setProcessChannelMode(QProcess::MergedChannels);

    auto done = std::make_shared<bool>(false);
    QObject::connect(proc, &QProcess::finished, this,
        [cb, proc, done](int, QProcess::ExitStatus) {
            if (*done) return;
            *done = true;
            QString out = QString::fromUtf8(proc->readAllStandardOutput());
            if (out.size() > 20000) {
                out = QStringLiteral("[output truncated]\n") + out.right(20000);
            }
            proc->deleteLater();
            cb(true, out);
        });

    QObject::connect(proc, &QProcess::errorOccurred, this,
        [cb, proc, done](QProcess::ProcessError err) {
            if (*done || err == QProcess::Timedout)
                return;
            *done = true;
            QString msg = QStringLiteral("[a-ice] terminal: process error: ")
                          + proc->errorString();
            proc->deleteLater();
            cb(false, msg);
        });

    proc->start(QStringLiteral("bash"), QStringList{QStringLiteral("-c"), command});

    if (!proc->waitForStarted(2000)) {
        if (*done) return;
        *done = true;
        QString msg = QStringLiteral("[a-ice] terminal: failed to start: ")
                      + proc->errorString();
        proc->deleteLater();
        cb(false, msg);
        return;
    }

    if (!proc->waitForFinished(timeoutMs)) {
        if (*done) return;
        *done = true;
        proc->terminate();
        if (!proc->waitForFinished(1000))
            proc->kill();
        QString out = QString::fromUtf8(proc->readAllStandardOutput());
        proc->deleteLater();
        cb(false, QStringLiteral("[a-ice] terminal: Timeout after %1ms\n%2")
                       .arg(timeoutMs)
                       .arg(out));
    }
}