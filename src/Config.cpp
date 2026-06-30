#include "Config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTextStream>
#include <QDebug>

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

    // Tools (function calling) : optionnel.
    const QJsonObject tools = root.value(QStringLiteral("tools")).toObject();
    if (tools.contains(QStringLiteral("enabled"))) {
        m_tools.enabled = tools.value(QStringLiteral("enabled")).toBool(m_tools.enabled);
    }
    if (tools.contains(QStringLiteral("brave_api_key"))) {
        m_tools.braveApiKey = tools.value(QStringLiteral("brave_api_key")).toString();
    }
    if (tools.contains(QStringLiteral("terminal_workdir"))) {
        m_tools.terminalWorkdir = tools.value(QStringLiteral("terminal_workdir")).toString();
    }
    // Override via env var BRAVE_API_KEY (priorité sur config).
    const QByteArray envKey = qgetenv("BRAVE_API_KEY");
    if (!envKey.isEmpty()) {
        m_tools.braveApiKey = QString::fromUtf8(envKey);
    }
    // Si terminal_workdir vide → home.
    if (m_tools.terminalWorkdir.isEmpty()) {
        m_tools.terminalWorkdir = QDir::homePath();
    }

    qInfo() << "[a-ice] tools: enabled=" << m_tools.enabled
            << "brave=" << (!m_tools.braveApiKey.isEmpty() ? "configured" : "-")
            << "workdir=" << m_tools.terminalWorkdir;

    // Chargement du SOUL.md (prompt système) : à côté de config.json,
    // sinon fallback sur share/a-ice/SOUL.md installé.
    const QString configDir = QFileInfo(target).absolutePath();
    m_soulPath = loadSoul(configDir);
    if (!m_soulPath.isEmpty()) {
        QFile sf(m_soulPath);
        if (sf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            m_systemPrompt = QString::fromUtf8(sf.readAll());
            sf.close();
            qInfo() << "[a-ice] SOUL chargé:" << m_soulPath
                    << "(" << m_systemPrompt.size() << "chars)";
        }
    } else {
        m_systemPrompt.clear();
        qInfo() << "[a-ice] SOUL.md absent (pas de prompt système)";
    }

    return true;
}

QString Config::loadSoul(const QString &configDir)
{
    // 1. À côté de config.json (~/.config/a-ice/SOUL.md).
    const QString local = QDir(configDir).absoluteFilePath(QStringLiteral("SOUL.md"));
    if (QFileInfo::exists(local))
        return local;

    // 2. share/a-ice/SOUL.md installé (AppDataLocation couvre
    //    ~/.local/share/a-ice, /usr/local/share/a-ice, /usr/share/a-ice).
    const QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::AppDataLocation);
    for (const QString &d : dirs) {
        const QString p = QDir(d).absoluteFilePath(QStringLiteral("SOUL.md"));
        if (QFileInfo::exists(p))
            return p;
    }
    return {};
}