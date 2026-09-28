// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QObject>

class QAudioSink;
class QIODevice;

namespace pist {

/// The media channel's audio output (docs/PLAN.md §12, phase 3): plays the
/// guest's mixed sound, streamed as per-VBL chunks of s16le stereo, through
/// a QAudioSink. A small jitter buffer smooths chunk arrival; latency is
/// capped by dropping the oldest audio on overflow, because live sound at a
/// slight gap beats a growing delay.
class EmuAudio : public QObject
{
    Q_OBJECT

public:
    explicit EmuAudio(QObject *parent = nullptr);
    ~EmuAudio() override;

    /// One chunk from the fork. The sink is (re)created if the rate changes.
    void writeChunk(quint32 rate, const QByteArray &samples);

    /// Session end: stop playback and forget the buffered audio.
    void reset();

private:
    QAudioSink *m_sink = nullptr;
    QIODevice *m_device = nullptr; // owned by m_sink while started
    quint32 m_rate = 0;
};

} // namespace pist
