#pragma once
// ClaudeTune editor - single-producer / single-consumer FIFO that carries
// captured audio (with its timeline position) from the audio thread to the
// background worker. Lock-free and allocation-free on the producer side.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace claudetune::edit
{

class CaptureFifo
{
public:
    struct Header
    {
        int64_t timeline = 0; // timeline sample of the first frame
        int numFrames = 0;
    };

    // Not thread-safe: call while neither side is running.
    void allocate (int channels, int capacityFrames, int maxHeaders)
    {
        numChannels = std::max (1, channels);
        frameCapacity = std::max (16, capacityFrames);
        headerCapacity = std::max (16, maxHeaders);
        samples.assign ((size_t) frameCapacity * (size_t) numChannels, 0.0f);
        headers.assign ((size_t) headerCapacity, {});
        frameWrite.store (0);
        frameRead.store (0);
        headerWrite.store (0);
        headerRead.store (0);
    }

    int channels() const noexcept { return numChannels; }
    bool isAllocated() const noexcept { return ! samples.empty(); }

    // Producer. Copies numFrames frames starting at offset from each channel
    // pointer (channels beyond numChannelsIn repeat the last one).
    bool push (int64_t timeline, const float* const* in, int numChannelsIn, int offset, int numFrames) noexcept
    {
        if (samples.empty() || numFrames <= 0 || numChannelsIn <= 0)
            return numFrames <= 0;

        const uint64_t fw = frameWrite.load (std::memory_order_relaxed);
        const uint64_t fr = frameRead.load (std::memory_order_acquire);
        const uint64_t hw = headerWrite.load (std::memory_order_relaxed);
        const uint64_t hr = headerRead.load (std::memory_order_acquire);
        if (fw - fr + (uint64_t) numFrames > (uint64_t) frameCapacity || hw - hr >= (uint64_t) headerCapacity)
            return false;

        for (int i = 0; i < numFrames; ++i)
        {
            const size_t slot = (size_t) ((fw + (uint64_t) i) % (uint64_t) frameCapacity) * (size_t) numChannels;
            for (int c = 0; c < numChannels; ++c)
                samples[slot + (size_t) c] = in[std::min (c, numChannelsIn - 1)][offset + i];
        }
        headers[(size_t) (hw % (uint64_t) headerCapacity)] = { timeline, numFrames };
        frameWrite.store (fw + (uint64_t) numFrames, std::memory_order_release);
        headerWrite.store (hw + 1, std::memory_order_release);
        return true;
    }

    // Consumer: pops one record into per-channel vectors (appended).
    bool pop (Header& h, std::vector<std::vector<float>>& out)
    {
        const uint64_t hr = headerRead.load (std::memory_order_relaxed);
        if (hr == headerWrite.load (std::memory_order_acquire))
            return false;
        h = headers[(size_t) (hr % (uint64_t) headerCapacity)];
        const uint64_t fr = frameRead.load (std::memory_order_relaxed);

        if (out.size() != (size_t) numChannels)
            out.resize ((size_t) numChannels);
        for (int c = 0; c < numChannels; ++c)
        {
            auto& ch = out[(size_t) c];
            const size_t base = ch.size();
            ch.resize (base + (size_t) h.numFrames);
            for (int i = 0; i < h.numFrames; ++i)
                ch[base + (size_t) i] = samples[(size_t) ((fr + (uint64_t) i) % (uint64_t) frameCapacity) * (size_t) numChannels + (size_t) c];
        }
        frameRead.store (fr + (uint64_t) h.numFrames, std::memory_order_release);
        headerRead.store (hr + 1, std::memory_order_release);
        return true;
    }

    bool isEmpty() const noexcept
    {
        return headerRead.load (std::memory_order_acquire) == headerWrite.load (std::memory_order_acquire);
    }

private:
    int numChannels = 1, frameCapacity = 0, headerCapacity = 0;
    std::vector<float> samples;
    std::vector<Header> headers;
    std::atomic<uint64_t> frameWrite { 0 }, frameRead { 0 }, headerWrite { 0 }, headerRead { 0 };
};

} // namespace claudetune::edit
