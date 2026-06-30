#pragma once

#include <QString>

// Sécurité du terminal — logique isolée du QProcess pour permettre un test
// unitaire qui n'inclut jamais l'exécution shell.
//
// checkHardline() renvoie une QString vide si la commande est autorisée,
// sinon le message d'erreur (hardline, non-bypassable) à renvoyer au modèle.
QString checkHardline(const QString &command);