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
#if !defined(Q_OS_MACOS)
    Q_UNUSED(interleaved);
    Q_UNUSED(frames);
#else
    if (!interleaved || frames <= 0)
        return;
    std::lock_guard<std::mutex> lock(m_impl->mu);
    m_impl->ensureLocked();
    if (!m_impl->open)
        return;
    m_impl->pending.insert(m_impl->pending.end(), interleaved, interleaved + frames * 2);
    m_impl->trimLocked();
    m_impl->flushLocked();
#endif
}

void HostAudio::flush()
{
#if defined(Q_OS_MACOS)
    std::lock_guard<std::mutex> lock(m_impl->mu);
    m_impl->flushLocked();
#endif
}

bool HostAudio::needsPacing() const
{
#if !defined(Q_OS_MACOS)
    return false;
#else
    std::lock_guard<std::mutex> lock(m_impl->mu);
    return m_impl->queuedFramesLocked() > Impl::kPaceFrames;
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
#endif
}
