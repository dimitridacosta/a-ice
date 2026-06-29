#pragma once

#include <QString>
#include <QJsonObject>

/**
 * Configuration A-ICE.
 *
 * Lit un fichier JSON contenant les infos du provider et du modèle.
 * Emplacement par défaut : ~/.config/a-ice/config.json
 * Override via le flag --config <path>.
 *
 * Schéma attendu :
 * {
 *   "provider": {
 *     "name": "openai_compatible",
 *     "api_url": "http://localhost:18081/v1",
 *     "prompt_format": "qwen"
 *   },
 *   "model": {
 *     "name": "qwen36-28b-reap",
 *     "temperature": 0.7,
 *     "max_tokens": 64,
 *     "stream": false
 *   }
 * }
 *
 * Si le fichier est absent ou invalide, on retombe sur les valeurs par défaut
 * (modèle local qwen36-28b-reap sur http://localhost:18081/v1).
 */
class Config
{
public:
    struct Provider {
        QString name;        // ex: "openai_compatible"
        QString apiUrl;      // ex: "http://localhost:18081/v1"
        QString promptFormat; // ex: "qwen"
    };

    struct Model {
        QString name;        // ex: "qwen36-28b-reap"
        double temperature = 0.7;
        int maxTokens = 64;
        bool stream = false;
    };

    Config();

    /// Chemin du fichier de config effectivement utilisé.
    QString configPath() const { return m_configPath; }

    /// URL de base du provider (api_url normalisée avec trailing slash).
    QString apiUrl() const { return m_provider.apiUrl; }

    const Provider &provider() const { return m_provider; }
    const Model &model() const { return m_model; }

    /// Charge la config depuis `path`. Si `path` est vide, utilise l'emplacement
    /// par défaut (~/.config/a-ice/config.json). Retourne false si le fichier
    /// est absent ou invalide (auquel cas les valeurs par défaut sont utilisées).
    bool load(const QString &path = QString());

    /// Produit les valeurs par défaut (modèle local qwen36-28b-reap).
    static Config defaultLocal();

private:
    void applyDefaults();
    static QString defaultConfigPath();

    QString m_configPath;
    Provider m_provider;
    Model m_model;
};