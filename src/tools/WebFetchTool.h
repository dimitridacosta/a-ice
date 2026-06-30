#pragma once

#include "../Tool.h"
#include <QNetworkAccessManager>

class WebFetchTool : public Tool
{
    Q_OBJECT
public:
    explicit WebFetchTool(QObject *parent = nullptr)
        : Tool(parent), m_nam(new QNetworkAccessManager(this)) {}

    Spec spec() const override;
    void execute(const QJsonObject &args,
                 std::function<void(bool ok, QString result)> cb) override;

private:
    QNetworkAccessManager *m_nam;
};