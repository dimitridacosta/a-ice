#include "BraveSearchTool.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonArray>
#include <QTimer>
#include <QUrl>
#include <QPointer>
#include <QJsonArray>
#include <algorithm>

BraveSearchTool::BraveSearchTool(const QString &apiKey, QObject *parent)
    : Tool(parent), m_nam(new QNetworkAccessManager(this)), m_apiKey(apiKey)
{
    const QByteArray env = qgetenv("BRAVE_API_KEY");
    if (!env.isEmpty())
        m_apiKey = QString::fromUtf8(env);
}

Tool::Spec BraveSearchTool::spec() const
{
    QJsonObject params;
    params["type"] = QStringLiteral("object");

    QJsonObject props;
    QJsonObject query;
    query["type"] = QStringLiteral("string");
    query["description"] = QStringLiteral("The search query");
    props["query"] = query;

    QJsonObject count;
    count["type"] = QStringLiteral("integer");
    count["description"] = QStringLiteral("Number of results (default 5, max 20)");
    props["count"] = count;

    params["properties"] = props;
    params["required"] = QJsonArray{QStringLiteral("query")};

    return Spec{
        QStringLiteral("brave_search"),
        QStringLiteral("Search the web using Brave Search. Returns up to N results "
                       "with title, URL and snippet. Use for current information, "
                       "documentation lookups, news. Do NOT use for fetching full "
                       "page content (use fetch_url instead)."),
        params
    };
}

void BraveSearchTool::execute(const QJsonObject &args,
                               std::function<void(bool ok, QString result)> cb)
{
    if (m_apiKey.isEmpty()) {
        const QByteArray env = qgetenv("BRAVE_API_KEY");
        if (!env.isEmpty())
            m_apiKey = QString::fromUtf8(env);
    }

    if (m_apiKey.isEmpty()) {
        cb(false, QStringLiteral("[a-ice] Brave API key not configured "
                                 "(set BRAVE_API_KEY env var or brave_api_key in config.json)"));
        return;
    }

    const QString query = args.value(QStringLiteral("query")).toString().trimmed();
    if (query.isEmpty()) {
        cb(false, QStringLiteral("[a-ice] brave_search: empty query"));
        return;
    }

    int count = args.value(QStringLiteral("count")).toInt(5);
    count = std::clamp(count, 1, 20);

    QUrl url(QStringLiteral("https://api.search.brave.com/res/v1/web/search"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("q"), query);
    q.addQueryItem(QStringLiteral("count"), QString::number(count));
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setRawHeader("X-Subscription-Token", m_apiKey.toUtf8());
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("Accept-Encoding", "gzip");

    QNetworkReply *reply = m_nam->get(req);

    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    timer->setInterval(15000);

    QPointer<QNetworkReply> guard(reply);
    QObject::connect(timer, &QTimer::timeout, this, [cb, guard, timer]() {
        timer->deleteLater();
        if (guard) {
            guard->abort();
            cb(false, QStringLiteral("[a-ice] brave_search: Timeout after 15s"));
        }
    });

    QObject::connect(reply, &QNetworkReply::finished, this, [cb, guard, timer]() {
        timer->stop();
        timer->deleteLater();
        if (!guard) return;

        QNetworkReply *r = guard.data();
        const int httpCode = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray body = r->readAll();
        r->deleteLater();

        if (httpCode != 200) {
            QString excerpt = QString::fromUtf8(body).left(300);
            cb(false, QStringLiteral("[a-ice] Brave search failed: HTTP %1 %2")
                           .arg(httpCode).arg(excerpt));
            return;
        }

        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
        if (parseErr.error != QJsonParseError::NoError) {
            cb(false, QStringLiteral("[a-ice] Brave search: invalid JSON: %1")
                           .arg(parseErr.errorString()));
            return;
        }

        const QJsonArray results = doc.object()
            .value(QStringLiteral("web")).toObject()
            .value(QStringLiteral("results")).toArray();

        if (results.isEmpty()) {
            cb(true, QStringLiteral("No results found for: %1").arg(QString::fromUtf8(body)));
            return;
        }

        QString md;
        for (const QJsonValue &v : results) {
            const QJsonObject o = v.toObject();
            const QString title = o.value(QStringLiteral("title")).toString();
            const QString urlStr = o.value(QStringLiteral("url")).toString();
            const QString desc = o.value(QStringLiteral("description")).toString();
            md += QStringLiteral("## %1\n%2\n%3\n\n")
                      .arg(title, urlStr, desc);
        }

        cb(true, md);
    });
}