#pragma once

#include "../Tool.h"
#include <QString>
#include <QDir>

class TerminalTool : public Tool
{
    Q_OBJECT
public:
    explicit TerminalTool(QObject *parent = nullptr)
        : Tool(parent), m_workdir(QDir::homePath()) {}
    explicit TerminalTool(const QString &workdir, QObject *parent = nullptr)
        : Tool(parent), m_workdir(workdir.isEmpty() ? QDir::homePath() : workdir) {}

    Spec spec() const override;
    void execute(const QJsonObject &args,
                 std::function<void(bool ok, QString result)> cb) override;

    // Mode "circuit ouvert" : ACTIF au demarrage. Toute commande recue est
    // interceptee et renvoie un message sterile — bash n'est jamais lance.
    // Permet de valider en securite que rien ne peut fuiter du terminal avant
    // de faire confiance a l'hardline. Desactivable uniquement depuis l'UI
    // (ChatWidget, Ctrl+Shift+T) — jamais pilotable par le modele.
    static bool isTestMode() { return s_testMode; }
    static void setTestMode(bool on) { s_testMode = on; }

private:
    QString m_workdir;
    static bool s_testMode;
};