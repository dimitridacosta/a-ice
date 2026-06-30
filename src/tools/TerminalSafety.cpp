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