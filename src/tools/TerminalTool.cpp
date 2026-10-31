#include "TerminalTool.h"
#include "TerminalSafety.h"

#include <QDir>
#include <QProcess>
#include <QTimer>
#include <QJsonArray>
#include <algorithm>
#include <memory>

bool TerminalTool::s_testMode = false;

// setTestMode est définie inline dans le header.

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

    // Couche 1 — Hardline : block inconditionnel, avant toute exécution.
    if (const QString blocked = checkHardline(command); !blocked.isEmpty()) {
        cb(false, QStringLiteral("[a-ice] terminal: %1").arg(blocked));
        return;
    }

    // Filet de test ultime : en test mode, aucune commande n'atteint bash.
    // Activé au lancement (env var A_ICE_TERMINAL_TEST) — non pilotable par le modèle.
    if (s_testMode) {
        cb(true, QStringLiteral("[TEST MODE] terminal command NOT executed: \"%1\"\n"
                                "TEST TERMINAL OUTPUT").arg(command));
        return;
    }

    int timeoutMs = args.value(QStringLiteral("timeout_ms")).toInt(30000);
    timeoutMs = std::clamp(timeoutMs, 1000, 120000);

    // Execution asynchrone : on ne bloque PAS l'event loop (waitForFinished
    // gelait l'UI et empechait le bouton Stop de fonctionner pendant une
    // commande). Le timeout est gere via QTimer, et cancel() peut tuer le
    // QProcess depuis l'exterieur. Voir ROADMAP item 8.
    auto *proc = new QProcess(this);
    proc->setWorkingDirectory(m_workdir);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    m_activeProcs.append(proc);

    auto *timeoutTimer = new QTimer(this);
    timeoutTimer->setSingleShot(true);

    auto done = std::make_shared<bool>(false);

    QObject::connect(proc, &QProcess::finished, this,
        [this, cb, proc, done, timeoutTimer, timeoutMs](int, QProcess::ExitStatus) {
            if (*done) return;
            *done = true;
            timeoutTimer->stop();
            timeoutTimer->deleteLater();
            m_activeProcs.removeAll(proc);
            QString out = QString::fromUtf8(proc->readAllStandardOutput());
            if (out.size() > 20000) {
                out = QStringLiteral("[output truncated]\n") + out.right(20000);
            }
            const bool cancelled = proc->property("aice_cancel").toBool();
            const bool timedOut = proc->property("aice_timeout").toBool();
            proc->deleteLater();
            if (cancelled)
                cb(false, QStringLiteral("[a-ice] terminal: cancelled by user"));
            else if (timedOut)
                cb(false, QStringLiteral("[a-ice] terminal: Timeout after %1ms\n%2")
                               .arg(timeoutMs)
                               .arg(out));
            else
                cb(true, out);
        });

    QObject::connect(proc, &QProcess::errorOccurred, this,
        [this, cb, proc, done, timeoutTimer](QProcess::ProcessError) {
            if (*done) return;
            *done = true;
            timeoutTimer->stop();
            timeoutTimer->deleteLater();
            m_activeProcs.removeAll(proc);
            const QString msg = QStringLiteral("[a-ice] terminal: process error: %1")
                                .arg(proc->errorString());
            proc->deleteLater();
            cb(false, msg);
        });

    QObject::connect(timeoutTimer, &QTimer::timeout, this, [proc]() {
        proc->setProperty("aice_timeout", true);
        proc->terminate();
        QTimer::singleShot(1000, proc, [proc]() {
            if (proc->state() != QProcess::NotRunning)
                proc->kill();
        });
    });

    proc->start(QStringLiteral("bash"), QStringList{QStringLiteral("-c"), command});
    timeoutTimer->start(timeoutMs);
}

void TerminalTool::cancel()
{
    // Termine proprement tous les QProcess actifs. Le finished handler
    // detecte la property "aice_cancel" et renvoie un resultat "cancelled".
    for (QProcess *p : m_activeProcs) {
        if (p->state() != QProcess::NotRunning) {
            p->setProperty("aice_cancel", true);
            p->terminate();
            QTimer::singleShot(1000, p, [p]() {
                if (p->state() != QProcess::NotRunning)
                    p->kill();
            });
        }
    }
}