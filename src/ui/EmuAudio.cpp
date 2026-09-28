// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/EmuAudio.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QBuffer>
#include <QIODevice>

namespace pist {

namespace {

/// A push-side ring the sink pulls from. Bounds latency by construction: new
/// audio past the cap overwrites the unplayed tail (a gap over a growing
/// delay — the fork's clock and the sound card's will never agree exactly).
class AudioRing : public QIODevice
{
public:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const qint64 n = qMin(maxSize, qint64(m_buf.size()));
        if (n > 0) {
            memcpy(data, m_buf.constData(), size_t(n));
            m_buf.remove(0, int(n));
        }
        return n;
    }
    qint64 writeData(const char *data, qint64 maxSize) override
    {
        m_buf.append(data, int(maxSize));
        // ~200 ms of 44.1 kHz s16 stereo: 176400 bytes per second.
        constexpr int kCap = 176400 / 5;
        if (m_buf.size() > kCap)
            m_buf.remove(0, m_buf.size() - kCap);
        return maxSize;
    }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return m_buf.size(); }

private:
    QByteArray m_buf;
};

} // namespace

EmuAudio::EmuAudio(QObject *parent)
    : QObject(parent)
{
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
        m_sink = new QAudioSink(format, this);
        auto *ring = new AudioRing;
        ring->open(QIODevice::ReadWrite);
        m_device = ring;
        m_sink->start(ring);
        m_rate = rate;
    }
    m_device->write(samples);
}

void EmuAudio::reset()
{
    if (m_sink) {
        m_sink->stop();
        m_sink->deleteLater();
        m_sink = nullptr;
        m_device = nullptr;
    }
    m_rate = 0;
}

} // namespace pist
