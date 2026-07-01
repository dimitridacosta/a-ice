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