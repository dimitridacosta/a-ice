#include "TerminalSafety.h"

#include <QRegularExpression>
#include <QList>
#include <QPair>
#include <QStringList>

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
// --- Paths sensibles (approval même en lecture) -----------------------------
// Accès à ces fichiers/dirs = potentiellement compromettant (exfiltration de
// clés, reconfig de l'app, etc.). Source : RESEARCH-terminal-safety.md §5.
static const QStringList &sensitivePaths()
{
    static const QStringList kSensitive = {
        QStringLiteral("~/.ssh/"),
        QStringLiteral("~/.aws/"),
        QStringLiteral("~/.gnupg/"),
        QStringLiteral("~/.kube/"),
        QStringLiteral("~/.docker/"),
        QStringLiteral("~/.config/gh/"),
        QStringLiteral("~/.config/gcloud/"),
        QStringLiteral("~/.config/a-ice/"),
        QStringLiteral("~/.bashrc"),
        QStringLiteral("~/.zshrc"),
        QStringLiteral("~/.profile"),
        QStringLiteral("~/.bash_profile"),
        QStringLiteral("~/.zprofile"),
        QStringLiteral("~/.netrc"),
        QStringLiteral("~/.pgpass"),
        QStringLiteral("~/.npmrc"),
        QStringLiteral("~/.pypirc"),
        QStringLiteral("~/.git-credentials"),
        QStringLiteral("/etc/sudoers"),
        QStringLiteral("/etc/passwd"),
        QStringLiteral("/etc/shadow"),
        QStringLiteral("/etc/ssh/"),
    };
    return kSensitive;
}

static QString checkSensitivePath(const QString &command)
{
    for (const QString &p : sensitivePaths()) {
        if (command.contains(p))
            return QStringLiteral("sensitive path access: %1").arg(p);
    }
    return {};
}

// --- Read-only allowlist -----------------------------------------------------
// Une commande est "lecture pure" si :
//   - elle ne contient aucun marqueur de mutation (redirect, tee, -exec,
//     -delete, xargs, sed -i, pipe |)
//   - ET son premier verbe (éventuellement préfixé par sudo/env) est dans une
//     allowlist de commandes read-only connues.
// Tout ce qui n'est pas reconnu comme lecture pure → approval (default-deny
// sur les mutations, cf. choix utilisateur).
static bool isReadOnly(const QString &command)
{
    // Marqueurs de mutation → pas read-only.
    if (command.contains(QLatin1String("-exec"))
        || command.contains(QLatin1String("-execdir"))
        || command.contains(QLatin1String("-delete"))
        || command.contains(QLatin1String("xargs"))
        || command.contains(QLatin1String("tee"))
        || command.contains('|'))
        return false;
    // Redirect vers fichier (sauf 2> stderr-only).
    static const QRegularExpression kRedirect(
        QStringLiteral(R"((?:^|[^\d])>>?[^&])"));
    if (kRedirect.match(command).hasMatch())
        return false;
    // sed -i (édition in-place).
    static const QRegularExpression kSedI(
        QStringLiteral(R"(\bsed\s+-[^\s]*i)"));
    if (kSedI.match(command).hasMatch())
        return false;

    // Allowlist de verbes read-only (éventuellement préfixés par sudo/env).
    static const QRegularExpression kReadOnlyVerbs(
        QStringLiteral(R"(^\s*(?:sudo\s+|env\s+)?(?:)"
            "ls|cat|head|tail|less|more|grep|egrep|fgrep|rg|ack|"
            "wc|file|stat|du|df|pwd|whoami|id|who|which|type|whereis|"
            "env|printenv|uname|hostname|date|uptime|ps|free|lsblk|"
            "find|tree|diff|echo|printf|test|true|false|"
            "git status|git log|git diff|git show|git blame|"
            "git remote|git ls-files|git rev-parse|git config --get|"
            "git --version|node --version|python --version|"
            "realpath|readlink|basename|dirname"
            R"()\b)"));
    return kReadOnlyVerbs.match(command).hasMatch();
}

QString checkApprovalRequired(const QString &command)
{
    // 1. Hardline : ne pas demander d'approval — le hardline bloquera
    //    inconditionnellement (une commande non-récupérable ne doit jamais
    //    être approuvable).
    if (!checkHardline(command).isEmpty())
        return {};
    // 2. Dangerous pattern (récupérable mais risqué).
    const QString danger = checkDangerous(command);
    if (!danger.isEmpty())
        return danger;
    // 3. Path sensible (même en lecture → exfiltration potentielle).
    const QString sens = checkSensitivePath(command);
    if (!sens.isEmpty())
        return sens;
    // 4. Non read-only → mutation → approval.
    if (!isReadOnly(command))
        return QStringLiteral("command modifies state (not read-only)");
    // 5. Lecture pure + pas sensible → auto-allow.
    return {};
}
