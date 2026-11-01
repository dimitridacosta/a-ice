// a-ice 2.0 — M3 : le parseur markdown hors UI thread (voir Markdown.h).

#include "Markdown.h"

#include <QTextDocument>
#include <QThreadPool>

Markdown::Markdown(QObject *parent)
    : QObject(parent)
{
}

void Markdown::parse(const QString &messageId, const QString &markdown)
{
    if (messageId.isEmpty() || markdown.trimmed().isEmpty())
        return;

    // QTextDocument ne survit pas a un changement de thread : sa vie est celle
    // de la tache, la-dedans. Aucun parent : sa destruction reste dans le thread
    // du pool qui l'a cree. Pas de QFuture attendu : le resultat remonte par
    // parsed(), le pool global suffit.
    QThreadPool::globalInstance()->start([this, messageId, markdown]() {
        QTextDocument doc;
        doc.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
        emit parsed(messageId, doc.toHtml());
    });
}