#include "WebFetchTool.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QTimer>
#include <QUrl>
#include <QTextDocument>
#include <QRegularExpression>
#include <QPointer>
#include <QJsonArray>
#include <algorithm>

Tool::Spec WebFetchTool::spec() const
{
    QJsonObject params;
    params["type"] = QStringLiteral("object");

    QJsonObject props;
    QJsonObject url;
    url["type"] = QStringLiteral("string");
    url["description"] = QStringLiteral("The HTTP(S) URL to fetch");
    props["url"] = url;

    QJsonObject raw;
    raw["type"] = QStringLiteral("boolean");
    raw["description"] = QStringLiteral("If true, return raw text without markdown conversion (default false)");
    props["raw"] = raw;

    params["properties"] = props;
    params["required"] = QJsonArray{QStringLiteral("url")};

    return Spec{
        QStringLiteral("fetch_url"),
        QStringLiteral("Fetch a web page and return its content as markdown. "
                       "Good for reading articles, documentation, raw text. "
                       "Strips scripts/styles. Returns markdown (links, headings, "
                       "lists, paragraphs). Truncated to 30000 chars."),
        params
    };
}

static QString collapseBlankLines(const QString &in)
{
    static const QRegularExpression re(QStringLiteral("\n{3,}"));
    QString out = in;
    out.replace(re, QStringLiteral("\n\n"));
    return out;
}

static QString stripBasicHtml(const QString &in)
{
    QString out = in;
    static const QRegularExpression script(QStringLiteral("<script[^>]*>.*?</script>"),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression style(QStringLiteral("<style[^>]*>.*?</style>"),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    out.remove(script);
    out.remove(style);
    static const QRegularExpression tags(QStringLiteral("<[^>]+>"));
    out.replace(tags, QStringLiteral(" "));
    out.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    out = out.trimmed();
    return collapseBlankLines(out);
}

static QString cleanMarkdown(const QString &md)
{
    static const QRegularExpression imgLine(
        QStringLiteral("^\\s*!\\[[^\\]]*\\]\\([^)]*\\)\\s*$"),
        QRegularExpression::MultilineOption);
    static const QRegularExpression imgTag(
        QStringLiteral("<img[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    QString out = md;
    out.remove(imgLine);
    out.remove(imgTag);
    return collapseBlankLines(out);
}

void WebFetchTool::execute(const QJsonObject &args,
                           std::function<void(bool ok, QString result)> cb)
{
    const QString urlStr = args.value(QStringLiteral("url")).toString().trimmed();
    if (urlStr.isEmpty()) {
        cb(false, QStringLiteral("[a-ice] fetch_url: empty url"));
        return;
    }
    const QUrl url(urlStr);
    if (!url.isValid() ||
        (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))) {
        cb(false, QStringLiteral("[a-ice] fetch_url: Only http/https URLs allowed"));
        return;
    }

    const bool raw = args.value(QStringLiteral("raw")).toBool(false);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("A-Ice/0.1 (KHTML like Gecko)"));

    QNetworkReply *reply = m_nam->get(req);

    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    timer->setInterval(20000);

    QPointer<QNetworkReply> guard(reply);
    QObject::connect(timer, &QTimer::timeout, this, [cb, guard, timer]() {
        timer->deleteLater();
        if (guard) {
            guard->abort();
            cb(false, QStringLiteral("[a-ice] fetch_url: Timeout after 20s"));
        }
    });

    QObject::connect(reply, &QNetworkReply::finished, this, [cb, guard, timer, raw]() {
        timer->stop();
        timer->deleteLater();
        if (!guard) return;

        QNetworkReply *r = guard.data();
        const int httpCode = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray body = r->readAll();
        const QString contentType = r->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
        r->deleteLater();

        if (httpCode == 0 || httpCode >= 400) {
            cb(false, QStringLiteral("[a-ice] fetch_url: HTTP %1").arg(httpCode));
            return;
        }

        const bool isHtml = contentType.contains(QStringLiteral("html"));

        QString text;
        if (!raw && isHtml) {
            QTextDocument doc;
            doc.setHtml(QString::fromUtf8(body));
            text = cleanMarkdown(doc.toMarkdown());
        } else if (raw && isHtml) {
            text = stripBasicHtml(QString::fromUtf8(body));
        } else {
            text = QString::fromUtf8(body);
        }

        if (text.size() > 30000) {
            text = QStringLiteral("[output truncated]\n") + text.left(30000);
        }

        cb(true, text);
    });
}