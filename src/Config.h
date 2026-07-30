#pragma once

#include <QString>
#include <QList>
#include <QJsonObject>

/**
 * Configuration A-ICE.
 *
 * Lit un fichier JSON décrivant un ou plusieurs providers OpenAI-compatibles,
 * chacun avec un ou plusieurs modèles. Emplacement par défaut :
 * ~/.config/a-ice/config.json. Override via le flag --config <path>.
 *
 * Schéma multi-providers :
 * {
 *   "providers": {
 *     "local": {
 *       "type": "openai_compatible",
 *       "api_url": "http://localhost:18081/v1",
 *       "prompt_format": "qwen",
 *       "models": {
 *         "qwen": {
 *           "name": "qwen36-28b-reap",
 *           "temperature": 0.7,
 *           "max_tokens": 65536,
 *           "stream": true
 *         },
 *         "glm": {
 *           "name": "glm-5.2",
 *           "temperature": 0.7,
 *           "max_tokens": 65536,
 *           "stream": true
 *         }
 *       }
 *     }
 *   },
 *   "default_provider": "local",
 *   "default_model": "qwen",
 *   "tools": { ... }
 * }
 *
 * - La clé d'un modèle dans "models" est son **alias** (raccourci pour
 *   /model). Le champ "name" est le nom réel envoyé à l'API.
 * - La commande /model <alias|name> bascule à la volée : recherche par alias
 *   en priorité, puis fallback sur le nom complet.
 *
 * Schéma legacy (rétro-compatible, single provider) :
 * {
 *   "provider": { "type", "api_url", "prompt_format" },
 *   "model":    { "name", "temperature", "max_tokens", "stream" }
 * }
 * -> reconstruit un provider "default" avec un seul modèle.
 *
 * Si le fichier est absent ou invalide, on retombe sur les valeurs par défaut
 * (modèle local qwen36-28b-reap sur http://localhost:18081/v1).
 *
 * SOUL.md : un fichier de prompt système décrivant la personnalité de l'agent.
 * Emplacement par défaut : ~/.config/a-ice/SOUL.md (à côté de config.json).
 * Fallback : share/a-ice/SOUL.md (installé à côté de config.example.json).
 * Le contenu est injecté en tête des messages comme message role="system".
 */
class Config
{
public:
    struct Model {
        QString alias;       // ex: "glm" (clé dans "models"), == name si non distinct
        QString name;        // ex: "glm-5.2" — nom réel envoyé à l'API
        double temperature = 0.7;
        int maxTokens = 64;
        bool stream = false;
    };

    struct Provider {
        QString id;          // ex: "local" (clé dans "providers")
        QString type;        // ex: "openai_compatible" — nature du backend
        QString apiUrl;      // ex: "http://localhost:18081/v1"
        QString promptFormat; // ex: "qwen"
        QString apiKey;      // ex: token Ollama Cloud — injecté en Authorization: Bearer
        QList<Model> models;  // ordre du JSON préservé
    };

    struct Tools {
        bool enabled = false;
        QString braveApiKey;   // clé API Brave Search
        QString terminalWorkdir; // home par défaut
        int maxToolIterations = 8; // limite anti-boucle de tool calls par message
    };

    Config();

    /// Chemin du fichier de config effectivement utilisé.
    QString configPath() const { return m_configPath; }

    /// URL de base du provider courant (api_url normalisée avec trailing slash).
    QString apiUrl() const { return m_provider.apiUrl; }

    /// Provider courant (vue synchronisée sur la sélection active).
    const Provider &provider() const { return m_provider; }
    /// Modèle courant (vue synchronisée sur la sélection active).
    const Model &model() const { return m_model; }
    const Tools &tools() const { return m_tools; }

    /// Liste de tous les providers déclarés (ordre du JSON).
    const QList<Provider> &providers() const { return m_providers; }
    /// Id du provider courant (clé dans "providers").
    QString currentProviderId() const { return m_currentProviderId; }
    /// Alias du modèle courant (clé dans "models" du provider courant).
    QString currentModelAlias() const { return m_currentModelAlias; }

    /// Tente de basculer vers le modèle identifié par `aliasOrName`.
    /// Recherche en priorité par alias (dans le provider courant d'abord,
    /// puis les autres), en fallback par nom complet.
    /// Met à jour le provider/modèle courants. Retourne false si introuvable
    /// (auquel cas `message` décrit l'erreur et la liste des alias dispo).
    bool switchModel(const QString &aliasOrName, QString *message = nullptr);

    /// Prompt système lu depuis SOUL.md (vide si absent).
    QString systemPrompt() const { return m_systemPrompt; }
    /// Chemin du SOUL.md effectivement utilisé (vide si aucun trouvé).
    QString soulPath() const { return m_soulPath; }

    /// Charge la config depuis `path`. Si `path` est vide, utilise l'emplacement
    /// par défaut (~/.config/a-ice/config.json). Retourne false si le fichier
    /// est absent ou invalide (auquel cas les valeurs par défaut sont utilisées).
    bool load(const QString &path = QString());

    /// Produit les valeurs par défaut (modèle local qwen36-28b-reap).
    static Config defaultLocal();

private:
    void applyDefaults();
    static QString defaultConfigPath();
    /// Tente de charger SOUL.md : d'abord à côté de config.json,
    /// sinon dans les dossiers share/a-ice installés. Retourne le chemin
    /// utilisé (vide si rien trouvé).
    QString loadSoul(const QString &configDir);

    /// Re-synchronise m_provider/m_model depuis m_currentProviderId +
    /// m_currentModelAlias. En cas d'incohérence, retombe sur le 1er modèle
    /// du 1er provider (ou les defaults si vide).
    void syncCurrent();

    QString m_configPath;
    QString m_soulPath;
    QString m_systemPrompt;

    QList<Provider> m_providers;
    QString m_currentProviderId;
    QString m_currentModelAlias;

    Provider m_provider;  // vue courante
    Model m_model;        // vue courante
    Tools m_tools;
};