#pragma once
// ClaudeTune editor - offline pitch analysis.
//
// Unlike the live engine, the editor can look at the whole take, so it uses
// several YIN candidates per frame and picks the smoothest path through them
// (Viterbi). This removes the octave jumps that a frame-by-frame tracker makes.

#include <cstdint>
#include <vector>

namespace claudetune::edit
{

struct PitchFrame
{
    float midi = -1.0f;        // fractional MIDI note (A4 = 69 at 440 Hz), -1 = unvoiced
    float confidence = 0.0f;   // 0..1
    float rmsDb = -120.0f;     // frame loudness
};

struct PitchTrack
{
    double sampleRate = 48000.0;
    int hop = 240;                    // samples between frames; frame f is centred on sample f * hop
    int64_t numSamples = 0;
    std::vector<PitchFrame> frames;

    int numFrames() const noexcept { return (int) frames.size(); }
    bool voiced (int f) const noexcept { return f >= 0 && f < numFrames() && frames[(size_t) f].midi >= 0.0f; }
    double frameToSeconds (double f) const noexcept { return f * hop / sampleRate; }
    double secondsToFrame (double s) const noexcept { return s * sampleRate / hop; }

    // Interpolated pitch at a (fractional) sample position; -1 when unvoiced.
    float pitchAtSample (double samplePos) const noexcept;
};

struct AnalysisSettings
{
    float minHz = 60.0f;
    float maxHz = 1200.0f;
    float gateDb = -50.0f;     // quieter frames are treated as silence
    double hopSeconds = 0.005; // 5 ms frames
};

inline int analysisHop (double sampleRate, const AnalysisSettings& s = {}) noexcept
{
    const int h = (int) (sampleRate * s.hopSeconds + 0.5);
    return h < 16 ? 16 : h;
}

inline int numFramesFor (int64_t numSamples, int hop) noexcept { return (int) (numSamples / hop) + 1; }

// Whole-take analysis.
PitchTrack analyzePitch (const float* mono, int64_t numSamples, double sampleRate,
                         const AnalysisSettings& settings = {});

// Analyses only frames [firstFrame, endFrame) of a take (frame f is centred
// on sample f * hop). Used to update part of a track after new audio arrives;
// give it a second or so of context either side and keep the middle.
std::vector<PitchFrame> analyzeFrames (const float* mono, int64_t numSamples, double sampleRate,
                                       const AnalysisSettings& settings, int firstFrame, int endFrame);

} // namespace claudetune::edit
