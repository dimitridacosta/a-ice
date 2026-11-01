// a-ice 2.0 — M3 : le markdown des reponses, parse HORS de l'UI thread.
//
// QTextDocument::setMarkdown + toHtml coutent assez cher pour bloquer le rendu
// si on les fait vivre dans une bulle : le parseur tourne donc dans le pool
// QtConcurrent (un QTextDocument empile la-dedans, a vie courte), et le resultat
// remonte par un signal requeue. Cote QML, une bulle en streaming reste en
// PlainText (le texte coulent), et une bulle fermee bascule en RichText quand
// son html arrive — c'est parsed() qui porte la bascule.

#pragma once

#include <QObject>
#include <QString>

class Markdown : public QObject
{
    Q_OBJECT

public:
    explicit Markdown(QObject *parent = nullptr);

    /*
     * Parse la bulle fermee de messageId. Le texte a convertir est duplique dans
     * la tache (pas de reference sur un objet vivant dans l'UI), le signal
     * parsed(messageId, html) revient sur le thread de l'UI.
     */
    Q_INVOKABLE void parse(const QString &messageId, const QString &markdown);

signals:
    void parsed(const QString &messageId, const QString &html);
};