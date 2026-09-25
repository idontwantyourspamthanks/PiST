// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/HostAudio.h"

#include <QDebug>

#include <cstring>
#include <mutex>
#include <vector>

#if defined(Q_OS_MACOS)
#include <AudioToolbox/AudioQueue.h>
#elif defined(PIST_HOST_AUDIO_ALSA)
#include <alsa/asoundlib.h>
#elif defined(PIST_HOST_AUDIO_WINMM)
#include <windows.h>
#include <mmsystem.h>
#endif

struct HostAudio::Impl
{
#if defined(Q_OS_MACOS)
    static constexpr int kRate = 44100;
    static constexpr int kFrames = 2048;
    static constexpr int kBuffers = 3;
    static constexpr int kPaceFrames = (kRate * 80) / 1000;
    static constexpr int kMaxPendingFrames = (kRate * 200) / 1000;

    struct Slot {
        AudioQueueBufferRef ref = nullptr;
        bool busy = false;
    };

    AudioQueueRef queue = nullptr;
    Slot buffers[kBuffers];
    std::vector<int16_t> pending;
    size_t read = 0;
    bool open = false;
    bool started = false;
    bool warned = false;
    mutable std::mutex mu;

    static void callback(void *user, AudioQueueRef, AudioQueueBufferRef buf)
    {
        auto *self = static_cast<Impl *>(user);
        std::lock_guard<std::mutex> lock(self->mu);
        if (!self->open)
            return;
        for (auto &slot : self->buffers) {
            if (slot.ref == buf)
                slot.busy = false;
        }
    }

    int queuedFramesLocked() const
    {
        int frames = int((pending.size() - read) / 2);
        for (const auto &slot : buffers) {
            if (slot.busy)
                frames += kFrames;
        }
        return frames;
    }

    void ensureLocked()
    {
        if (open || warned)
            return;
        AudioStreamBasicDescription format{};
        format.mSampleRate = kRate;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
        format.mBytesPerPacket = 4;
        format.mFramesPerPacket = 1;
        format.mBytesPerFrame = 4;
        format.mChannelsPerFrame = 2;
        format.mBitsPerChannel = 16;
        if (AudioQueueNewOutput(&format, callback, this, nullptr, nullptr, 0, &queue) != noErr) {
            qWarning("PiST: AudioQueue output did not open");
            warned = true;
            return;
        }
        for (auto &slot : buffers) {
            if (AudioQueueAllocateBuffer(queue, kFrames * 4, &slot.ref) != noErr) {
                qWarning("PiST: AudioQueue output did not open");
                AudioQueueDispose(queue, true);
                queue = nullptr;
                warned = true;
                return;
            }
        }
        open = true;
    }

    void trimLocked()
    {
        size_t frames = (pending.size() - read) / 2;
        if (frames <= size_t(kMaxPendingFrames))
            return;
        read += (frames - size_t(kMaxPendingFrames)) * 2;
    }

    void compactLocked()
    {
        if (read == 0)
            return;
        if (read == pending.size()) {
            pending.clear();
            read = 0;
            return;
        }
        if (read > 8192) {
            pending.erase(pending.begin(), pending.begin() + std::ptrdiff_t(read));
            read = 0;
        }
    }

    void flushLocked()
    {
        if (!open)
            return;
        while ((pending.size() - read) / 2 >= size_t(kFrames)) {
            Slot *slot = nullptr;
            for (auto &candidate : buffers) {
                if (!candidate.busy) {
                    slot = &candidate;
                    break;
                }
            }
            if (!slot || !slot->ref)
                return;
            auto *dst = static_cast<int16_t *>(slot->ref->mAudioData);
            std::memcpy(dst, pending.data() + read, size_t(kFrames) * 2 * sizeof(int16_t));
            slot->ref->mAudioDataByteSize = kFrames * 4;
            if (AudioQueueEnqueueBuffer(queue, slot->ref, 0, nullptr) != noErr)
                return;
            read += size_t(kFrames) * 2;
            slot->busy = true;
            if (!started) {
                if (AudioQueueStart(queue, nullptr) != noErr)
                    return;
                started = true;
            }
        }
        compactLocked();
    }
#elif defined(PIST_HOST_AUDIO_ALSA)
    static constexpr int kRate = 44100;
    static constexpr int kPaceFrames = (kRate * 80) / 1000;
    static constexpr int kMaxPendingFrames = (kRate * 200) / 1000;

    // snd_pcm_delay takes a non-const handle. needsPacing is const.
    mutable snd_pcm_t *pcm = nullptr;
    std::vector<int16_t> pending;
    size_t read = 0;
    bool open = false;
    bool warned = false;
    mutable std::mutex mu;

    int queuedFramesLocked() const
    {
        int frames = int((pending.size() - read) / 2);
        if (!pcm)
            return frames;
        snd_pcm_sframes_t delay = 0;
        if (snd_pcm_delay(pcm, &delay) == 0 && delay > 0)
            frames += int(delay);
        return frames;
    }

    void ensureLocked()
    {
        if (open || warned)
            return;
        if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
            qWarning("PiST: ALSA output did not open");
            pcm = nullptr;
            warned = true;
            return;
        }
        snd_pcm_hw_params_t *hw;
        snd_pcm_hw_params_alloca(&hw);
        snd_pcm_hw_params_any(pcm, hw);
        snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
        snd_pcm_hw_params_set_channels(pcm, hw, 2);
        unsigned int rate = kRate;
        snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, nullptr);
        snd_pcm_uframes_t period = 2048;
        snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr);
        snd_pcm_uframes_t buffer = period * 3;
        snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
        if (snd_pcm_hw_params(pcm, hw) < 0 || snd_pcm_prepare(pcm) < 0) {
            qWarning("PiST: ALSA output did not open");
            snd_pcm_close(pcm);
            pcm = nullptr;
            warned = true;
            return;
        }
        open = true;
    }

    void trimLocked()
    {
        size_t frames = (pending.size() - read) / 2;
        if (frames <= size_t(kMaxPendingFrames))
            return;
        read += (frames - size_t(kMaxPendingFrames)) * 2;
    }

    void compactLocked()
    {
        if (read == 0)
            return;
        if (read == pending.size()) {
            pending.clear();
            read = 0;
            return;
        }
        if (read > 8192) {
            pending.erase(pending.begin(), pending.begin() + std::ptrdiff_t(read));
            read = 0;
        }
    }

    void flushLocked()
    {
        if (!open)
            return;
        for (;;) {
            const size_t have = (pending.size() - read) / 2;
            if (have == 0)
                break;
            const snd_pcm_sframes_t avail = snd_pcm_avail(pcm);
            if (avail < 0) {
                if (avail == -EPIPE)
                    snd_pcm_prepare(pcm);
                return;
            }
            if (avail == 0)
                return;
            snd_pcm_uframes_t chunk = have;
            if (chunk > snd_pcm_uframes_t(avail))
                chunk = snd_pcm_uframes_t(avail);
            const snd_pcm_sframes_t wrote = snd_pcm_writei(pcm, pending.data() + read, chunk);
            if (wrote == -EPIPE) {
                snd_pcm_prepare(pcm);
                return;
            }
            if (wrote <= 0)
                return;
            read += size_t(wrote) * 2;
        }
        compactLocked();
    }
#elif defined(PIST_HOST_AUDIO_WINMM)
    static constexpr int kRate = 44100;
    static constexpr int kFrames = 2048;
    static constexpr int kBuffers = 3;
    static constexpr int kPaceFrames = (kRate * 80) / 1000;
    static constexpr int kMaxPendingFrames = (kRate * 200) / 1000;

    struct WaveSlot {
        WAVEHDR hdr{};
        std::vector<int16_t> data;
        bool busy = false;
    };

    HWAVEOUT wave = nullptr;
    WaveSlot buffers[kBuffers];
    std::vector<int16_t> pending;
    size_t read = 0;
    bool open = false;
    bool warned = false;
    mutable std::mutex mu;

    void reclaimLocked()
    {
        for (auto &slot : buffers) {
            if (slot.busy && (slot.hdr.dwFlags & WHDR_DONE))
                slot.busy = false;
        }
    }

    int queuedFramesLocked() const
    {
        int frames = int((pending.size() - read) / 2);
        for (const auto &slot : buffers) {
            if (slot.busy)
                frames += kFrames;
        }
        return frames;
    }

    void ensureLocked()
    {
        if (open || warned)
            return;
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = kRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 4;
        format.nAvgBytesPerSec = kRate * 4;
        if (waveOutOpen(&wave, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            qWarning("PiST: waveOut output did not open");
            wave = nullptr;
            warned = true;
            return;
        }
        for (auto &slot : buffers) {
            slot.data.resize(size_t(kFrames) * 2);
            slot.hdr.lpData = reinterpret_cast<LPSTR>(slot.data.data());
            slot.hdr.dwBufferLength = kFrames * 4;
            if (waveOutPrepareHeader(wave, &slot.hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                qWarning("PiST: waveOut output did not open");
                waveOutClose(wave);
                wave = nullptr;
                warned = true;
                return;
            }
        }
        open = true;
    }

    void trimLocked()
    {
        size_t frames = (pending.size() - read) / 2;
        if (frames <= size_t(kMaxPendingFrames))
            return;
        read += (frames - size_t(kMaxPendingFrames)) * 2;
    }

    void compactLocked()
    {
        if (read == 0)
            return;
        if (read == pending.size()) {
            pending.clear();
            read = 0;
            return;
        }
        if (read > 8192) {
            pending.erase(pending.begin(), pending.begin() + std::ptrdiff_t(read));
            read = 0;
        }
    }

    void flushLocked()
    {
        if (!open)
            return;
        reclaimLocked();
        while ((pending.size() - read) / 2 >= size_t(kFrames)) {
            WaveSlot *slot = nullptr;
            for (auto &candidate : buffers) {
                if (!candidate.busy) {
                    slot = &candidate;
                    break;
                }
            }
            if (!slot)
                return;
            std::memcpy(slot->data.data(), pending.data() + read, size_t(kFrames) * 2 * sizeof(int16_t));
            slot->hdr.dwFlags &= ~WHDR_DONE;
            slot->hdr.dwBufferLength = kFrames * 4;
            if (waveOutWrite(wave, &slot->hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
                return;
            read += size_t(kFrames) * 2;
            slot->busy = true;
        }
        compactLocked();
    }
#endif
};

HostAudio::HostAudio()
    : m_impl(new Impl)
{
}

HostAudio::~HostAudio()
{
    close();
    delete m_impl;
}

void HostAudio::write(const int16_t *interleaved, int frames)
{
#if defined(Q_OS_MACOS) || defined(PIST_HOST_AUDIO_ALSA) || defined(PIST_HOST_AUDIO_WINMM)
    if (!interleaved || frames <= 0)
        return;
    std::lock_guard<std::mutex> lock(m_impl->mu);
    m_impl->ensureLocked();
    if (!m_impl->open)
        return;
    m_impl->pending.insert(m_impl->pending.end(), interleaved, interleaved + frames * 2);
    m_impl->trimLocked();
    m_impl->flushLocked();
#else
    Q_UNUSED(interleaved);
    Q_UNUSED(frames);
#endif
}

void HostAudio::flush()
{
#if defined(Q_OS_MACOS) || defined(PIST_HOST_AUDIO_ALSA) || defined(PIST_HOST_AUDIO_WINMM)
    std::lock_guard<std::mutex> lock(m_impl->mu);
    m_impl->flushLocked();
#endif
}

bool HostAudio::needsPacing() const
{
#if defined(Q_OS_MACOS) || defined(PIST_HOST_AUDIO_ALSA) || defined(PIST_HOST_AUDIO_WINMM)
    std::lock_guard<std::mutex> lock(m_impl->mu);
    return m_impl->open && m_impl->queuedFramesLocked() > Impl::kPaceFrames;
#else
    return false;
#endif
}

void HostAudio::close()
{
#if defined(Q_OS_MACOS)
    AudioQueueRef queue = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        m_impl->open = false;
        m_impl->started = false;
        queue = m_impl->queue;
        m_impl->queue = nullptr;
        for (auto &slot : m_impl->buffers) {
            slot.ref = nullptr;
            slot.busy = false;
        }
        m_impl->pending.clear();
        m_impl->read = 0;
        m_impl->warned = false;
    }
    if (queue) {
        AudioQueueStop(queue, true);
        AudioQueueDispose(queue, true);
    }
#elif defined(PIST_HOST_AUDIO_ALSA)
    snd_pcm_t *pcm = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        m_impl->open = false;
        pcm = m_impl->pcm;
        m_impl->pcm = nullptr;
        m_impl->pending.clear();
        m_impl->read = 0;
        m_impl->warned = false;
    }
    if (pcm) {
        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
    }
#elif defined(PIST_HOST_AUDIO_WINMM)
    HWAVEOUT wave = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        m_impl->open = false;
        wave = m_impl->wave;
        m_impl->wave = nullptr;
        m_impl->pending.clear();
        m_impl->read = 0;
        m_impl->warned = false;
    }
    if (wave) {
        waveOutReset(wave);
        for (auto &slot : m_impl->buffers)
            waveOutUnprepareHeader(wave, &slot.hdr, sizeof(WAVEHDR));
        waveOutClose(wave);
    }
#endif
}
