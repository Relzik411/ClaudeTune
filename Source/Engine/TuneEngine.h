#pragma once
// ClaudeTune - real-time monophonic pitch-correction engine.
//
// Signal path:
//   input -> YIN pitch detection (every hop) -> target-note logic
//         (scale / note states / tolerance / MIDI) -> speed, transition and
//         vibrato smoothing -> pitch-synchronous overlap-add (PSOLA) shifter.
//
// PSOLA keeps the vocal formants in place ("Formant: Corrected"). The
// "Non-Corrected" option resamples each grain, which moves the formants
// with the pitch like a classic pitch shifter.
//
// The engine has no JUCE dependency so it can be unit-tested on its own.

#include "PitchDetector.h"
#include "Scales.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace claudetune
{

struct EngineParams
{
    float speedMs          = 15.0f;   // 0.1 .. 800
    float transitionMs     = 120.0f;  // 0.1 .. 800
    float toleranceCents   = 0.0f;    // 0 = Off, up to 40
    float toleranceTimeMs  = 0.0f;    // 0 = Off, up to 300
    bool  vibratoOn        = false;
    float vibratoDepth     = 1.0f;    // 0 .. 2 (0% .. 200%)
    bool  correctionOn     = true;
    float correctionAmount = 1.0f;    // 0 .. 1
    bool  formantCorrected = true;
    int   rangeLow         = 36;
    int   rangeHigh        = 84;
    float referenceHz      = 440.0f;
    bool  referenceToneOn  = false;
    bool  targetPitchOn    = false;
    float referenceToneDb  = 0.0f;    // -18 .. +18
};

struct EngineTelemetry
{
    std::atomic<float> inputPeak { 0.0f };
    std::atomic<float> outputPeak { 0.0f };
    std::atomic<float> detectedPitch { -1.0f }; // MIDI note (fractional), -1 = none
    std::atomic<int>   targetNote { -1 };       // -1 = none / bypassed
    std::atomic<float> correctionCents { 0.0f };
    std::atomic<int>   latency { 0 };
};

class TuneEngine
{
public:
    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();

    void setParams (const EngineParams& p);
    void setNoteStates (const NoteStateArray& states) noexcept { noteStates = states; }

    void noteOn (int note) noexcept;
    void noteOff (int note) noexcept;
    void allNotesOff() noexcept;

    // In-place processing. numChannels must be <= the value given to prepare().
    void process (float* const* channels, int numChannels, int numSamples) noexcept;

    int getLatencySamples() const noexcept { return delay; }
    static int latencyForRange (double sampleRate, int rangeLow);

    EngineTelemetry telemetry;

private:
    void configureForRange();
    void runDetection() noexcept;
    void updateCorrection (bool voiced, float pitch) noexcept;
    int  resolveTarget (float pitch) const noexcept;
    bool isLegal (int note) const noexcept;
    void generateMarks() noexcept;
    void placeGrains (int64_t outIndex) noexcept;
    float smoothingCoef (float ms) const noexcept;
    int  heldNote() const noexcept;
    float outputPitchAt (double inputTime) noexcept;
    void pushTimeline (double time, float outputPitch) noexcept;

    static int hopForRate (double sr) noexcept;

    // configuration
    double sr = 48000.0;
    int numCh = 2;
    int hop = 128;
    int tauMin = 20;
    int maxPeriod = 800;   // longest period that can be corrected
    int window = 800;      // YIN integration window
    int detectMaxLag = 1600; // detector looks an octave below the range
    int delay = 1800;
    int unvoicedPeriod = 240;
    EngineParams params;
    int configuredLow = -1, configuredHigh = -1;
    NoteStateArray noteStates = buildScale (0, 0);

    // buffers
    int ringSize = 0, ringMask = 0;
    std::vector<std::vector<float>> inRing, accRing;
    std::vector<float> monoRing, detectBuffer;
    PitchDetector detector;

    int64_t written = 0;  // number of input samples written
    int hopCountdown = 0;

    // analysis marks (sub-sample positions in the input timeline)
    static constexpr int kMarkCap = 4096;
    std::array<double, kMarkCap> markPos {};
    std::array<float, kMarkCap> markPeriod {};
    int64_t markWrite = 0, markRead = 0;
    double lastMark = 0.0;
    float markSourcePeriod = 0.0f; // current analysis period (0 = unvoiced / uncorrectable)

    double nextSynth = 0.0;

    // Desired output pitch (MIDI units, -1 = leave untouched) keyed to input
    // time, so each grain is spaced for the audio it actually carries.
    static constexpr int kTimelineCap = 512;
    std::array<double, kTimelineCap> timelineTime {};
    std::array<float, kTimelineCap> timelinePitch {};
    int64_t timelineWrite = 0, timelineRead = 0;

    // pitch tracking / correction state
    float pitchHistory[3] {};
    int pitchHistoryCount = 0;
    static constexpr int kNoTarget = -2;
    int currentTarget = kNoTarget;
    float pendingMs = 0.0f;
    bool prevInRange = false;
    bool prevGate = false;
    float targetSmooth = 0.0f;
    float offsetState = 0.0f;
    float shiftOut = 0.0f; // semitones, most recent value

    static constexpr int kVibCap = 512;
    std::array<float, kVibCap> vibBuf {};
    int vibLen = 64, vibPos = 0, vibCount = 0;
    double vibSum = 0.0;

    // wet/dry crossfade for Correction On/Off
    float wetGain = 1.0f;

    // MIDI
    std::array<int, 16> heldStack {};
    int heldCount = 0;

    // reference tone
    double tonePhase = 0.0;
    float toneEnv = 0.0f;
};

} // namespace claudetune
