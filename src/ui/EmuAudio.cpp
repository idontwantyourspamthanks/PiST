// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/EmuAudio.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QTimer>

namespace pist {

namespace {

/// ~200 ms of 44.1 kHz s16le stereo (176400 bytes/s). New audio past the cap
/// overwrites the unplayed tail: a gap over a growing delay, since the fork's
/// clock and the sound card's will never agree exactly.
constexpr int kQueueCap = 176400 / 5;

/// How long to wait for more sink space before pumping again.
constexpr int kPumpIntervalMs = 10;

} // namespace

EmuAudio::EmuAudio(QObject *parent)
    : QObject(parent)
{
    m_pumpTimer = new QTimer(this);
    m_pumpTimer->setInterval(kPumpIntervalMs);
    connect(m_pumpTimer, &QTimer::timeout, this, &EmuAudio::pump);
}

EmuAudio::~EmuAudio()
{
    reset();
}

void EmuAudio::writeChunk(quint32 rate, const QByteArray &samples)
{
    if (samples.isEmpty())
        return;

    if (!m_sink || rate != m_rate) {
        // (Re)start at the chunk's rate: the fork declares it per chunk, and
        // PiST configures 44100 in media mode.
        reset();
        QAudioFormat format;
        format.setSampleRate(int(rate));
        format.setChannelCount(2);
        format.setSampleFormat(QAudioFormat::Int16);
        m_sink = new QAudioSink(m_deviceInfo, format, this);
        // Push mode: we write bounded by bytesFree(). A pull-mode sink whose
        // device starves goes Idle and never resumes — the silent-output
        // failure push mode avoids; and when an underrun does idle it, new
        // data brings it back.
        m_io = m_sink->start();
        connect(m_sink, &QAudioSink::stateChanged, this,
                [this](QAudio::State state) {
                    if (state == QAudio::IdleState)
                        pump();
                });
        m_rate = rate;
    }

    m_pending.append(samples);
    if (m_pending.size() > kQueueCap)
        m_pending.remove(0, m_pending.size() - kQueueCap);
    pump();
}

void EmuAudio::pump()
{
    if (!m_sink || !m_io)
        return;
    while (!m_pending.isEmpty() && m_sink->bytesFree() > 0) {
        const qint64 n = qMin(qint64(m_pending.size()), m_sink->bytesFree());
        m_io->write(m_pending.constData(), n);
        m_pending.remove(0, int(n));
    }
    if (!m_pending.isEmpty() && !m_pumpTimer->isActive())
        m_pumpTimer->start();
    else if (m_pending.isEmpty())
        m_pumpTimer->stop();
}

void EmuAudio::setOutputDevice(const QAudioDevice &device)
{
    m_deviceInfo = device;
    reset();
}

void EmuAudio::reset()
{
    m_pumpTimer->stop();
    m_pending.clear();
    if (m_sink) {
        m_sink->stop();
        m_sink->deleteLater();
        m_sink = nullptr;
        m_io = nullptr;
    }
    m_rate = 0;
}

} // namespace pist
