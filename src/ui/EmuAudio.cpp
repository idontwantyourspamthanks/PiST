// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/EmuAudio.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QDateTime>
#include <QIODevice>
#include <QMediaDevices>
#include <QTimer>

namespace pist {

namespace {

/// ~200 ms of 44.1 kHz s16le stereo (176400 bytes/s). New audio past the cap
/// overwrites the unplayed tail: a gap over a growing delay, since the fork's
/// clock and the sound card's will never agree exactly.
constexpr int kQueueCap = 176400 / 5;

/// How long to wait for more sink space before pumping again.
constexpr int kPumpIntervalMs = 10;

/// Audio held before the sink is started. Chunks arrive once per guest VBL
/// (~20 ms); a sink started empty underruns on its first period and the
/// device output breaks up — a single beep arrives as a train of blips
/// (measured against a real ST, which plays the same stream as one tone).
constexpr int kPrebufferBytes = 176400 / 10; // 100 ms
/// If the stream ends before the prebuffer fills (a click, a very short
/// effect), don't wait forever: play what there is.
constexpr qint64 kPrebufferMaxWaitMs = 250;
/// The sink's own buffer: room for the arrival jitter plus the prebuffer, so
/// a late chunk never drains the device.
constexpr int kSinkBufferBytes = 176400 / 4; // 250 ms

} // namespace

EmuAudio::EmuAudio(QObject *parent)
    : QObject(parent)
{
    // A default-constructed QAudioDevice is a *null* device in Qt6, not
    // "the system default" — a sink opened with it opens no stream at all
    // (KDE's playback-streams list never shows the app). The real default
    // must be named explicitly.
    m_deviceInfo = QMediaDevices::defaultAudioOutput();
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
        m_sink->setBufferSize(kSinkBufferBytes);
        connect(m_sink, &QAudioSink::stateChanged, this,
                [this](QAudio::State state) {
                    if (state == QAudio::IdleState)
                        pump();
                });
        m_rate = rate;
    }

    if (m_pending.isEmpty())
        m_pendingSinceMs = QDateTime::currentMSecsSinceEpoch();
    m_pending.append(samples);
    if (m_pending.size() > kQueueCap)
        m_pending.remove(0, m_pending.size() - kQueueCap);
    pump();
}

void EmuAudio::pump()
{
    if (!m_sink)
        return;

    // Hold playback until enough audio is buffered to ride out the jitter of
    // per-VBL arrival; starting an empty sink underruns on its first period.
    // A stream too short to fill the prebuffer starts on a deadline instead,
    // so a single short effect still plays.
    if (!m_io && !m_starting && !m_pending.isEmpty()
        && (m_pending.size() >= kPrebufferBytes
            || QDateTime::currentMSecsSinceEpoch() - m_pendingSinceMs
                   >= kPrebufferMaxWaitMs)) {
        // start() can report Idle synchronously, which re-enters pump()
        // through stateChanged() before m_io is assigned; without this guard
        // the sink is started twice.
        m_starting = true;
        m_io = m_sink->start();
        m_starting = false;
    }

    if (m_io) {
        while (!m_pending.isEmpty() && m_sink->bytesFree() > 0) {
            const qint64 n = qMin(qint64(m_pending.size()), m_sink->bytesFree());
            m_io->write(m_pending.constData(), n);
            m_pending.remove(0, int(n));
        }
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
    m_pendingSinceMs = 0;
    m_starting = false;
    if (m_sink) {
        m_sink->stop();
        m_sink->deleteLater();
        m_sink = nullptr;
        m_io = nullptr;
    }
    m_rate = 0;
}

} // namespace pist
