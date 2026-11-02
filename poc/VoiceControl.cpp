// a-ice 2.0 — M3e : voir VoiceControl.h.

#include "VoiceControl.h"
#include "../src/voice/VoiceStream.h"

#include <QByteArray>
#include <QSettings>
#include <QTimer>

VoiceControl::VoiceControl(VoiceStream *voiceStream, QObject *parent)
    : QObject(parent), stream(voiceStream)
{
    QSettings settings(QStringLiteral("a-ice"), QStringLiteral("a-ice"));
    bool allume = settings.value(QStringLiteral("voice/enabled"), false).toBool();

    // L echappatoire de mesure : elle ne se persiste pas, elle ecarte seulement
    // la voix de la seance en cours.
    const QByteArray force = qgetenv("AICE_VOICE");
    if (!force.isEmpty())
        allume = force != QByteArray("0");

    stream->setEnabled(allume);
    derniereLigne = status();

    // Le knob de test du module (VoiceStream::setSocketPath) : une voie de dialogue
    // avec un faux serveur, pour verifier le stream sans toucher au vrai ddvoice.
    const QByteArray socket = qgetenv("AICE_VOICE_SOCKET");
    if (!socket.isEmpty())
        stream->setSocketPath(QString::fromLocal8Bit(socket));

    // Le diagnostic ne doit pas faire bouger la carte a chaque chunk emis : un
    // seul regard periodique, tant que le stream parle.
    rafraichi = new QTimer(this);
    rafraichi->setInterval(700);
    rafraichi->setTimerType(Qt::VeryCoarseTimer);
    QObject::connect(rafraichi, &QTimer::timeout, this, &VoiceControl::annoncer);

    QObject::connect(stream, &VoiceStream::chunkSent, this, [this](int chars, double seconds) {
        (void) chars;
        (void) seconds;
        if (!rafraichi->isActive())
            rafraichi->start();
    });
    // Serveur TTS mort en plein stream : le module a abandonne le tour, la carte
    // doit le dire tout de suite (plus de chunk pour rappeler le timer).
    QObject::connect(stream, &VoiceStream::serverLost, this, [this]() {
        rafraichi->stop();
        annoncer();
    });
}

bool VoiceControl::isOn() const
{
    return stream && stream->enabled();
}

QString VoiceControl::status() const
{
    if (!stream)
        return QStringLiteral("off");

    // Le prefixe « voice » du module est redundant avec celui de la carte.
    const QString line = stream->statusLine();
    const QString entete = QStringLiteral("voice ");
    return line.startsWith(entete) ? line.mid(entete.size()) : line;
}

void VoiceControl::turnOn()
{
    stream->setEnabled(true);
    ecrire(true);
    rafraichi->stop();
    annoncer();
}

void VoiceControl::turnOff()
{
    // Coupe immediate : son en cours + file serveur + buffer local (la regle de
    // la v1, pas un simple drapeau).
    stream->stop();
    stream->setEnabled(false);
    ecrire(false);
    rafraichi->stop();
    annoncer();
}

void VoiceControl::toggle()
{
    if (stream->enabled())
        turnOff();
    else
        turnOn();
}

void VoiceControl::resetBuffer()
{
    stream->reset();
    annoncer();
}

// L'etat survit d'une fenetre a l'autre, dans la meme cle que la v1.
void VoiceControl::ecrire(bool on)
{
    QSettings settings(QStringLiteral("a-ice"), QStringLiteral("a-ice"));
    settings.setValue(QStringLiteral("voice/enabled"), on);
}

// Ne previent la carte que si la ligne a reellement change.
void VoiceControl::annoncer()
{
    const QString line = status();
    if (line == derniereLigne)
        return;
    derniereLigne = line;
    emit statusChanged();
}
