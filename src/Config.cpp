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
    // Defaults = modèle local de Dimitri (cf. settings.json Zed).
    m_providers.clear();
    Provider p;
    p.id = QStringLiteral("default");
    p.type = QStringLiteral("openai_compatible");
    p.apiUrl = QStringLiteral("http://localhost:18081/v1");
    p.promptFormat = QStringLiteral("qwen");
    Model m;
    m.alias = QStringLiteral("qwen");
    m.name = QStringLiteral("qwen36-28b-reap");
    m.temperature = 0.7;
    m.maxTokens = 64;
    m.stream = false;
    p.models.append(m);
    m_providers.append(p);

    m_currentProviderId = p.id;
    m_currentModelAlias = m.alias;

    syncCurrent();
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

void Config::syncCurrent()
{
    // Retrouve le provider courant.
    const Provider *pp = nullptr;
    for (const auto &p : m_providers) {
        if (p.id == m_currentProviderId) {
            pp = &p;
            break;
        }
    }
    if (!pp && !m_providers.isEmpty())
        pp = &m_providers.first();
    if (!pp) {
        // Aucun provider : on garde l'ancienne vue (defaults au pire).
        return;
    }
    m_provider = *pp;
    // Normalise api_url (trailing slash) pour la vue courante.
    if (!m_provider.apiUrl.isEmpty() && !m_provider.apiUrl.endsWith('/'))
        m_provider.apiUrl += '/';

    // Retrouve le modèle courant dans ce provider.
    const Model *mm = nullptr;
    for (const auto &mdl : pp->models) {
        if (mdl.alias == m_currentModelAlias) {
            mm = &mdl;
            break;
        }
    }
    if (!mm && !pp->models.isEmpty())
        mm = &pp->models.first();
    if (mm) {
        m_model = *mm;
        m_currentModelAlias = mm->alias;
    }
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

    // ---- Providers : nouveau schéma multi-providers ----
    QList<Provider> providers;
    const QJsonObject providersObj = root.value(QStringLiteral("providers")).toObject();
    if (!providersObj.isEmpty()) {
        for (auto it = providersObj.begin(); it != providersObj.end(); ++it) {
            const QJsonObject po = it.value().toObject();
            Provider p;
            p.id = it.key();
            // "type" (nouveau) en priorité, fallback "name" (rétro-compat).
            if (po.contains(QStringLiteral("type")))
                p.type = po.value(QStringLiteral("type")).toString();
            else if (po.contains(QStringLiteral("name")))
                p.type = po.value(QStringLiteral("name")).toString();
            if (po.contains(QStringLiteral("api_url")))
                p.apiUrl = po.value(QStringLiteral("api_url")).toString();
            if (po.contains(QStringLiteral("prompt_format")))
                p.promptFormat = po.value(QStringLiteral("prompt_format")).toString();
            if (po.contains(QStringLiteral("api_key")))
                p.apiKey = po.value(QStringLiteral("api_key")).toString();

            const QJsonObject modelsObj = po.value(QStringLiteral("models")).toObject();
            for (auto mit = modelsObj.begin(); mit != modelsObj.end(); ++mit) {
                const QJsonObject mo = mit.value().toObject();
                Model m;
                m.alias = mit.key();
                if (mo.contains(QStringLiteral("name"))) {
                    m.name = mo.value(QStringLiteral("name")).toString();
                } else {
                    // Si pas de name explicite, l'alias fait office de name.
                    m.name = m.alias;
                }
                if (mo.contains(QStringLiteral("temperature")))
                    m.temperature = mo.value(QStringLiteral("temperature")).toDouble(m.temperature);
                if (mo.contains(QStringLiteral("max_tokens")))
                    m.maxTokens = static_cast<int>(mo.value(QStringLiteral("max_tokens")).toInt(m.maxTokens));
                if (mo.contains(QStringLiteral("stream")))
                    m.stream = mo.value(QStringLiteral("stream")).toBool(m.stream);
                p.models.append(m);
            }
            if (!p.models.isEmpty())
                providers.append(p);
        }
    }

    // ---- Schéma legacy : provider/model au top-level ----
    // Reconstruit un provider "default" si "providers" est absent ou vide.
    if (providers.isEmpty()) {
        const QJsonObject provider = root.value(QStringLiteral("provider")).toObject();
        const QJsonObject model = root.value(QStringLiteral("model")).toObject();
        if (!provider.isEmpty() || !model.isEmpty()) {
            Provider p;
            p.id = QStringLiteral("default");
            // "type" en priorité, fallback "name" (rétro-compat legacy).
            if (provider.contains(QStringLiteral("type")))
                p.type = provider.value(QStringLiteral("type")).toString();
            else if (provider.contains(QStringLiteral("name")))
                p.type = provider.value(QStringLiteral("name")).toString();
            else
                p.type = QStringLiteral("openai_compatible");
            if (provider.contains(QStringLiteral("api_url")))
                p.apiUrl = provider.value(QStringLiteral("api_url")).toString();
            if (provider.contains(QStringLiteral("prompt_format")))
                p.promptFormat = provider.value(QStringLiteral("prompt_format")).toString();
            if (provider.contains(QStringLiteral("api_key")))
                p.apiKey = provider.value(QStringLiteral("api_key")).toString();

            Model m;
            m.alias = model.value(QStringLiteral("name")).toString();
            if (m.alias.isEmpty())
                m.alias = QStringLiteral("qwen36-28b-reap");
            m.name = m.alias;
            if (model.contains(QStringLiteral("temperature")))
                m.temperature = model.value(QStringLiteral("temperature")).toDouble(m.temperature);
            if (model.contains(QStringLiteral("max_tokens")))
                m.maxTokens = static_cast<int>(model.value(QStringLiteral("max_tokens")).toInt(m.maxTokens));
            if (model.contains(QStringLiteral("stream")))
                m.stream = model.value(QStringLiteral("stream")).toBool(m.stream);
            p.models.append(m);
            providers.append(p);
        }
    }

    // Si rien n'a été parsé, on garde les defaults (déjà posés par applyDefaults).
    if (!providers.isEmpty()) {
        m_providers = providers;
        m_currentProviderId = root.value(QStringLiteral("default_provider")).toString();
        m_currentModelAlias = root.value(QStringLiteral("default_model")).toString();
        if (m_currentProviderId.isEmpty())
            m_currentProviderId = m_providers.first().id;
        if (m_currentModelAlias.isEmpty())
            m_currentModelAlias = m_providers.first().models.first().alias;
        syncCurrent();
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
    if (tools.contains(QStringLiteral("max_tool_iterations"))) {
        m_tools.maxToolIterations = tools.value(QStringLiteral("max_tool_iterations")).toInt(m_tools.maxToolIterations);
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
            << "workdir=" << m_tools.terminalWorkdir
            << "max_tool_iterations=" << m_tools.maxToolIterations;

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

    qInfo() << "[a-ice] providers:" << m_providers.size()
            << "courant: provider=" << m_currentProviderId
            << "model alias=" << m_currentModelAlias
            << "name=" << m_model.name;

    return true;
}

bool Config::switchModel(const QString &aliasOrName, QString *message)
{
    if (m_providers.isEmpty()) {
        if (message) *message = QStringLiteral("Aucun provider configuré.");
        return false;
    }
    const QString key = aliasOrName.trimmed();
    if (key.isEmpty()) {
        if (message) *message = QStringLiteral("Alias vide.");
        return false;
    }

    // 1. Provider courant d'abord : par alias puis par name.
    const Provider *curP = nullptr;
    for (const auto &p : m_providers) {
        if (p.id == m_currentProviderId) { curP = &p; break; }
    }
    if (curP) {
        for (const auto &m : curP->models) {
            if (m.alias == key || m.name == key) {
                m_currentModelAlias = m.alias;
                syncCurrent();
                if (message)
                    *message = QStringLiteral("Modèle : %1 (%2) @ %3")
                        .arg(m.alias, m.name, curP->id);
                qInfo() << "[a-ice] switch model ->" << m.alias << "/" << m.name
                        << "provider" << curP->id;
                return true;
            }
        }
    }

    // 2. Autres providers : par alias puis par name.
    for (const auto &p : m_providers) {
        if (p.id == m_currentProviderId) continue;
        for (const auto &m : p.models) {
            if (m.alias == key || m.name == key) {
                m_currentProviderId = p.id;
                m_currentModelAlias = m.alias;
                syncCurrent();
                if (message)
                    *message = QStringLiteral("Modèle : %1 (%2) @ %3")
                        .arg(m.alias, m.name, p.id);
                qInfo() << "[a-ice] switch model+provider ->" << m.alias << "/"
                        << m.name << "provider" << p.id;
                return true;
            }
        }
    }

    // 3. Introuvable : liste les alias dispo pour aider.
    QString list;
    for (const auto &p : m_providers) {
        for (const auto &m : p.models) {
            if (!list.isEmpty()) list += QStringLiteral(", ");
            list += m.alias;
        }
    }
    if (message)
        *message = QStringLiteral("Modèle introuvable : '%1'. Disponibles : %2.")
            .arg(key, list);
    return false;
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