// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <cstdint>

/// Plays the stereo frames `pist_hatari_audio` copies out. On macOS that is
/// an AudioQueue at 44100 Hz. Everywhere else `write` discards them: the
/// pull still has to happen, or Hatari's mix ring wraps, but the Linux
/// tests must not open a device or block on playback.
class HostAudio
{
public:
    HostAudio();
    ~HostAudio();

    HostAudio(const HostAudio &) = delete;
    HostAudio &operator=(const HostAudio &) = delete;

    /// Interleaved signed 16-bit host-endian frames, left then right.
    void write(const int16_t *interleaved, int frames);
    /// Enqueue any full buffer the last write left waiting.
    void flush();
    /// True when more than a short stretch is already queued. The owner
    /// thread waits on this so playback stays at 44100 Hz.
    bool needsPacing() const;
    void close();

private:
    struct Impl;
    Impl *m_impl;
};
