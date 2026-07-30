// Test unitaire de Config — parsing multi-providers + switchModel à la volée.
//
// Vérifie :
//   - Schéma multi-providers : providers/models parsés, alias vs name,
//     default_provider/default_model appliqués, api_url normalisée.
//   - Schéma legacy (single provider/model) reconstruit en provider "default".
//   - switchModel() : alias prioritaire, fallback sur le nom complet,
//     changement de provider quand l'alias n'existe pas dans le courant,
//     échec + message si introuvable.
//   - applyDefaults() (config absente) garde le modèle local.
//
// Build : link uniquement Config.cpp + Qt6::Core. Run : ./test_config → exit 0.

#include "../src/Config.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTextStream>
#include <QDebug>

#include <cstdio>

static int g_failures = 0;

static void check(bool cond, const QString &what)
{
    if (cond) {
        std::printf("  [OK]   %s\n", qPrintable(what));
    } else {
        std::printf("  [FAIL] %s\n", qPrintable(what));
        ++g_failures;
    }
}

static QString writeFile(const QTemporaryDir &dir, const QString &name, const QString &content)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(content.toUtf8());
        f.close();
    }
    return path;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("a-ice-test");

    // --- Config absente : defaults ---
    {
        Config c;
        check(!c.load(QStringLiteral("/nonexistent/path/config.json")),
              "load(absent) retourne false");
        check(c.providers().size() == 1, "defaults: 1 provider");
        check(c.provider().name == QStringLiteral("openai_compatible"),
              "defaults: provider.name");
        check(c.model().name == QStringLiteral("qwen36-28b-reap"),
              "defaults: model.name");
        check(c.apiUrl().endsWith('/'), "defaults: api_url normalisée (trailing /)");
    }

    // --- Schéma multi-providers ---
    {
        QTemporaryDir dir;
        const QString json = QStringLiteral(
            "{\n"
            "  \"providers\": {\n"
            "    \"local\": {\n"
            "      \"name\": \"openai_compatible\",\n"
            "      \"api_url\": \"http://localhost:18081/v1\",\n"
            "      \"prompt_format\": \"qwen\",\n"
            "      \"models\": {\n"
            "        \"qwen\": { \"name\": \"qwen36-28b-reap\", \"temperature\": 0.7, \"max_tokens\": 65536, \"stream\": true },\n"
            "        \"glm\":  { \"name\": \"glm-5.2\", \"temperature\": 0.7, \"max_tokens\": 65536, \"stream\": true }\n"
            "      }\n"
            "    },\n"
            "    \"cloud\": {\n"
            "      \"name\": \"openai_compatible\",\n"
            "      \"api_url\": \"https://api.exemple.com/v1\",\n"
            "      \"prompt_format\": \"chatml\",\n"
            "      \"models\": {\n"
            "        \"gpt\": { \"name\": \"gpt-4o-mini\", \"temperature\": 0.7, \"max_tokens\": 16384, \"stream\": true }\n"
            "      }\n"
            "    }\n"
            "  },\n"
            "  \"default_provider\": \"local\",\n"
            "  \"default_model\": \"qwen\"\n"
            "}\n");
        const QString path = writeFile(dir, QStringLiteral("config.json"), json);

        Config c;
        check(c.load(path), "multi: load ok");
        check(c.providers().size() == 2, "multi: 2 providers");
        check(c.currentProviderId() == QStringLiteral("local"), "multi: default_provider");
        check(c.currentModelAlias() == QStringLiteral("qwen"), "multi: default_model");
        check(c.model().name == QStringLiteral("qwen36-28b-reap"), "multi: model.name courant");
        check(c.model().stream == true, "multi: stream=true");
        check(c.model().maxTokens == 65536, "multi: max_tokens");
        check(c.apiUrl() == QStringLiteral("http://localhost:18081/v1/"), "multi: api_url normalisée");
        check(c.provider().promptFormat == QStringLiteral("qwen"), "multi: prompt_format");

        // switchModel par alias (même provider).
        QString msg;
        check(c.switchModel(QStringLiteral("glm"), &msg), "switch(glm): alias ok");
        check(c.currentModelAlias() == QStringLiteral("glm"), "switch(glm): alias courant");
        check(c.model().name == QStringLiteral("glm-5.2"), "switch(glm): name");
        check(c.currentProviderId() == QStringLiteral("local"), "switch(glm): provider inchangé");
        check(msg.contains(QStringLiteral("glm")), "switch(glm): message contient alias");

        // switchModel par nom complet (fallback).
        check(c.switchModel(QStringLiteral("qwen36-28b-reap"), &msg), "switch(nom complet): ok");
        check(c.currentModelAlias() == QStringLiteral("qwen"), "switch(nom complet): alias résolu");

        // switchModel qui change de provider (alias inexistant dans local).
        check(c.switchModel(QStringLiteral("gpt"), &msg), "switch(gpt): change provider");
        check(c.currentProviderId() == QStringLiteral("cloud"), "switch(gpt): provider=cloud");
        check(c.model().name == QStringLiteral("gpt-4o-mini"), "switch(gpt): name");
        check(c.apiUrl() == QStringLiteral("https://api.exemple.com/v1/"), "switch(gpt): api_url changée");

        // switchModel introuvable.
        check(!c.switchModel(QStringLiteral("inexistant"), &msg), "switch(inexistant): échec");
        check(msg.contains(QStringLiteral("qwen")) && msg.contains(QStringLiteral("glm")) && msg.contains(QStringLiteral("gpt")),
              "switch(inexistant): message liste les alias dispo");
    }

    // --- Schéma legacy (single provider/model) ---
    {
        QTemporaryDir dir;
        const QString json = QStringLiteral(
            "{\n"
            "  \"provider\": {\n"
            "    \"name\": \"openai_compatible\",\n"
            "    \"api_url\": \"http://localhost:9999/v1\",\n"
            "    \"prompt_format\": \"qwen\"\n"
            "  },\n"
            "  \"model\": {\n"
            "    \"name\": \"mon-modele\",\n"
            "    \"temperature\": 0.5,\n"
            "    \"max_tokens\": 4096,\n"
            "    \"stream\": false\n"
            "  }\n"
            "}\n");
        const QString path = writeFile(dir, QStringLiteral("config.json"), json);

        Config c;
        check(c.load(path), "legacy: load ok");
        check(c.providers().size() == 1, "legacy: 1 provider reconstruit");
        check(c.providers().first().id == QStringLiteral("default"), "legacy: provider id=default");
        check(c.currentProviderId() == QStringLiteral("default"), "legacy: provider courant");
        check(c.model().name == QStringLiteral("mon-modele"), "legacy: model.name");
        check(c.model().temperature == 0.5, "legacy: temperature");
        check(c.model().stream == false, "legacy: stream=false");
        check(c.apiUrl() == QStringLiteral("http://localhost:9999/v1/"), "legacy: api_url normalisée");
        check(c.providers().first().models.size() == 1, "legacy: 1 modèle");
    }

    // --- switchModel sans providers (cas défensif) ---
    {
        Config c; // defaults
        QString msg;
        // /model absent reste fonctionnel : on ne peut pas casser l'état par défaut.
        check(c.switchModel(QStringLiteral("qwen")), "defauts: switch sur l'alias natif ok");
        check(!c.switchModel(QStringLiteral("zzz"), &msg), "defauts: switch inexistant échoue");
    }

    std::printf("\n=== TOTAL: %d failure(s) ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}