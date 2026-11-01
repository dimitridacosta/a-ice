// a-ice 2.0 — M3a : le markdown des reponses, parse HORS de l'UI thread.
//
// La v1 rendait le markdown avec un seul QTextDocument pose dans un QLabel : la
// mesure et le rendu se passaient dans le thread de dessin, et c'est exactement
// ce qui faisait lagger la bulle des qu'une reponse faisait plusieurs blocs.
// Ici, deux choses changent :
//   - le decoupage se fait a la source (bloc par bloc), pas a travers un
//     QTextDocument entier : chaque bloc a son propre HTML de fragment, et la
//     carte peut continuer d'epouser son texte ;
//   - le decoupage tourne dans le pool QtConcurrent/QThreadPool, et le resultat
//     remonte par un signal requeue.
//
// Le modele d'une bulle devient donc { role, raw, msgId, blocks } : `text` reste
// la source brute (le streaming qui coule, le copier-coller), `blocks` est le
// rendu. Tant qu'une bulle est en streaming elle reste en texte brut ; elle bascule
// en blocs quand parsed() lui rapporte ce qu'elle vaut.
//
// Voir docs/v2-rewrite/M3-md-permissions-questions.md (section 4, voie B).

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

/*
 * Decoupe un markdown en blocs exploitables par la carte. Fonction libre : le
 * test de tests/test_md.cpp la teste sans corps ni hub.
 *
 * Chaque bloc est un QVariantMap :
 *   kind   "para" | "heading" | "code" | "list" | "task" | "quote" | "table" | "rule"
 *   level  0 sauf pour un titre (1 a 6)
 *   mono   vrai pour code et table : le texte brut est rendu en monospace
 *   html   le rendu riche du bloc (vide quand le bloc se rend en texte brut)
 *   text   le contenu brut, pour le mono et les titres
 *
 * Les styles en ligne (gras, italique, lien, code) viennent du moteur markdown de
 * Qt : on ne les reecrit pas.
 */
QVariantList markdownBlocks(const QString &markdown);

class Markdown : public QObject
{
    Q_OBJECT

public:
    explicit Markdown(QObject *parent = nullptr);

    /*
     * Parse la bulle fermee de messageId. Le texte est duplique dans la tache
     * (aucune reference a un objet vivant de l'UI), la liste de blocs revient
     * sur le thread de l'UI.
     *
     * Une bulle figee qui refait surface (reprise d'historique) est retrouvee par
     * son empreinte : elle ne se re-parse pas.
     */
    Q_INVOKABLE void parse(const QString &messageId, const QString &markdown);

signals:
    void parsed(const QString &messageId, const QVariantList &blocks);
};
