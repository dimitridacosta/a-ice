#include "Config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

Config::Config()
{
    applyDefaults();
}

void Config::applyDefaults()
{
    m_provider.name = QStringLiteral("openai_compatible");
    m_provider.apiUrl = QStringLiteral("http://localhost:18081/v1");
    m_provider.promptFormat = QStringLiteral("qwen");

    m_model.name = QStringLiteral("qwen36-28b-reap");
    m_model.temperature = 0.7;
    m_model.maxTokens = 64;
    m_model.stream = false;
}

// Valeurs par défaut = modèle local de Dimitri (cf. settings.json Zed).
Config Config::defaultLocal()
{
    Config c;
    c.applyDefaults();
    return c;
}

QString Config::defaultConfigPath()
{
    // ~/.config/a-ice/config.json
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return QDir(base).filePath(QStringLiteral("a-ice/config.json"));
}

bool Config::load(const QString &path)
{
    const QString target = path.isEmpty() ? defaultConfigPath() : path;
    m_configPath = target;

    QFileInfo info(target);
    if (!info.exists() || !info.isFile()) {
        // Fichier absent : on garde les valeurs par défaut.
        return false;
    }

    QFile file(target);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();

    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        // JSON invalide : on garde les valeurs par défaut.
        return false;
    }

    const QJsonObject root = doc.object();

    // Provider
    const QJsonObject provider = root.value(QStringLiteral("provider")).toObject();
    if (provider.contains(QStringLiteral("name"))) {
        m_provider.name = provider.value(QStringLiteral("name")).toString();
    }
    if (provider.contains(QStringLiteral("api_url"))) {
        m_provider.apiUrl = provider.value(QStringLiteral("api_url")).toString();
    }
    if (provider.contains(QStringLiteral("prompt_format"))) {
        m_provider.promptFormat = provider.value(QStringLiteral("prompt_format")).toString();
    }

    // Model
    const QJsonObject model = root.value(QStringLiteral("model")).toObject();
    if (model.contains(QStringLiteral("name"))) {
        m_model.name = model.value(QStringLiteral("name")).toString();
    }
    if (model.contains(QStringLiteral("temperature"))) {
        m_model.temperature = model.value(QStringLiteral("temperature")).toDouble(m_model.temperature);
    }
    if (model.contains(QStringLiteral("max_tokens"))) {
        m_model.maxTokens = static_cast<int>(model.value(QStringLiteral("max_tokens")).toInt(m_model.maxTokens));
    }
    if (model.contains(QStringLiteral("stream"))) {
        m_model.stream = model.value(QStringLiteral("stream")).toBool(m_model.stream);
    }

    // Normalisation : api_url doit se terminer par "/" pour pouvoir concaténer "chat/completions".
    if (!m_provider.apiUrl.isEmpty() && !m_provider.apiUrl.endsWith('/')) {
        m_provider.apiUrl += '/';
    }

    return true;
}