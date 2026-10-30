#include "SessionCodec.h"

namespace claudetune
{

namespace
{
    constexpr int kAudioMagic = 0x31535443; // "CTS1"
    constexpr int kEditsMagic = 0x31455443; // "CTE1"
    enum Codec { RawFloatGz = 0, Flac24 = 1 };

    juce::MemoryBlock gzip (const void* data, size_t size)
    {
        juce::MemoryBlock out;
        {
            juce::MemoryOutputStream mo (out, false);
            juce::GZIPCompressorOutputStream gz (mo, 6);
            gz.write (data, size);
        }
        return out;
    }

    bool gunzip (const void* data, size_t size, juce::MemoryBlock& out)
    {
        juce::MemoryInputStream mi (data, size, false);
        juce::GZIPDecompressorInputStream gz (mi);
        out.reset();
        juce::MemoryOutputStream mo (out, false);
        mo.writeFromInputStream (gz, -1);
        return true;
    }

    juce::MemoryBlock encodeFlac (const std::vector<std::vector<float>>& audio, double sr, float scale)
    {
        juce::MemoryBlock block;
        if (audio.empty() || audio[0].empty())
            return block;

        juce::FlacAudioFormat flac;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (block, false);
        auto writer = flac.createWriterFor (stream, juce::AudioFormatWriterOptions {}
                                                        .withSampleRate (sr)
                                                        .withNumChannels ((int) audio.size())
                                                        .withBitsPerSample (24)
                                                        .withQualityOptionIndex (5));
        if (writer == nullptr)
            return {};

        const int n = (int) audio[0].size();
        const int chunk = 65536;
        juce::AudioBuffer<float> buf ((int) audio.size(), chunk);
        for (int pos = 0; pos < n; pos += chunk)
        {
            const int len = std::min (chunk, n - pos);
            for (size_t c = 0; c < audio.size(); ++c)
                for (int i = 0; i < len; ++i)
                    buf.setSample ((int) c, i, audio[c][(size_t) (pos + i)] * scale);
            if (! writer->writeFromFloatArrays (buf.getArrayOfReadPointers(), (int) audio.size(), len))
                return {};
        }
        writer.reset(); // flushes into block
        return block;
    }

    bool decodeFlac (const void* data, size_t size, int channels, int64_t numSamples, float scale,
                     std::vector<std::vector<float>>& audio)
    {
        juce::FlacAudioFormat flac;
        auto reader = std::unique_ptr<juce::AudioFormatReader> (
            flac.createReaderFor (new juce::MemoryInputStream (data, size, false), true));
        if (reader == nullptr || (int64_t) reader->lengthInSamples < numSamples)
            return false;

        audio.assign ((size_t) channels, std::vector<float> ((size_t) numSamples, 0.0f));
        const int chunk = 65536;
        juce::AudioBuffer<float> buf ((int) reader->numChannels, chunk);
        for (int64_t pos = 0; pos < numSamples; pos += chunk)
        {
            const int len = (int) std::min<int64_t> (chunk, numSamples - pos);
            if (! reader->read (&buf, 0, len, pos, true, true))
                return false;
            for (int c = 0; c < channels; ++c)
            {
                const int srcCh = std::min (c, (int) reader->numChannels - 1);
                for (int i = 0; i < len; ++i)
                    audio[(size_t) c][(size_t) (pos + i)] = buf.getSample (srcCh, i) / scale;
            }
        }
        return true;
    }
}

juce::MemoryBlock SessionCodec::encodeAudio (const edit::SessionState& s)
{
    juce::MemoryBlock out;
    juce::MemoryOutputStream mo (out, false);
    mo.writeInt (kAudioMagic);
    mo.writeInt (1);
    mo.writeDouble (s.sampleRate);
    mo.writeInt (s.numChannels);
    mo.writeInt64 (s.length);
    mo.writeInt ((int) s.coverage.size());

    for (size_t k = 0; k < s.coverage.size(); ++k)
    {
        const auto& r = s.coverage[k];
        const auto& audio = s.audio[k];
        float peak = 0.0f;
        for (const auto& ch : audio)
            for (const float v : ch)
                peak = std::max (peak, std::abs (v));
        const float scale = peak > 0.999f ? 0.999f / peak : 1.0f;

        auto flacData = encodeFlac (audio, s.sampleRate, scale);
        mo.writeInt64 (r.first);
        mo.writeInt64 (r.second);
        if (flacData.getSize() > 0)
        {
            mo.writeFloat (scale);
            mo.writeInt (Flac24);
            mo.writeInt64 ((juce::int64) flacData.getSize());
            mo.write (flacData.getData(), flacData.getSize());
        }
        else
        {
            // fall back to raw floats (an unusual sample rate FLAC refuses)
            juce::MemoryBlock raw;
            for (const auto& ch : audio)
                raw.append (ch.data(), ch.size() * sizeof (float));
            auto packed = gzip (raw.getData(), raw.getSize());
            mo.writeFloat (1.0f);
            mo.writeInt (RawFloatGz);
            mo.writeInt64 ((juce::int64) packed.getSize());
            mo.write (packed.getData(), packed.getSize());
        }
    }

    // pitch analysis
    std::vector<float> frames;
    frames.reserve (s.frames.size() * 3);
    for (const auto& f : s.frames)
    {
        frames.push_back (f.midi);
        frames.push_back (f.confidence);
        frames.push_back (f.rmsDb);
    }
    auto packed = gzip (frames.data(), frames.size() * sizeof (float));
    mo.writeInt ((int) s.frames.size());
    mo.writeInt64 ((juce::int64) packed.getSize());
    mo.write (packed.getData(), packed.getSize());
    mo.flush();
    return out;
}

juce::MemoryBlock SessionCodec::encodeEdits (const std::string& edits)
{
    juce::MemoryBlock out;
    juce::MemoryOutputStream mo (out, false);
    mo.writeInt (kEditsMagic);
    mo.writeInt64 ((juce::int64) edits.size());
    mo.write (edits.data(), edits.size());
    mo.flush();
    return out;
}

bool SessionCodec::decode (const void* data, size_t size, edit::SessionState& s)
{
    juce::MemoryInputStream in (data, size, false);
    if (in.readInt() != kAudioMagic || in.readInt() != 1)
        return false;

    s = {};
    s.sampleRate = in.readDouble();
    s.numChannels = in.readInt();
    s.length = in.readInt64();
    const int ranges = in.readInt();
    if (s.sampleRate <= 0.0 || s.numChannels <= 0 || s.numChannels > 16 || ranges < 0 || s.length < 0)
        return false;

    for (int k = 0; k < ranges; ++k)
    {
        const int64_t a = in.readInt64(), b = in.readInt64();
        const float scale = in.readFloat();
        const int codec = in.readInt();
        const auto bytes = (size_t) in.readInt64();
        if (b < a || bytes > size || in.getNumBytesRemaining() < (juce::int64) bytes || scale <= 0.0f)
            return false;

        juce::MemoryBlock blob;
        in.readIntoMemoryBlock (blob, (ssize_t) bytes);
        std::vector<std::vector<float>> audio;
        if (codec == Flac24)
        {
            if (! decodeFlac (blob.getData(), blob.getSize(), s.numChannels, b - a, scale, audio))
                return false;
        }
        else
        {
            juce::MemoryBlock raw;
            gunzip (blob.getData(), blob.getSize(), raw);
            if (raw.getSize() != (size_t) (b - a) * (size_t) s.numChannels * sizeof (float))
                return false;
            audio.assign ((size_t) s.numChannels, std::vector<float> ((size_t) (b - a)));
            auto* f = static_cast<const float*> (raw.getData());
            for (int c = 0; c < s.numChannels; ++c)
                std::copy (f + (size_t) c * (size_t) (b - a), f + (size_t) (c + 1) * (size_t) (b - a), audio[(size_t) c].begin());
        }
        s.coverage.push_back ({ a, b });
        s.audio.push_back (std::move (audio));
    }

    const int numFrames = in.readInt();
    const auto frameBytes = (size_t) in.readInt64();
    if (numFrames < 0 || in.getNumBytesRemaining() < (juce::int64) frameBytes)
        return false;
    juce::MemoryBlock packed, raw;
    in.readIntoMemoryBlock (packed, (ssize_t) frameBytes);
    gunzip (packed.getData(), packed.getSize(), raw);
    if (raw.getSize() != (size_t) numFrames * 3 * sizeof (float))
        return false;
    auto* f = static_cast<const float*> (raw.getData());
    s.frames.resize ((size_t) numFrames);
    for (int i = 0; i < numFrames; ++i)
        s.frames[(size_t) i] = { f[3 * i], f[3 * i + 1], f[3 * i + 2] };

    if (in.getNumBytesRemaining() >= 12 && in.readInt() == kEditsMagic)
    {
        const auto bytes = (size_t) in.readInt64();
        if (in.getNumBytesRemaining() < (juce::int64) bytes)
            return false;
        juce::MemoryBlock e;
        in.readIntoMemoryBlock (e, (ssize_t) bytes);
        s.edits.assign (static_cast<const char*> (e.getData()), e.getSize());
    }
    return true;
}

} // namespace claudetune
