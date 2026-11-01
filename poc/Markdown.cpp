// a-ice 2.0 — M3a : le decoupage du markdown en blocs (voir Markdown.h).

#include "Markdown.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QThreadPool>
#include <QVariantMap>

namespace {

// Les limites de bloc, reconnues a la source : c'est plus fiable sur de la
// sortie d'agent que de dependre des details internes de QTextDocument.
const QRegularExpression reFence { QStringLiteral("^\\s{0,3}(`{3,}|~{3,})(.*)$") };
const QRegularExpression reHeading { QStringLiteral("^(#{1,6})[ \\t]+(.*?)[ \\t#]*$") };
const QRegularExpression reRule { QStringLiteral("^\\s{0,3}([-*_])([ ]{0,2}\\1){2,}[ \\t]*$") };
const QRegularExpression reQuote { QStringLiteral("^\\s{0,3}>[ \\t]?(.*)$") };
const QRegularExpression reList { QStringLiteral("^\\s{0,6}([-*+]|\\d{1,6}[.)])[ \\t]+(.*)$") };
const QRegularExpression reTask { QStringLiteral("^\\s*([-*+])[ \\t]+\\[([ xX])\\][ \\t]+(.*)$") };

/*
 * Le code en ligne : Qt le comprend tres bien mais ne lui rend aucun style dans
 * toHtml() (une experience menee ici meme : `code` devient du texte nu). On le
 * marque donc d'un caractere reserve (zone d'usage prive) avant le parse, et on
 * le remanie en span apres le parse.
 */
const QRegularExpression reInlineCode { QStringLiteral("`[^`\\n]+`") };
const QChar codeMark { QChar(0xE000) };
const QChar codeMarkEnd { QChar(0xE001) };

// QStringLiteral ne supporte pas les echappements : le span du code en ligne
// est ecrit en chaine brute.
const QString codeOpenTag { QString::fromLatin1(R"(<span style=" font-family:'Ubuntu Mono'; color:#cfe0ff;">)") };
const QString codeCloseTag { QStringLiteral("</span>") };

bool isBlank(const QString &line)
{
    return line.trimmed().isEmpty();
}

bool isTableRow(const QString &line)
{
    return line.trimmed().startsWith(QLatin1Char('|'));
}

QVariantMap makeBlock(const QString &kind, int level, bool mono,
                     const QString &html, const QString &text)
{
    QVariantMap block;
    block.insert(QStringLiteral("kind"), kind);
    block.insert(QStringLiteral("level"), level);
    block.insert(QStringLiteral("mono"), mono);
    block.insert(QStringLiteral("html"), html);
    block.insert(QStringLiteral("text"), text);
    // La mesure de la carte se fait sur le texte rendu (sans les marques du
    // markdown) : c'est la largeur que la carte doit epouser.
    block.insert(QStringLiteral("plain"),
                 html.isEmpty() ? text
                                : QTextDocumentFragment::fromHtml(html).toPlainText());
    return block;
}

/*
 * Le HTML d'un seul bloc : un fragment, pas un document. L'air entre les blocs
 * reste a la carte (Qt ecrase de toute facon les marges CSS par des marges
 * inline, et les met a zero de toute facon) — la v1 l'avait mesure.
 */
QString fragmentHtml(const QString &source)
{
    // 1. marquer le code en ligne, que le parseur de Qt ne rend pas
    QString prepared;
    int cursor = 0;
    QRegularExpressionMatchIterator marks = reInlineCode.globalMatch(source);
    while (marks.hasNext()) {
        const QRegularExpressionMatch match = marks.next();
        prepared += source.mid(cursor, match.capturedStart() - cursor);
        prepared += codeMark;
        prepared += source.mid(match.capturedStart() + 1, match.capturedLength() - 2);
        prepared += codeMarkEnd;
        cursor = match.capturedStart() + match.capturedLength();
    }
    prepared += source.mid(cursor);

    const QTextDocumentFragment fragment =
        QTextDocumentFragment::fromMarkdown(prepared, QTextDocument::MarkdownDialectGitHub);

    // 2. garder seulement ce que la carte peut rendre : toHtml() d'un fragment
    //    rend un document HTML complet (DOCTYPE, <head> et son <style>).
    QString html = fragment.toHtml();
    const int bodyStart = html.indexOf(QStringLiteral("<body>"));
    const int bodyEnd = html.lastIndexOf(QStringLiteral("</body>"));
    if (bodyStart >= 0 && bodyEnd > bodyStart)
        html = html.mid(bodyStart + 6, bodyEnd - bodyStart - 6);
    html.remove(QStringLiteral("<!--StartFragment-->"));
    html.remove(QStringLiteral("<!--EndFragment-->"));

    // 3. le code en ligne retrouve son compte
    html.replace(codeMark, codeOpenTag);
    html.replace(codeMarkEnd, codeCloseTag);

    // Qt cree avec la couleur par defaut du document. Le corps est encre claire.
    html.replace(QStringLiteral("color:#000000"), QStringLiteral("color:#e9eaee"));
    return html.trimmed();
}

/*
 * Un titre se rend avec la police et la taille de la carte, pas avec celles que
 * Qt attache a <h1>. On garde donc son texte, debarrasse de ses marques.
 */
QString headingText(const QString &source)
{
    QString text = source;
    text.remove(QStringLiteral("**"));
    text.remove(QStringLiteral("__"));
    text.remove(QStringLiteral("~~"));
    text.remove(QChar('`'));
    return text.trimmed();
}

/*
 * Une liste de cases a cocher : Qt les rend en images de case (Task*), que le
 * texte riche de Qt Quick ne sait pas dessiner. On les remplace par des glyphes.
 */
bool taskLines(const QStringList &lines, QString &out)
{
    QStringList rendered;
    for (const QString &line : lines) {
        const QRegularExpressionMatch match = reTask.match(line);
        if (!match.hasMatch())
            return false;
        const QString indent = line.left(match.capturedStart(1));
        rendered << (indent + (match.captured(2) == QLatin1Char(' ')
                                   ? QStringLiteral("\u2610 ")   // case vide
                                   : QStringLiteral("\u2611 "))  // case cochee
                    + match.captured(3).trimmed());
    }
    out = rendered.join(QChar('\n'));
    return true;
}

// Une ligne qui ouvre un nouveau bloc coupe le paragraphe en cours.
bool startsBlock(const QString &line)
{
    return isBlank(line)
           || reFence.match(line).hasMatch()
           || reHeading.match(line).hasMatch()
           || reRule.match(line).hasMatch()
           || reQuote.match(line).hasMatch()
           || isTableRow(line)
           || reList.match(line).hasMatch();
}

} // namespace

QVariantList markdownBlocks(const QString &markdown)
{
    QVariantList blocks;
    QString normalized = markdown;
    normalized.replace(QChar('\r'), QChar('\n'));
    const QStringList lines = normalized.split(QChar('\n'));

    int i = 0;
    while (i < lines.size()) {
        if (isBlank(lines.at(i))) {
            ++i;
            continue;
        }

        const QString line = lines.at(i);

        // --- bloc de code fence : son texte brut, tel quel, en monospace
        const QRegularExpressionMatch fence = reFence.match(line);
        if (fence.hasMatch()) {
            const QChar fenceChar = fence.captured(1).at(0);
            const int fenceLen = fence.captured(1).size();
            QStringList body;
            ++i;
            while (i < lines.size()) {
                const QRegularExpressionMatch closer = reFence.match(lines.at(i));
                if (closer.hasMatch() && closer.captured(1).at(0) == fenceChar
                    && closer.captured(1).size() >= fenceLen
                    && closer.captured(2).trimmed().isEmpty()) {
                    ++i;
                    break;
                }
                body << lines.at(i);
                ++i;
            }
            blocks << makeBlock(QStringLiteral("code"), 0, true, QString(),
                                body.join(QChar('\n')));
            continue;
        }

        // --- titre
        const QRegularExpressionMatch heading = reHeading.match(line);
        if (heading.hasMatch()) {
            blocks << makeBlock(QStringLiteral("heading"), heading.captured(1).size(),
                                false, QString(), headingText(heading.captured(2)));
            ++i;
            continue;
        }

        // --- filet
        if (reRule.match(line).hasMatch()) {
            blocks << makeBlock(QStringLiteral("rule"), 0, false, QString(), QString());
            ++i;
            continue;
        }

        // --- tableau : rendu en lignes alignees (monospace), pas en HTML
        if (isTableRow(line)) {
            QStringList rows;
            while (i < lines.size() && isTableRow(lines.at(i))) {
                rows << lines.at(i).trimmed();
                ++i;
            }
            blocks << makeBlock(QStringLiteral("table"), 0, true, QString(),
                                rows.join(QChar('\n')));
            continue;
        }

        // --- citation
        if (reQuote.match(line).hasMatch()) {
            QStringList quoted;
            while (i < lines.size()) {
                const QRegularExpressionMatch quote = reQuote.match(lines.at(i));
                if (!quote.hasMatch())
                    break;
                quoted << quote.captured(1);
                ++i;
            }
            const QString source = quoted.join(QChar('\n'));
            blocks << makeBlock(QStringLiteral("quote"), 0, false, fragmentHtml(source), source);
            continue;
        }

        // --- liste (et liste de cases), poursuivie par ses lignes alouees
        if (reList.match(line).hasMatch()) {
            QStringList items;
            while (i < lines.size()) {
                const QString &current = lines.at(i);
                if (isBlank(current))
                    break;
                if (!reList.match(current).hasMatch()
                    && !(current.startsWith(QLatin1Char(' ')) || current.startsWith(QLatin1Char('\t'))))
                    break;
                items << current;
                ++i;
            }

            QString tasks;
            if (taskLines(items, tasks)) {
                blocks << makeBlock(QStringLiteral("task"), 0, false, QString(), tasks);
                continue;
            }
            const QString source = items.join(QChar('\n'));
            blocks << makeBlock(QStringLiteral("list"), 0, false, fragmentHtml(source), source);
            continue;
        }

        // --- paragraphe : jusqu'a la ligne blanche ou l'ouverture d'un autre bloc
        QStringList paragraph;
        while (i < lines.size() && !startsBlock(lines.at(i))) {
            paragraph << lines.at(i);
            ++i;
        }
        const QString source = paragraph.join(QChar('\n'));
        blocks << makeBlock(QStringLiteral("para"), 0, false, fragmentHtml(source), source);
    }

    return blocks;
}

// --- le parseur vivant, cote corps -------------------------------------------

namespace {
// L'empreinte du texte sert a ne jamais re-parser une bulle figee : une
// conversation reprise rejoute les memes messages, et un scroll dans une liste
// de 2 000 bulles ne doit couter aucun parse.
QMutex cacheLock;
QHash<QString, QVariantList> cacheByKey;
QStringList cacheOrder;
constexpr int cacheMax = 128;
} // namespace

Markdown::Markdown(QObject *parent)
    : QObject(parent)
{
}

void Markdown::parse(const QString &messageId, const QString &markdown)
{
    if (messageId.isEmpty() || markdown.trimmed().isEmpty())
        return;

    {
        const QMutexLocker lock(&cacheLock);
        const auto found = cacheByKey.constFind(markdown);
        if (found != cacheByKey.constEnd()) {
            const QVariantList blocks = found.value();
            emit parsed(messageId, blocks);
            return;
        }
    }

    // QTextDocument ne survit pas a un changement de thread : sa vie est celle
    // de la tache, la-dedans. Aucun parent : sa destruction reste dans le thread
    // du pool qui l'a cree. Pas de QFuture attendu : le resultat remonte par
    // parsed(), le pool global suffit.
    QThreadPool::globalInstance()->start([this, messageId, markdown]() {
        const QVariantList blocks = markdownBlocks(markdown);

        {
            const QMutexLocker lock(&cacheLock);
            cacheByKey.insert(markdown, blocks);
            cacheOrder.append(markdown);
            while (cacheOrder.size() > cacheMax) {
                cacheByKey.remove(cacheOrder.takeFirst());
            }
        }

        emit parsed(messageId, blocks);
    });
}
