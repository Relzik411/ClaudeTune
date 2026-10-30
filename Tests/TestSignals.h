#pragma once
// Shared synthetic test material for ClaudeTune tests.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace testsig
{

constexpr double kPi = 3.14159265358979323846;

inline double midiToHz (double n) { return 440.0 * std::pow (2.0, (n - 69.0) / 12.0); }

// Additive "voice": harmonics shaped by three vowel formants. pitchAt(t)
// returns a MIDI note, ampAt(t) a linear amplitude (0 = silent).
inline std::vector<float> voice (double sr, double seconds,
                                 const std::function<double (double)>& pitchAt,
                                 const std::function<double (double)>& ampAt,
                                 double formantScale = 1.0)
{
    const int n = (int) (sr * seconds);
    std::vector<float> out ((size_t) n, 0.0f);
    const double formants[3] = { 730 * formantScale, 1090 * formantScale, 2440 * formantScale };
    const double widths[3] = { 90, 110, 160 };
    const double gains[3] = { 1.0, 0.5, 0.25 };
    std::vector<double> phases (100, 0.0);

    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        const double a = ampAt (t);
        const double f0 = midiToHz (pitchAt (t));
        double v = 0.0;
        for (int h = 1; h < 100; ++h)
        {
            const double fh = f0 * h;
            if (fh > sr * 0.45) break;
            double env = 0.02;
            for (int k = 0; k < 3; ++k)
            {
                const double x = (fh - formants[k]) / widths[k];
                env += gains[k] / (1.0 + x * x);
            }
            phases[(size_t) h] += 2.0 * kPi * fh / sr;
            if (a > 0.0)
                v += env * std::sin (phases[(size_t) h]) / std::sqrt ((double) h);
        }
        out[(size_t) i] = (float) (v * a);
    }
    return out;
}

inline void normalise (std::vector<float>& x, float peak)
{
    float m = 1e-9f;
    for (auto v : x) m = std::max (m, std::abs (v));
    for (auto& v : x) v *= peak / m;
}

inline void addNoiseBurst (std::vector<float>& x, double sr, double t0, double t1, float level, unsigned seed = 1)
{
    std::mt19937 rng (seed);
    std::normal_distribution<float> nd (0.0f, 1.0f);
    float hp = 0.0f, prev = 0.0f;
    for (int i = (int) (t0 * sr); i < (int) (t1 * sr) && i < (int) x.size(); ++i)
    {
        const float w = nd (rng);
        hp = 0.9f * (hp + w - prev); // crude high-pass for an "s"/"t" sound
        prev = w;
        const double u = (i / sr - t0) / (t1 - t0);
        x[(size_t) i] += level * hp * (float) std::sin (kPi * u);
    }
}

inline void writeWav (const std::string& path, const std::vector<float>& x, int sr)
{
    FILE* f = std::fopen (path.c_str(), "wb");
    if (! f) return;
    const uint32_t dataBytes = (uint32_t) x.size() * 2;
    auto w32 = [&] (uint32_t v) { std::fwrite (&v, 4, 1, f); };
    auto w16 = [&] (uint16_t v) { std::fwrite (&v, 2, 1, f); };
    std::fwrite ("RIFF", 1, 4, f); w32 (36 + dataBytes); std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); w32 (16); w16 (1); w16 (1); w32 ((uint32_t) sr); w32 ((uint32_t) sr * 2); w16 (2); w16 (16);
    std::fwrite ("data", 1, 4, f); w32 (dataBytes);
    for (auto v : x)
        w16 ((uint16_t) (int16_t) std::lround (std::clamp (v, -1.0f, 1.0f) * 32767.0f));
    std::fclose (f);
}

// A sung-style phrase with known note boundaries.
struct TruthNote
{
    double start, end;  // seconds
    double pitch;       // intended MIDI note centre (what the singer is "near")
    bool legatoIntoNext;
};

struct Phrase
{
    std::vector<float> audio;
    std::vector<TruthNote> notes;
    double sampleRate;
};

// 8 notes: scoops, drift, vibrato, legato slides, consonant gaps and one
// repeated note separated only by a loudness dip.
inline Phrase makePhrase (double sr, double offsetCents = 0.0)
{
    Phrase ph;
    ph.sampleRate = sr;
    // start, end, pitch (slightly off-key), legato into next
    ph.notes = {
        { 0.20, 0.62, 60.30, false },
        { 0.72, 1.10, 61.75, true },  // legato slide into next
        { 1.10, 1.60, 64.20, false },
        { 1.72, 2.30, 67.35, false }, // long note with vibrato
        { 2.40, 2.70, 64.80, false },
        { 2.74, 3.04, 64.80, false }, // repeated pitch, separated by a dip only
        { 3.16, 3.60, 62.25, true },
        { 3.60, 4.20, 59.70, false },
    };
    for (auto& n : ph.notes) n.pitch += offsetCents / 100.0;
    const double dur = 4.5;
    const auto notes = ph.notes;

    auto pitchAt = [notes] (double t) {
        // find current or nearest note
        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto& n = notes[i];
            if (t < n.start - 0.05) return i == 0 ? n.pitch : notes[i - 1].pitch;
            if (t <= n.end)
            {
                double p = n.pitch;
                const double local = t - n.start;
                // legato slide from previous note over 60 ms
                if (i > 0 && notes[i - 1].legatoIntoNext && local < 0.06)
                    p = notes[i - 1].pitch + (n.pitch - notes[i - 1].pitch) * (0.5 - 0.5 * std::cos (kPi * local / 0.06));
                else
                    p += -0.45 * std::exp (-std::max (0.0, local) / 0.04); // scoop up into the note
                p += 0.12 * std::sin (2 * kPi * 0.8 * t);                   // slow drift
                if (local > 0.2) p += 0.30 * std::sin (2 * kPi * 5.4 * t);  // vibrato on held parts
                return p;
            }
        }
        return notes.back().pitch;
    };
    auto ampAt = [notes] (double t) {
        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto& n = notes[i];
            if (t >= n.start && t <= n.end)
            {
                const double in = (i > 0 && notes[i - 1].legatoIntoNext) ? 1.0 : std::min (1.0, (t - n.start) / 0.015);
                const double out = n.legatoIntoNext ? 1.0 : std::min (1.0, (n.end - t) / 0.02);
                return in * out;
            }
        }
        return 0.0;
    };

    ph.audio = voice (sr, dur, pitchAt, ampAt);
    normalise (ph.audio, 0.5f);
    // consonants in some gaps
    addNoiseBurst (ph.audio, sr, 0.63, 0.71, 0.12f, 3);
    addNoiseBurst (ph.audio, sr, 1.62, 1.71, 0.10f, 4);
    addNoiseBurst (ph.audio, sr, 3.06, 3.15, 0.12f, 5);
    return ph;
}

} // namespace testsig
