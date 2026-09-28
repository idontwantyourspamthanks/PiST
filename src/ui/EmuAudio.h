// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QAudioDevice>
#include <QObject>

class QAudioSink;
class QIODevice;
class QTimer;

namespace pist {

/// The QSettings key the audio output device persists under (an
/// application-wide preference, like the appearance keys — MIN-53's rule:
/// the key lives with its reader). Empty value = system default.
inline constexpr const char *kAudioOutputDeviceKey = "audio/outputDeviceId";

/// The media channel's audio output (docs/PLAN.md §12, phase 3): plays the
/// guest's mixed sound, streamed as per-VBL chunks of s16le stereo, through
/// a QAudioSink in push mode. A small queue smooths chunk arrival; latency is
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

    /// The output device to play through (default-constructed = the system
    /// default). Takes effect on the next chunk: the sink is rebuilt.
    void setOutputDevice(const QAudioDevice &device);

    /// Session end: stop playback and forget the buffered audio.
    void reset();

private:
    void pump();

    QAudioSink *m_sink = nullptr;
    QIODevice *m_io = nullptr; // the sink's push device, owned by the sink
    QAudioDevice m_deviceInfo; // default-constructed: system default
    QTimer *m_pumpTimer = nullptr;
    QByteArray m_pending;
    quint32 m_rate = 0;
};

} // namespace pist
