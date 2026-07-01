#pragma once

#include <QString>

// Sécurité du terminal — logique isolée du QProcess pour permettre un test
// unitaire qui n'inclut jamais l'exécution shell.
//
// checkHardline() renvoie une QString vide si la commande est autorisée,
// sinon le message d'erreur (hardline, non-bypassable) à renvoyer au modèle.
QString checkHardline(const QString &command);

// checkDangerous() renvoie une QString vide si la commande ne match aucun
// pattern dangereux, sinon une courte description du pattern matché (utilisé
// comme clé de session-approval : une fois approuvé, on ne redemande pas pour
// la même description). Récupérable = exige approbation manuelle, pas un block.
QString checkDangerous(const QString &command);

// checkApprovalRequired() rassemble la politique complète d'approbation :
//   - hardline match → return {} (le hardline bloquera, pas d'approval sur
//     une commande non-récupérable)
//   - dangerous match → description du pattern
//   - accès à un path sensible (~/.ssh, ~/.bashrc, /etc/shadow, ...) →
//     "sensitive path access" (même en lecture : exfiltration de clés)
//   - commande non-read-only (mutation : redirect, tee, -exec, -delete,
//     xargs, sed -i, pipe, ou verbe hors allowlist lecture) →
//     "command modifies state (not read-only)"
//   - sinon (lecture pure + pas sensible) → {} = auto-allow
// Renvoie la raison de l'approval (clé de session) ou une QString vide.
QString checkApprovalRequired(const QString &command);