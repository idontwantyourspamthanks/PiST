// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/DebugBackend.h"

#include <QTimer>

namespace pist {

IDebugBackend::IDebugBackend(QObject *parent)
    : QObject(parent)
{
    // The media channel (docs/PLAN.md §12) belongs to every backend: the
    // hatari-pist fork speaks --pist-media no matter which debug transport
    // the session runs on. Frames, audio and log lines join ours.
    connect(&m_mediaServer, &MediaServer::frameReceived, this,
            &IDebugBackend::mediaFrameReceived);
    connect(&m_mediaServer, &MediaServer::logLine, this,
            [this](const QString &line) { emit logLine(line); });
    connect(&m_mediaServer, &MediaServer::audioReceived, this,
            &IDebugBackend::mediaAudioReceived);

    // A media session that never completes AUTH is a silent black panel with
    // a running headless emulator behind it. Say so, once, rather than
    // waiting forever. clientConnected is the off switch.
    m_mediaAuthWatchdog = new QTimer(this);
    m_mediaAuthWatchdog->setSingleShot(true);
    m_mediaAuthWatchdog->setInterval(10000);
    connect(m_mediaAuthWatchdog, &QTimer::timeout, this, [this] {
        emit logLine(tr("media: no authenticated client within 10 s — the "
                        "emulator is running windowless but never completed "
                        "the media handshake. Check that $PIST_HATARI (or the "
                        "configured emulator) is a current hatari-pist build."));
    });
    connect(&m_mediaServer, &MediaServer::clientConnected, this,
            [this] { m_mediaAuthWatchdog->stop(); });
}

int IDebugBackend::mediaListen()
{
    QString error;
    if (!m_mediaServer.listen(&error)) {
        // Not fatal for the session — the caller falls back to a session
        // without the media channel when it gets 0 back.
        emit logLine(tr("Media channel unavailable: %1").arg(error));
        return 0;
    }
    return m_mediaServer.port();
}

void IDebugBackend::mediaKey(quint8 scancode, bool down)
{
    m_mediaServer.sendKey(scancode, down);
}

void IDebugBackend::mediaMouse(qint16 dx, qint16 dy, quint8 buttons)
{
    m_mediaServer.sendMouse(dx, dy, buttons);
}

} // namespace pist
