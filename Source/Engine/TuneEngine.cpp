#include "TuneEngine.h"

#include <algorithm>
#include <cmath>

namespace claudetune
{

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    inline double midiToHz (double note, double ref = 440.0) noexcept
    {
        return ref * std::pow (2.0, (note - 69.0) / 12.0);
    }

    inline void storeMax (std::atomic<float>& a, float v) noexcept
    {
        float cur = a.load (std::memory_order_relaxed);
        while (v > cur && ! a.compare_exchange_weak (cur, v, std::memory_order_relaxed))
        {
        }
    }

    // Detection margins around the selected range (semitones). The low margin
    // also covers a reference pitch down to 415 Hz.
    constexpr int kLowMargin  = 3;
    constexpr int kHighMargin = 4;
}

//==============================================================================
int TuneEngine::hopForRate (double rate) noexcept
{
    if (rate <= 50000.0)  return 128;
    if (rate <= 100000.0) return 256;
    return 512;
}

int TuneEngine::latencyForRange (double rate, int rangeLow)
{
    const int maxP = (int) std::ceil (rate / midiToHz (rangeLow - kLowMargin));
    return 2 * maxP + hopForRate (rate) + 8;
}

//==============================================================================
void TuneEngine::prepare (double sampleRate, int maxBlockSize, int numChannels)
{
    sr = sampleRate;
    numCh = std::max (1, numChannels);
    hop = hopForRate (sr);

    // Size every buffer for the widest possible range so later range changes
    // never allocate on the audio thread.
    const int worstPeriod = (int) std::ceil (sr / midiToHz (kKeyboardLow - kLowMargin));
    const int worstDelay = latencyForRange (sr, kKeyboardLow);
    const int needed = 4 * (worstDelay + 4 * worstPeriod) + std::max (maxBlockSize, 0) + 1024;

    ringSize = 1;
    while (ringSize < needed)
        ringSize <<= 1;
    ringMask = ringSize - 1;

    inRing.assign ((size_t) numCh, std::vector<float> ((size_t) ringSize, 0.0f));
    accRing.assign ((size_t) numCh, std::vector<float> ((size_t) ringSize, 0.0f));
    monoRing.assign ((size_t) ringSize, 0.0f);
    detectBuffer.assign ((size_t) (3 * worstPeriod + 16), 0.0f);
    detector.prepare (3 * worstPeriod + 16);

    configuredLow = configuredHigh = -1;
    configureForRange();
}

void TuneEngine::configureForRange()
{
    configuredLow = params.rangeLow;
    configuredHigh = params.rangeHigh;

    maxPeriod = (int) std::ceil (sr / midiToHz (configuredLow - kLowMargin));
    tauMin = std::max (2, (int) std::floor (sr / midiToHz (configuredHigh + kHighMargin)));
    window = maxPeriod;
    detectMaxLag = 2 * maxPeriod; // see an octave below the range so low notes are not mistaken for harmonics
    delay = latencyForRange (sr, configuredLow);
    unvoicedPeriod = std::clamp ((int) std::lround (sr / 200.0), 2, maxPeriod);
    telemetry.latency.store (delay);

    reset();
}

void TuneEngine::reset()
{
    for (auto& ch : inRing)  std::fill (ch.begin(), ch.end(), 0.0f);
    for (auto& ch : accRing) std::fill (ch.begin(), ch.end(), 0.0f);
    std::fill (monoRing.begin(), monoRing.end(), 0.0f);

    written = 0;
    hopCountdown = hop;

    markPos[0] = 0.0;
    markPeriod[0] = 0.0f;
    markRead = 0;
    markWrite = 1;
    lastMark = 0.0;
    markSourcePeriod = 0.0f;

    nextSynth = (double) delay;

    timelineWrite = timelineRead = 0;
    pushTimeline (0.0, -1.0f);

    pitchHistoryCount = 0;
    currentTarget = kNoTarget;
    pendingMs = 0.0f;
    prevInRange = prevGate = false;
    targetSmooth = offsetState = shiftOut = 0.0f;
    vibCount = vibPos = 0;
    vibSum = 0.0;
    wetGain = params.correctionOn ? 1.0f : 0.0f;
    toneEnv = 0.0f;
    tonePhase = 0.0;

    telemetry.detectedPitch.store (-1.0f);
    telemetry.targetNote.store (-1);
    telemetry.correctionCents.store (0.0f);
}

void TuneEngine::setParams (const EngineParams& p)
{
    params = p;
    params.rangeLow = std::clamp (params.rangeLow, kKeyboardLow, kKeyboardHigh - 1);
    params.rangeHigh = std::clamp (params.rangeHigh, params.rangeLow + 1, kKeyboardHigh);
    params.referenceHz = std::clamp (params.referenceHz, 400.0f, 480.0f);
    params.correctionAmount = std::clamp (params.correctionAmount, 0.0f, 1.0f);
    params.vibratoDepth = std::clamp (params.vibratoDepth, 0.0f, 2.0f);

    if (params.rangeLow != configuredLow || params.rangeHigh != configuredHigh)
        configureForRange();
}

//==============================================================================
void TuneEngine::noteOn (int note) noexcept
{
    noteOff (note);
    if (heldCount < (int) heldStack.size())
        heldStack[(size_t) heldCount++] = note;
}

void TuneEngine::noteOff (int note) noexcept
{
    for (int i = 0; i < heldCount; ++i)
    {
        if (heldStack[(size_t) i] == note)
        {
            for (int j = i; j < heldCount - 1; ++j)
                heldStack[(size_t) j] = heldStack[(size_t) j + 1];
            --heldCount;
            return;
        }
    }
}

void TuneEngine::allNotesOff() noexcept { heldCount = 0; }

int TuneEngine::heldNote() const noexcept
{
    return heldCount > 0 ? heldStack[(size_t) heldCount - 1] : -1;
}

//==============================================================================
float TuneEngine::smoothingCoef (float ms) const noexcept
{
    // The time constant is a third of the control value, so a glide is
    // about 95% complete after the time shown on the knob.
    const double tau = std::max (0.05, (double) ms) / 3000.0;
    const double dt = (double) hop / sr;
    return (float) std::exp (-dt / tau);
}

bool TuneEngine::isLegal (int note) const noexcept
{
    return note >= 0 && note < 128 && noteStates[(size_t) note] == NoteState::Legal;
}

int TuneEngine::resolveTarget (float pitch) const noexcept
{
    const int nearest = std::clamp ((int) std::lround (pitch), 0, 127);

    switch (noteStates[(size_t) nearest])
    {
        case NoteState::Legal:
            return nearest;

        case NoteState::Bypass:
            return -1;

        case NoteState::Illegal:
            for (int d = 1; d <= 24; ++d)
            {
                const bool up = isLegal (nearest + d);
                const bool down = isLegal (nearest - d);
                if (up && down)
                    return std::abs (pitch - (float) (nearest + d)) <= std::abs (pitch - (float) (nearest - d))
                               ? nearest + d : nearest - d;
                if (up)   return nearest + d;
                if (down) return nearest - d;
            }
            return -1;

        case NoteState::IllegalUp:
            for (int d = 1; d <= 24; ++d)
                if (isLegal (nearest + d))
                    return nearest + d;
            return -1;

        case NoteState::IllegalDown:
            for (int d = 1; d <= 24; ++d)
                if (isLegal (nearest - d))
                    return nearest - d;
            return -1;
    }
    return -1;
}

//==============================================================================
void TuneEngine::pushTimeline (double time, float outputPitch) noexcept
{
    const size_t i = (size_t) (timelineWrite % kTimelineCap);
    timelineTime[i] = time;
    timelinePitch[i] = outputPitch;
    ++timelineWrite;
    if (timelineWrite - timelineRead > kTimelineCap - 2)
        timelineRead = timelineWrite - (kTimelineCap - 2);
}

float TuneEngine::outputPitchAt (double t) noexcept
{
    while (timelineRead + 1 < timelineWrite
           && timelineTime[(size_t) ((timelineRead + 1) % kTimelineCap)] <= t)
        ++timelineRead;

    const size_t a = (size_t) (timelineRead % kTimelineCap);
    if (timelineRead + 1 >= timelineWrite || t <= timelineTime[a])
        return timelinePitch[a];

    const size_t b = (size_t) ((timelineRead + 1) % kTimelineCap);
    const float pa = timelinePitch[a], pb = timelinePitch[b];
    const double span = timelineTime[b] - timelineTime[a];
    const float f = span > 0.0 ? (float) ((t - timelineTime[a]) / span) : 1.0f;

    if (pa < 0.0f || pb < 0.0f) // no interpolation across voicing changes
        return f < 0.5f ? pa : pb;
    return pa + f * (pb - pa);
}

//==============================================================================
void TuneEngine::runDetection() noexcept
{
    // Newest audio first: the integration window sits on the most recent
    // samples, which keeps the estimate close to "now".
    const int length = window + detectMaxLag;
    double energy = 0.0;
    for (int i = 0; i < length; ++i)
    {
        const float v = monoRing[(size_t) ((written - 1 - i) & ringMask)];
        detectBuffer[(size_t) i] = v;
        if (i < window)
            energy += (double) v * v;
    }
    const double rms = std::sqrt (energy / window);

    float aperiodicity = 1.0f;
    const float period = detector.detect (detectBuffer.data(), window, tauMin, detectMaxLag, 0.15f, aperiodicity);

    const bool voiced = period > 1.0f && aperiodicity < 0.3f && rms > 0.003; // ~ -50 dBFS gate

    // The estimate describes the audio centred between the two compared segments.
    double analysisTime = (double) written - 0.5 * (window + (voiced ? period : (float) window));

    float pitch = -1.0f;
    if (voiced)
    {
        const float raw = 69.0f + 12.0f * (float) std::log2 ((sr / period) / params.referenceHz);

        // median of the last three estimates rejects single-hop octave errors
        if (pitchHistoryCount < 3)
            pitchHistory[pitchHistoryCount++] = raw;
        else
        {
            pitchHistory[0] = pitchHistory[1];
            pitchHistory[1] = pitchHistory[2];
            pitchHistory[2] = raw;
        }

        if (pitchHistoryCount == 3)
        {
            const float a = pitchHistory[0], b = pitchHistory[1], c = pitchHistory[2];
            pitch = std::max (std::min (a, b), std::min (std::max (a, b), c));
            analysisTime -= hop; // the median lags by about one hop
        }
        else
            pitch = raw;

        const float p = (float) (sr / midiToHz (pitch, params.referenceHz));
        // Periods longer than the shifter can handle are passed through untouched.
        markSourcePeriod = (p >= (float) tauMin * 0.5f && p <= (float) maxPeriod) ? p : 0.0f;
    }
    else
    {
        pitchHistoryCount = 0;
        markSourcePeriod = 0.0f;
    }

    updateCorrection (voiced, pitch);
    pushTimeline (analysisTime, markSourcePeriod > 0.0f ? pitch + shiftOut : -1.0f);
}

void TuneEngine::updateCorrection (bool voiced, float pitch) noexcept
{
    const float dtMs = (float) (1000.0 * hop / sr);

    const bool inRange = voiced
                         && markSourcePeriod > 0.0f
                         && pitch >= (float) params.rangeLow - 0.5f
                         && pitch <= (float) params.rangeHigh + 0.5f;

    // Vibrato centre: moving average over roughly one vibrato cycle. It is
    // restarted at note onsets and on real jumps, so it tracks the sung note.
    float centre = pitch;
    if (inRange)
    {
        const int vibWanted = std::clamp ((int) std::lround (180.0f / dtMs), 4, kVibCap);
        const bool jump = vibCount > 0 && std::abs (pitch - (float) (vibSum / vibCount)) > 1.0f;
        if (! prevInRange || jump || vibWanted != vibLen)
        {
            vibLen = vibWanted;
            vibCount = 0;
            vibPos = 0;
            vibSum = 0.0;
        }
        if (vibCount == vibLen)
            vibSum -= vibBuf[(size_t) vibPos];
        else
            ++vibCount;
        vibBuf[(size_t) vibPos] = pitch;
        vibSum += pitch;
        vibPos = (vibPos + 1) % vibLen;
        centre = (float) (vibSum / vibCount);
    }

    // With Vibrato on, note decisions follow the vibrato centre so the
    // vibrato swing never flips the target note.
    const float notePitch = params.vibratoOn ? centre : pitch;

    int target = -1;

    if (inRange)
    {
        const int midiTarget = params.targetPitchOn ? heldNote() : -1;

        if (midiTarget >= 0)
        {
            currentTarget = midiTarget;
            pendingMs = 0.0f;
        }
        else
        {
            const int candidate = resolveTarget (notePitch);

            if (! prevInRange || currentTarget == kNoTarget)
            {
                currentTarget = candidate;
                pendingMs = 0.0f;
            }
            else if (candidate != currentTarget)
            {
                const bool centsOn = params.toleranceCents > 0.0f;
                const bool timeOn = params.toleranceTimeMs > 0.0f;
                bool change = true;

                if (centsOn || timeOn)
                {
                    bool passCents = false;
                    if (centsOn)
                    {
                        // The pitch must clear the note boundary by the
                        // tolerance before a new target is accepted.
                        float pulled = notePitch;
                        if (currentTarget >= 0)
                            pulled += (notePitch > (float) currentTarget ? -1.0f : 1.0f) * params.toleranceCents / 100.0f;
                        passCents = resolveTarget (pulled) != currentTarget;
                    }

                    pendingMs += dtMs;
                    const bool passTime = timeOn && pendingMs >= params.toleranceTimeMs;
                    change = passCents || passTime;
                }

                if (change)
                {
                    currentTarget = candidate;
                    pendingMs = 0.0f;
                }
            }
            else
            {
                pendingMs = 0.0f;
            }
        }

        target = currentTarget;
    }
    else
    {
        currentTarget = kNoTarget;
        pendingMs = 0.0f;
    }

    prevInRange = inRange;

    const bool gate = inRange && target >= 0;

    if (gate)
    {
        if (! prevGate)
        {
            targetSmooth = (float) target;
            offsetState = 0.0f;
        }

        const float aTransition = smoothingCoef (params.transitionMs);
        targetSmooth += (1.0f - aTransition) * ((float) target - targetSmooth);

        const float source = params.vibratoOn ? centre : pitch;
        const float aSpeed = smoothingCoef (params.speedMs);
        offsetState += (1.0f - aSpeed) * ((targetSmooth - source) - offsetState);

        float shift = offsetState;
        if (params.vibratoOn)
            shift += (params.vibratoDepth - 1.0f) * std::clamp (pitch - centre, -2.0f, 2.0f);

        shift *= params.correctionAmount;
        shiftOut = std::clamp (shift, -12.0f, 12.0f);
    }
    else
    {
        // fade any remaining correction out over ~15 ms
        shiftOut *= smoothingCoef (45.0f);
        if (std::abs (shiftOut) < 1e-4f)
            shiftOut = 0.0f;
    }

    prevGate = gate;

    telemetry.detectedPitch.store (voiced ? pitch : -1.0f, std::memory_order_relaxed);
    telemetry.targetNote.store (gate ? target : -1, std::memory_order_relaxed);
    telemetry.correctionCents.store (gate ? shiftOut * 100.0f : 0.0f, std::memory_order_relaxed);
}

//==============================================================================
void TuneEngine::generateMarks() noexcept
{
    const int64_t newest = written - 1;

    auto sampleAt = [this] (double pos) noexcept
    {
        const double fl = std::floor (pos);
        const int64_t i0 = (int64_t) fl;
        const float f = (float) (pos - fl);
        return monoRing[(size_t) (i0 & ringMask)] * (1.0f - f) + monoRing[(size_t) ((i0 + 1) & ringMask)] * f;
    };

    for (;;)
    {
        const bool voiced = markSourcePeriod > 0.0f;
        const double period = voiced ? (double) markSourcePeriod : (double) unvoicedPeriod;
        const double predicted = lastMark + period;
        const int half = std::max (1, (int) (period * 0.5));
        const int search = voiced ? std::max (1, (int) (period * 0.0625)) : 0;

        if (predicted + search + half + 2 > (double) newest)
            break;

        double best = predicted;

        if (search > 0)
        {
            // Lock the new mark to the waveform of the previous one (with
            // sub-sample precision) so grains stay phase-coherent and the
            // marks never drift against the real period.
            constexpr int kMaxSearch = 128;
            double scores[2 * kMaxSearch + 1];
            const int s = std::min (search, kMaxSearch);
            int bestIdx = 0;
            for (int idx = 0; idx <= 2 * s; ++idx)
            {
                const double candidate = predicted + (idx - s);
                double dot = 0.0, energy = 1e-12;
                for (int k = -half; k < half; ++k)
                {
                    const float a = sampleAt (lastMark + k);
                    const float b = sampleAt (candidate + k);
                    dot += (double) a * b;
                    energy += (double) b * b;
                }
                scores[idx] = dot / std::sqrt (energy);
                if (scores[idx] > scores[bestIdx])
                    bestIdx = idx;
            }

            double refined = bestIdx - s;
            if (bestIdx > 0 && bestIdx < 2 * s)
            {
                const double y0 = scores[bestIdx - 1], y1 = scores[bestIdx], y2 = scores[bestIdx + 1];
                const double den = y0 - 2.0 * y1 + y2;
                if (std::abs (den) > 1e-12)
                    refined += std::clamp (0.5 * (y0 - y2) / den, -0.5, 0.5);
            }
            best = predicted + refined;
        }

        // keep marks strictly increasing
        best = std::max (best, lastMark + 2.0);

        if (markWrite - markRead >= kMarkCap - 1)
            ++markRead; // drop the oldest (should never happen in practice)

        markPos[(size_t) (markWrite % kMarkCap)] = best;
        markPeriod[(size_t) (markWrite % kMarkCap)] = voiced ? (float) period : 0.0f;
        ++markWrite;
        lastMark = best;
    }
}

void TuneEngine::placeGrains (int64_t outIndex) noexcept
{
    for (;;)
    {
        const double s = nextSynth;
        const double inputTime = s - (double) delay;

        // latest analysis mark at or before the matching input time
        while (markRead + 1 < markWrite
               && markPos[(size_t) ((markRead + 1) % kMarkCap)] <= inputTime)
            ++markRead;

        const double mark = markPos[(size_t) (markRead % kMarkCap)];
        const float markP = markPeriod[(size_t) (markRead % kMarkCap)];
        const bool voiced = markP > 0.0f;

        // Unvoiced or uncorrectable audio is re-synthesised 1:1 with fixed grains.
        const double analysisPeriod = voiced ? (double) markP : (double) unvoicedPeriod;
        const float wanted = voiced ? outputPitchAt (inputTime) : -1.0f;
        const double synthPeriod = wanted >= 0.0f
                                     ? std::clamp (sr / midiToHz (wanted, params.referenceHz), 2.0, (double) maxPeriod)
                                     : analysisPeriod;
        const double half = synthPeriod;

        if (s - half > (double) outIndex + 1.0)
            break;

        // Formant corrected: copy the grain at its original rate (PSOLA).
        // Non-corrected: resample the grain so formants follow the pitch.
        const double step = params.formantCorrected ? 1.0 : analysisPeriod / synthPeriod;

        const int64_t s0 = (int64_t) std::floor (s);
        const double frac = s - (double) s0;
        const int kStart = (int) std::ceil (-half + frac);
        const int kEnd = (int) std::floor (half + frac);
        const double invHalf = kPi / half;

        for (int k = kStart; k <= kEnd; ++k)
        {
            const int64_t outPos = s0 + k;
            if (outPos < outIndex)
                continue;

            const double u = (double) k - frac;
            if (u <= -half || u >= half)
                continue;

            const float w = (float) (0.5 + 0.5 * std::cos (u * invHalf));
            const double src = mark + u * step;
            const double srcFloor = std::floor (src);
            const int64_t i0 = (int64_t) srcFloor;
            const float f = (float) (src - srcFloor);

            for (int c = 0; c < numCh; ++c)
            {
                const auto& in = inRing[(size_t) c];
                const float v = in[(size_t) (i0 & ringMask)] * (1.0f - f)
                              + in[(size_t) ((i0 + 1) & ringMask)] * f;
                accRing[(size_t) c][(size_t) (outPos & ringMask)] += w * v;
            }
        }

        nextSynth += synthPeriod;
    }
}

//==============================================================================
void TuneEngine::process (float* const* channels, int numChannels, int numSamples) noexcept
{
    const int chans = std::min (numChannels, numCh);
    if (chans <= 0)
        return;

    float inPeak = 0.0f, outPeak = 0.0f;

    const float wetTarget = params.correctionOn ? 1.0f : 0.0f;
    const float wetStep = (float) (1.0 / (0.010 * sr)); // 10 ms crossfade

    const int toneNote = heldNote();
    const bool toneWanted = params.referenceToneOn && toneNote >= 0;
    const float toneAmp = 0.125f * std::pow (10.0f, params.referenceToneDb / 20.0f);
    const double toneInc = toneNote >= 0 ? 2.0 * kPi * midiToHz (toneNote, params.referenceHz) / sr : 0.0;
    const float envStep = (float) (1.0 / (0.005 * sr));

    for (int i = 0; i < numSamples; ++i)
    {
        // 1. write input
        float mono = 0.0f;
        const size_t w = (size_t) (written & ringMask);
        for (int c = 0; c < chans; ++c)
        {
            const float x = channels[c][i];
            inRing[(size_t) c][w] = x;
            mono += x;
            inPeak = std::max (inPeak, std::abs (x));
        }
        for (int c = chans; c < numCh; ++c)
            inRing[(size_t) c][w] = 0.0f;

        monoRing[w] = mono / (float) chans;
        ++written;

        // 2. analysis
        if (--hopCountdown <= 0)
        {
            hopCountdown = hop;
            runDetection();
        }
        generateMarks();

        // 3. synthesis
        const int64_t outIndex = written - 1;
        placeGrains (outIndex);

        if (wetGain < wetTarget)      wetGain = std::min (wetTarget, wetGain + wetStep);
        else if (wetGain > wetTarget) wetGain = std::max (wetTarget, wetGain - wetStep);

        // reference tone
        float tone = 0.0f;
        if (toneWanted)          toneEnv = std::min (1.0f, toneEnv + envStep);
        else if (toneEnv > 0.0f) toneEnv = std::max (0.0f, toneEnv - envStep);
        if (toneEnv > 0.0f)
        {
            tone = toneAmp * toneEnv * (float) (std::sin (tonePhase) + 0.25 * std::sin (2.0 * tonePhase));
            tonePhase += toneInc;
            if (tonePhase > 2.0 * kPi)
                tonePhase -= 2.0 * kPi;
        }

        const size_t o = (size_t) (outIndex & ringMask);
        const size_t d = (size_t) ((outIndex - delay) & ringMask);
        for (int c = 0; c < chans; ++c)
        {
            float& acc = accRing[(size_t) c][o];
            const float wet = acc;
            acc = 0.0f;
            const float dry = outIndex >= delay ? inRing[(size_t) c][d] : 0.0f;
            const float y = dry + wetGain * (wet - dry) + tone;
            channels[c][i] = y;
            outPeak = std::max (outPeak, std::abs (y));
        }
        for (int c = chans; c < numCh; ++c)
            accRing[(size_t) c][o] = 0.0f;
    }

    storeMax (telemetry.inputPeak, inPeak);
    storeMax (telemetry.outputPeak, outPeak);
}

} // namespace claudetune
