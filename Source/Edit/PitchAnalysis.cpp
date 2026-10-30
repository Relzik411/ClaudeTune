#include "PitchAnalysis.h"
#include "../Engine/PitchDetector.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace claudetune::edit
{

namespace
{
    struct Candidate
    {
        float period;  // samples
        float midi;
        float cost;
    };

    constexpr int kMaxCandidates = 6;
    constexpr float kTransitionPerSemitone = 0.12f;
    constexpr float kVoicingSwitchCost = 0.4f;
    constexpr float kUnvoicedCost = 0.3f;

    inline float periodToMidi (double sr, double period) noexcept
    {
        return (float) (69.0 + 12.0 * std::log2 ((sr / period) / 440.0));
    }
}

float PitchTrack::pitchAtSample (double samplePos) const noexcept
{
    if (frames.empty())
        return -1.0f;

    const double f = samplePos / hop;
    const int f0 = (int) std::floor (f);
    const double frac = f - f0;
    const bool v0 = voiced (f0), v1 = voiced (f0 + 1);

    if (v0 && v1)
        return frames[(size_t) f0].midi + (float) frac * (frames[(size_t) f0 + 1].midi - frames[(size_t) f0].midi);
    if (v0 && frac < 0.5) return frames[(size_t) f0].midi;
    if (v1 && frac >= 0.5) return frames[(size_t) f0 + 1].midi;
    return -1.0f;
}

PitchTrack analyzePitch (const float* x, int64_t numSamples, double sr, const AnalysisSettings& s)
{
    PitchTrack track;
    track.sampleRate = sr;
    track.numSamples = numSamples;
    track.hop = analysisHop (sr, s);
    track.frames = analyzeFrames (x, numSamples, sr, s, 0, numFramesFor (numSamples, track.hop));
    return track;
}

std::vector<PitchFrame> analyzeFrames (const float* x, int64_t numSamples, double sr,
                                       const AnalysisSettings& s, int firstFrame, int endFrame)
{
    const int hop = analysisHop (sr, s);
    firstFrame = std::max (0, firstFrame);
    endFrame = std::max (firstFrame, endFrame);
    const int numFrames = endFrame - firstFrame;

    std::vector<PitchFrame> out ((size_t) numFrames);
    if (numFrames == 0)
        return out;

    const int tauMax = (int) std::ceil (sr / s.minHz);
    const int tauMin = std::max (2, (int) std::floor (sr / s.maxHz));
    const int window = tauMax;
    const int length = window + tauMax;

    PitchDetector detector;
    detector.prepare (length + 8);
    std::vector<float> buffer ((size_t) length, 0.0f);

    std::vector<std::vector<Candidate>> candidates ((size_t) numFrames);
    std::vector<float> unvoicedCost ((size_t) numFrames, kUnvoicedCost);
    const float gate = std::pow (10.0f, s.gateDb / 20.0f);

    // ---- 1. per-frame candidates -------------------------------------------
    for (int i = 0; i < numFrames; ++i)
    {
        const int64_t centre = (int64_t) (firstFrame + i) * hop;
        // The YIN pair (window, window shifted by tau) is centred on
        // start + (window + tau) / 2; centre it for a mid-range lag.
        const int64_t start = centre - (window + tauMax / 2) / 2;

        for (int k = 0; k < length; ++k)
        {
            const int64_t idx = start + k;
            buffer[(size_t) k] = (idx >= 0 && idx < numSamples) ? x[idx] : 0.0f;
        }

        double energy = 0.0;
        for (int k = -window / 2; k < window / 2; ++k)
        {
            const int64_t idx = centre + k;
            if (idx >= 0 && idx < numSamples)
                energy += (double) x[idx] * x[idx];
        }
        const float rms = (float) std::sqrt (energy / window);
        out[(size_t) i].rmsDb = 20.0f * std::log10 (std::max (rms, 1e-6f));

        if (rms < gate)
        {
            unvoicedCost[(size_t) i] = 0.0f;
            continue; // silence: no voiced candidates
        }

        const float* cm = detector.computeCmndf (buffer.data(), window, tauMax);

        auto& cands = candidates[(size_t) i];
        for (int tau = tauMin + 1; tau < tauMax - 1; ++tau)
        {
            if (cm[tau] < 0.6f && cm[tau] < cm[tau - 1] && cm[tau] <= cm[tau + 1])
            {
                const float period = detector.refineMinimum (tau, tauMin, tauMax);
                cands.push_back ({ period, periodToMidi (sr, period), cm[tau] });
            }
        }

        std::sort (cands.begin(), cands.end(), [] (const Candidate& a, const Candidate& b) { return a.cost < b.cost; });
        if ((int) cands.size() > kMaxCandidates)
            cands.resize (kMaxCandidates);

        // A dip at twice the period of a good dip is usually a sub-octave
        // echo of the true pitch: make it less attractive.
        for (auto& c : cands)
            for (const auto& o : cands)
            {
                const float ratio = c.period / o.period;
                if (std::abs (ratio - 2.0f) < 0.06f && o.cost < c.cost + 0.1f)
                    c.cost += 0.15f;
            }
    }

    // ---- 2. Viterbi over candidates + an unvoiced state ----------------------
    // State index: 0..K-1 candidates, K = unvoiced
    std::vector<std::vector<float>> score ((size_t) numFrames);
    std::vector<std::vector<int>> back ((size_t) numFrames);

    for (int f = 0; f < numFrames; ++f)
    {
        const auto& cands = candidates[(size_t) f];
        const int K = (int) cands.size();
        score[(size_t) f].assign ((size_t) K + 1, std::numeric_limits<float>::max());
        back[(size_t) f].assign ((size_t) K + 1, -1);

        for (int k = 0; k <= K; ++k)
        {
            const bool kVoiced = k < K;
            const float obs = kVoiced ? cands[(size_t) k].cost : unvoicedCost[(size_t) f];

            if (f == 0)
            {
                score[0][(size_t) k] = obs;
                continue;
            }

            const auto& prevC = candidates[(size_t) f - 1];
            const int P = (int) prevC.size();
            float best = std::numeric_limits<float>::max();
            int bestJ = 0;
            for (int j = 0; j <= P; ++j)
            {
                const bool jVoiced = j < P;
                float t = 0.0f;
                if (jVoiced && kVoiced)
                    t = kTransitionPerSemitone * std::abs (prevC[(size_t) j].midi - cands[(size_t) k].midi);
                else if (jVoiced != kVoiced)
                    t = kVoicingSwitchCost;
                const float v = score[(size_t) f - 1][(size_t) j] + t;
                if (v < best)
                {
                    best = v;
                    bestJ = j;
                }
            }
            score[(size_t) f][(size_t) k] = best + obs;
            back[(size_t) f][(size_t) k] = bestJ;
        }
    }

    // backtrack
    std::vector<float> rawMidi ((size_t) numFrames, -1.0f);
    std::vector<float> rawCost ((size_t) numFrames, 1.0f);
    std::vector<float> rawPeriod ((size_t) numFrames, 0.0f);
    {
        const auto& last = score[(size_t) numFrames - 1];
        int k = (int) (std::min_element (last.begin(), last.end()) - last.begin());
        for (int f = numFrames - 1; f >= 0; --f)
        {
            const auto& cands = candidates[(size_t) f];
            if (k < (int) cands.size())
            {
                rawMidi[(size_t) f] = cands[(size_t) k].midi;
                rawCost[(size_t) f] = cands[(size_t) k].cost;
                rawPeriod[(size_t) f] = cands[(size_t) k].period;
            }
            k = back[(size_t) f][(size_t) k];
            if (k < 0) k = 0;
        }
    }

    // ---- 3. re-centre in time ------------------------------------------------
    // For lag tau the estimate describes the audio around start + (window + tau) / 2,
    // i.e. centre + (tau - tauMax / 2) / 2. Resample voiced runs onto the frame grid.
    std::vector<float> midi ((size_t) numFrames, -1.0f);
    auto timeOf = [&] (int g) { return (double) g * hop + 0.5 * (rawPeriod[(size_t) g] - tauMax / 2.0); };
    for (int a = 0; a < numFrames;)
    {
        if (rawMidi[(size_t) a] < 0.0f) { ++a; continue; }
        int b = a;
        while (b < numFrames && rawMidi[(size_t) b] >= 0.0f) ++b;

        int g = a;
        for (int f = a; f < b; ++f)
        {
            const double t = (double) f * hop;
            while (g + 1 < b && timeOf (g + 1) <= t) ++g;
            if (t <= timeOf (a))
                midi[(size_t) f] = rawMidi[(size_t) a];
            else if (g + 1 >= b)
                midi[(size_t) f] = rawMidi[(size_t) b - 1];
            else
            {
                const double ta = timeOf (g), tb = timeOf (g + 1);
                const double w = tb > ta ? std::clamp ((t - ta) / (tb - ta), 0.0, 1.0) : 0.0;
                midi[(size_t) f] = rawMidi[(size_t) g] + (float) w * (rawMidi[(size_t) g + 1] - rawMidi[(size_t) g]);
            }
        }
        a = b;
    }

    // ---- 4. drop voiced blips shorter than 30 ms ---------------------------
    const int minRun = std::max (2, (int) std::lround (0.030 / s.hopSeconds));
    for (int f = 0; f < numFrames;)
    {
        if (midi[(size_t) f] < 0.0f) { ++f; continue; }
        int e = f;
        while (e < numFrames && midi[(size_t) e] >= 0.0f) ++e;
        if (e - f < minRun)
            for (int g = f; g < e; ++g)
                midi[(size_t) g] = -1.0f;
        f = e;
    }

    for (int f = 0; f < numFrames; ++f)
    {
        out[(size_t) f].midi = midi[(size_t) f];
        out[(size_t) f].confidence = midi[(size_t) f] >= 0.0f ? std::clamp (1.0f - rawCost[(size_t) f], 0.0f, 1.0f) : 0.0f;
    }

    return out;
}

} // namespace claudetune::edit
