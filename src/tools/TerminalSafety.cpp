#include "TerminalSafety.h"

#include <QRegularExpression>
#include <QList>
#include <QPair>

// --- Hardline blocklist (unconditional, non-bypassable) ----------------------
// Patterns trop dangereux pour être exécutés même avec approbation manuelle.
// Match → on refuse d'exécuter, on renvoie une erreur non-récupérable au modèle.
// Source : docs/dev/RESEARCH-terminal-safety.md §"Hardline (block inconditionnel)".
static const QList<QPair<QRegularExpression, QString>> &hardlinePatterns()
{
    static const QList<QPair<QRegularExpression, QString>> kHardline = {
        {QRegularExpression(QStringLiteral(
            R"(\brm\s+(-[^\s]*\s+)*-?r?f?\s+/(\s|$))")),
         QStringLiteral("rm recursive on root path")},
        {QRegularExpression(QStringLiteral(R"(\bmkfs\b)")),
         QStringLiteral("format filesystem")},
        {QRegularExpression(QStringLiteral(
            R"(\bdd\b.*\bof=/dev/(sd|nvme|hd|mmcblk|vd|xvd))")),
         QStringLiteral("dd to raw block device")},
        {QRegularExpression(QStringLiteral(
            R"(>\s*/dev/(sd|nvme|hd|mmcblk|vd|xvd))")),
         QStringLiteral("redirect to raw block device")},
        {QRegularExpression(QStringLiteral(
            R"(:\(\)\s*\{\s*:\s*\|\s*:\s*&\s*\}\s*;\s*:)")),
         QStringLiteral("fork bomb")},
        {QRegularExpression(QStringLiteral(
            R"(\b(shutdown|reboot|halt|poweroff)\b)")),
         QStringLiteral("system power control")},
        {QRegularExpression(QStringLiteral(R"(\bkill\s+-9\s+-1\b)")),
         QStringLiteral("kill all processes (kill -9 -1)")},
        {QRegularExpression(QStringLiteral(
            R"(\bchmod\s+(-R|--recursive\b).*\s+/(s?bin|etc|usr|boot|lib))")),
         QStringLiteral("chmod on system directories")},
    };
    return kHardline;
}

QString checkHardline(const QString &command)
{
    for (const auto &p : hardlinePatterns()) {
        if (p.first.match(command).hasMatch()) {
            return QStringLiteral(
                "BLOCKED (unrecoverable): %1. This command is on the unconditional "
                "blocklist. Do NOT retry or attempt variants."
            ).arg(p.second);
        }
    }
    return {};
}

// --- Dangerous patterns (approve-once par session, récupérables) -------------
// Patterns dangereux mais récupérables : exigent approbation manuelle.
// Source : docs/dev/RESEARCH-terminal-safety.md §"Dangerous (approve-once)".
static const QList<QPair<QRegularExpression, QString>> &dangerousPatterns()
{
    static const QList<QPair<QRegularExpression, QString>> kDangerous = {
        {QRegularExpression(QStringLiteral(R"(\brm\s+-[^\s]*r)")),
         QStringLiteral("recursive delete (rm -r)")},
        {QRegularExpression(QStringLiteral(R"(\brm\s+(-[^\s]*\s+)*/)")),
         QStringLiteral("delete in root path")},
        {QRegularExpression(QStringLiteral(
            R"(\bchmod\s+(777|666|o\+w|a\+w))")),
         QStringLiteral("world-writable permissions")},
        {QRegularExpression(QStringLiteral(
            R"(\bchown\s+(-R|--recursive).*\sroot\b)")),
         QStringLiteral("recursive chown to root")},
        {QRegularExpression(QStringLiteral(
            R"(\b(bash|sh|zsh)\s+-[^\s]*c\b)")),
         QStringLiteral("shell exec via -c")},
        {QRegularExpression(QStringLiteral(
            R"(\b(python[23]?|perl|ruby|node)\s+-[ec]\s+)")),
         QStringLiteral("script exec via -e/-c")},
        {QRegularExpression(QStringLiteral(
            R"(\b(curl|wget)\b.*\|\s*(\|/\w+/)*/?(ba)?sh\b)")),
         QStringLiteral("pipe to shell (curl|sh)")},
        {QRegularExpression(QStringLiteral(R"(\bgit\s+reset\s+--hard\b)")),
         QStringLiteral("git reset --hard")},
        {QRegularExpression(QStringLiteral(
            R"(\bgit\s+push\b.*(-f|--force)\b)")),
         QStringLiteral("git force push")},
        {QRegularExpression(QStringLiteral(R"(\bgit\s+clean\s+-[^\s]*f)")),
         QStringLiteral("git clean force")},
        {QRegularExpression(QStringLiteral(R"(\bgit\s+branch\s+-D\b)")),
         QStringLiteral("git branch force delete")},
        {QRegularExpression(QStringLiteral(
            R"(\bsystemctl\s+(stop|restart|disable|mask)\b)")),
         QStringLiteral("system service control")},
        {QRegularExpression(QStringLiteral(R"(\bpkill\s+-9\b)")),
         QStringLiteral("force kill (pkill -9)")},
        {QRegularExpression(QStringLiteral(R"(\bxargs\s+.*\brm\b)")),
         QStringLiteral("xargs with rm")},
        {QRegularExpression(QStringLiteral(
            R"(\bfind\b.*-exec(?:dir)?\s+(/\S*/)?rm\b)")),
         QStringLiteral("find -exec rm")},
        {QRegularExpression(QStringLiteral(R"(\bfind\b.*-delete\b)")),
         QStringLiteral("find -delete")},
        {QRegularExpression(QStringLiteral(
            R"(\bsudo\b[^;&|]*?\s+(-S|--stdin|-s|-a|--askpass))")),
         QStringLiteral("sudo privilege flag")},
        {QRegularExpression(QStringLiteral(R"(>>?\s*["']?~/\.ssh/)")),
         QStringLiteral("write to ssh dir")},
        {QRegularExpression(QStringLiteral(R"(>>?\s*["']?~/\.bashrc["']?)")),
         QStringLiteral("overwrite shell rc (~/.bashrc)")},
        {QRegularExpression(QStringLiteral(R"(\bsed\s+-[^\s]*i)")),
         QStringLiteral("in-place sed edit")},
        {QRegularExpression(QStringLiteral(
            R"(\b(cp|mv|install)\b.*\s~/\.ssh/)")),
         QStringLiteral("copy/move into ssh dir")},
        {QRegularExpression(QStringLiteral(
            R"(\b(cp|mv|install)\b.*\s~/\.bashrc)")),
         QStringLiteral("overwrite shell rc")},
        {QRegularExpression(QStringLiteral(
            R"(\bDROP\s+(TABLE|DATABASE)\b)")),
         QStringLiteral("SQL DROP")},
        {QRegularExpression(QStringLiteral(
            R"(\bDELETE\s+FROM\b(?![^\n]*\bWHERE\b))")),
         QStringLiteral("SQL DELETE without WHERE")},
        {QRegularExpression(QStringLiteral(R"(\bTRUNCATE\s+)")),
         QStringLiteral("SQL TRUNCATE")},
    };
    return kDangerous;
}

QString checkDangerous(const QString &command)
{
    for (const auto &p : dangerousPatterns()) {
        if (p.first.match(command).hasMatch()) {
            return p.second;
        }
    }
    return {};
}