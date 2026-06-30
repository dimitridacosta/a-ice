#pragma once

#include "../Tool.h"
#include <QNetworkAccessManager>

class BraveSearchTool : public Tool
{
    Q_OBJECT
public:
    explicit BraveSearchTool(QObject *parent = nullptr)
        : Tool(parent), m_nam(new QNetworkAccessManager(this)) {}
    explicit BraveSearchTool(const QString &apiKey, QObject *parent = nullptr);

    Spec spec() const override;
    void execute(const QJsonObject &args,
                 std::function<void(bool ok, QString result)> cb) override;

private:
    QNetworkAccessManager *m_nam;
    QString m_apiKey;
};