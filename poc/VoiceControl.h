// a-ice 2.0 — M3e : la voix streaming de la v1, a portee de la barre de prompt.
//
// VoiceStream (src/voice/) est repris TEL QUEL : chunker adaptatif, filtre
// « ce qui peut etre dit », verrou anti-avalanche, coupe immediate. Cet
// adaptateur ne fait que le pont avec le corps :
//   - l'etat vit dans la meme cle que la v1 (a-ice.conf, [voice] enabled),
//   - le diagnostic ne redessine pas la carte a chaque chunk (un seul regard
//     par 700 ms),
//   - AICE_VOICE=0|1 desactive ou impose le son pour la seance, sans ecrire
//     dans la config (comme AICE_SHADOW=0 mesure sans ombre).

#pragma once

#include <QObject>
#include <QString>

class QTimer;
class VoiceStream;

class VoiceControl : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool on READ isOn NOTIFY statusChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)

public:
    explicit VoiceControl(VoiceStream *stream, QObject *parent = nullptr);

    bool isOn() const;
    // Le diagnostic de VoiceStream, ampute de son mot de depart : « voice on •
    // buffer 12 car • avance ≈ 1,3 s » devient « on • buffer 12 car • … », la
    // carte posant son propre prefixe.
    QString status() const;

    // /voice on • /voice off • /voice (bascule) — les trois formes de la v1.
    Q_INVOKABLE void turnOn();
    Q_INVOKABLE void turnOff();
    Q_INVOKABLE void toggle();
    // Changement de conversation : on oublie le buffer local sans couper le son
    // en cours (la coupe, c'est /voice off ou le bouton Stop).
    Q_INVOKABLE void resetBuffer();

signals:
    void statusChanged();

private:
    void ecrire(bool on);
    void annoncer();

    VoiceStream *stream = nullptr;
    QTimer *rafraichi = nullptr;
    QString derniereLigne;
};
