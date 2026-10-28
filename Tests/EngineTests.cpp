// ClaudeTune engine tests - builds without JUCE:
//   g++ -O2 -std=c++17 -I../Source Tests/EngineTests.cpp Source/Engine/TuneEngine.cpp -o engine_tests
// Optionally pass an output folder to also render before/after demo WAVs.

#include "Engine/TuneEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

using namespace claudetune;

static constexpr double kPi = 3.14159265358979323846;
static int failures = 0;

static double midiToHz (double n, double ref = 440.0) { return ref * std::pow (2.0, (n - 69.0) / 12.0); }

// Additive "voice": harmonics shaped by three vowel formants, with a pitch
// contour given in MIDI note units as a function of time.
static std::vector<float> makeVoice (double sr, double seconds, const std::function<double (double)>& pitchAt,
                                     double formantScale = 1.0, float level = 0.3f)
{
    const int n = (int) (sr * seconds);
    std::vector<float> out ((size_t) n, 0.0f);
    const double formants[3] = { 730 * formantScale, 1090 * formantScale, 2440 * formantScale };
    const double widths[3] = { 90, 110, 160 };
    const double gains[3] = { 1.0, 0.5, 0.25 };

    std::vector<double> phases (80, 0.0);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        const double f0 = midiToHz (pitchAt (t));
        double v = 0.0;
        for (int h = 1; h < 80; ++h)
        {
            const double fh = f0 * h;
            if (fh > sr * 0.45)
                break;
            double env = 0.02;
            for (int k = 0; k < 3; ++k)
            {
                const double x = (fh - formants[k]) / widths[k];
                env += gains[k] / (1.0 + x * x);
            }
            phases[(size_t) h] += 2.0 * kPi * fh / sr;
            v += env * std::sin (phases[(size_t) h]) / std::sqrt ((double) h);
        }
        // short fades
        const double fade = std::min ({ 1.0, t / 0.02, (seconds - t) / 0.02 });
        out[(size_t) i] = (float) (v * fade);
    }

    float peak = 1e-9f;
    for (auto s : out) peak = std::max (peak, std::abs (s));
    for (auto& s : out) s *= level / peak;
    return out;
}

static std::vector<float> runEngine (const std::vector<float>& input, double sr, const EngineParams& p,
                                     const NoteStateArray& states, int* latencyOut = nullptr,
                                     const std::function<void (TuneEngine&, double)>& perBlock = {})
{
    TuneEngine engine;
    engine.prepare (sr, 512, 1);
    engine.setParams (p);
    engine.setNoteStates (states);
    if (latencyOut) *latencyOut = engine.getLatencySamples();

    std::vector<float> out = input;
    const int block = 256;
    for (size_t pos = 0; pos < out.size(); pos += block)
    {
        const int n = (int) std::min<size_t> (block, out.size() - pos);
        if (perBlock) perBlock (engine, pos / sr);
        float* ch[1] = { out.data() + pos };
        engine.process (ch, 1, n);
    }
    return out;
}

// Median detected pitch (MIDI units) of a region, plus per-frame values
static double measurePitch (const std::vector<float>& x, double sr, double t0, double t1,
                            std::vector<double>* frames = nullptr)
{
    PitchDetector det;
    const int tauMax = (int) (sr / 50.0);
    const int window = tauMax;
    det.prepare (window + tauMax + 8);
    std::vector<double> vals;
    for (int start = (int) (t0 * sr); start + window + tauMax < (int) (t1 * sr) && start + window + tauMax < (int) x.size();
         start += 256)
    {
        float ap = 1.0f;
        const float period = det.detect (x.data() + start, window, (int) (sr / 1500.0), tauMax, 0.1f, ap);
        if (period > 0 && ap < 0.2f)
            vals.push_back (69.0 + 12.0 * std::log2 ((sr / period) / 440.0));
    }
    if (frames) *frames = vals;
    if (vals.empty()) return -1.0;
    auto sorted = vals;
    std::sort (sorted.begin(), sorted.end());
    return sorted[sorted.size() / 2];
}

static void check (bool ok, const std::string& name, const std::string& detail)
{
    std::printf ("[%s] %s  %s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
    if (! ok) ++failures;
}

static bool finite (const std::vector<float>& x)
{
    for (auto v : x) if (! std::isfinite (v) || std::abs (v) > 4.0f) return false;
    return true;
}

static double stddev (const std::vector<double>& v)
{
    if (v.size() < 2) return 0.0;
    double m = 0; for (auto a : v) m += a; m /= v.size();
    double s = 0; for (auto a : v) s += (a - m) * (a - m);
    return std::sqrt (s / v.size());
}

static void writeWav (const std::string& path, const std::vector<float>& x, int sr)
{
    FILE* f = std::fopen (path.c_str(), "wb");
    if (! f) return;
    const int n = (int) x.size();
    const int dataBytes = n * 2;
    auto w32 = [&] (uint32_t v) { std::fwrite (&v, 4, 1, f); };
    auto w16 = [&] (uint16_t v) { std::fwrite (&v, 2, 1, f); };
    std::fwrite ("RIFF", 1, 4, f); w32 (36 + dataBytes); std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); w32 (16); w16 (1); w16 (1); w32 ((uint32_t) sr); w32 ((uint32_t) sr * 2); w16 (2); w16 (16);
    std::fwrite ("data", 1, 4, f); w32 ((uint32_t) dataBytes);
    for (auto v : x)
    {
        const int s = (int) std::lround (std::clamp (v, -1.0f, 1.0f) * 32767.0f);
        w16 ((uint16_t) (int16_t) s);
    }
    std::fclose (f);
}

int main (int argc, char** argv)
{
    const char* outDir = argc > 1 ? argv[1] : nullptr;

    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        char tag[32]; std::snprintf (tag, sizeof tag, "@%gk", sr / 1000.0);
        const auto chromatic = buildScale (0, 0);

        // 1. 30 cents sharp A4 is pulled to A4
        {
            auto in = makeVoice (sr, 1.5, [] (double) { return 69.3; });
            EngineParams p; p.speedMs = 0.1f;
            auto out = runEngine (in, sr, p, chromatic);
            const double m = measurePitch (out, sr, 0.5, 1.4);
            check (finite (out) && std::abs (m - 69.0) < 0.05, std::string ("A4 +30c -> A4 ") + tag,
                   "out=" + std::to_string ((m - 69.0) * 100) + "c");
        }

        // 2. C major: C#4 +10c resolves to D4 (nearest legal)
        {
            auto in = makeVoice (sr, 1.5, [] (double) { return 61.1; });
            EngineParams p; p.speedMs = 0.1f;
            auto out = runEngine (in, sr, p, buildScale (1, 0));
            const double m = measurePitch (out, sr, 0.5, 1.4);
            check (std::abs (m - 62.0) < 0.06, std::string ("C major snaps C#+10c to D ") + tag,
                   "out=" + std::to_string (m));
        }

        // 3. 50% correction: +40c -> +20c
        {
            auto in = makeVoice (sr, 1.5, [] (double) { return 69.4; });
            EngineParams p; p.speedMs = 0.1f; p.correctionAmount = 0.5f;
            auto out = runEngine (in, sr, p, chromatic);
            const double m = measurePitch (out, sr, 0.5, 1.4);
            check (std::abs (m - 69.2) < 0.05, std::string ("Correction 50% ") + tag,
                   "out=" + std::to_string ((m - 69.0) * 100) + "c");
        }

        // 4. Bypass note passes untouched
        {
            auto states = chromatic; states[69] = NoteState::Bypass;
            auto in = makeVoice (sr, 1.5, [] (double) { return 69.3; });
            EngineParams p; p.speedMs = 0.1f;
            auto out = runEngine (in, sr, p, states);
            const double m = measurePitch (out, sr, 0.5, 1.4);
            check (std::abs (m - 69.3) < 0.05, std::string ("Bypass note untouched ") + tag,
                   "out=" + std::to_string ((m - 69.0) * 100) + "c");
        }

        // 5. Illegal-up note: A4 marked IllegalUp goes to A#4
        {
            auto states = chromatic; states[69] = NoteState::IllegalUp;
            auto in = makeVoice (sr, 1.5, [] (double) { return 69.1; });
            EngineParams p; p.speedMs = 0.1f;
            auto out = runEngine (in, sr, p, states);
            const double m = measurePitch (out, sr, 0.5, 1.4);
            check (std::abs (m - 70.0) < 0.06, std::string ("Illegal-up redirects ") + tag, "out=" + std::to_string (m));
        }

        // 6. Vibrato on, depth 100%: centre corrected, vibrato kept
        {
            auto vib = [] (double t) { return 69.3 + 0.4 * std::sin (2 * kPi * 5.5 * t); };
            auto in = makeVoice (sr, 2.0, vib);
            EngineParams p; p.speedMs = 0.1f; p.transitionMs = 0.1f; p.vibratoOn = true; p.vibratoDepth = 1.0f;
            auto out = runEngine (in, sr, p, chromatic);
            std::vector<double> fin, fout;
            measurePitch (in, sr, 0.6, 1.9, &fin);
            const double m = measurePitch (out, sr, 0.6, 1.9, &fout);
            double mean = 0; for (auto v : fout) mean += v; mean /= std::max<size_t> (1, fout.size());
            const double ratio = stddev (fout) / std::max (1e-6, stddev (fin));
            check (std::abs (mean - 69.0) < 0.06 && ratio > 0.75 && ratio < 1.25,
                   std::string ("Vibrato preserved ") + tag,
                   "centre=" + std::to_string ((mean - 69) * 100) + "c depthRatio=" + std::to_string (ratio) + " med=" + std::to_string (m));

            p.vibratoDepth = 0.0f;
            auto flat = runEngine (in, sr, p, chromatic);
            measurePitch (flat, sr, 0.6, 1.9, &fout);
            check (stddev (fout) < 0.4 * stddev (fin), std::string ("Vibrato 0% flattens ") + tag,
                   "sd in=" + std::to_string (stddev (fin)) + " out=" + std::to_string (stddev (fout)));
        }

        // 7. Correction off = clean dry signal delayed by the reported latency
        {
            auto in = makeVoice (sr, 1.0, [] (double) { return 69.3; });
            EngineParams p; p.correctionOn = false;
            int lat = 0;
            auto out = runEngine (in, sr, p, chromatic, &lat);
            double err = 0;
            for (size_t i = (size_t) lat; i < in.size(); ++i)
                err = std::max (err, (double) std::abs (out[i] - in[i - (size_t) lat]));
            check (err < 1e-6, std::string ("Bypass = delayed dry ") + tag,
                   "latency=" + std::to_string (lat) + " (" + std::to_string (1000.0 * lat / sr) + " ms) err=" + std::to_string (err));
        }

        // 8. Formant non-corrected still lands on pitch
        {
            auto in = makeVoice (sr, 1.5, [] (double) { return 66.4; });
            EngineParams p; p.speedMs = 0.1f; p.formantCorrected = false;
            auto out = runEngine (in, sr, p, chromatic);
            const double m = measurePitch (out, sr, 0.5, 1.4);
            check (finite (out) && std::abs (m - 66.0) < 0.06, std::string ("Formant non-corrected ") + tag,
                   "out=" + std::to_string (m));
        }

        // 9. Bass (A2 +40c) and soprano (A5 -40c) with matching ranges
        {
            auto in = makeVoice (sr, 1.5, [] (double) { return 45.4; });
            EngineParams p; p.speedMs = 0.1f; p.rangeLow = 36; p.rangeHigh = 64;
            auto out = runEngine (in, sr, p, chromatic);
            const double m = measurePitch (out, sr, 0.5, 1.4);
            check (std::abs (m - 45.0) < 0.06, std::string ("Bass A2 ") + tag, "out=" + std::to_string (m));

            auto in2 = makeVoice (sr, 1.5, [] (double) { return 80.6; });
            p.rangeLow = 59; p.rangeHigh = 84;
            auto out2 = runEngine (in2, sr, p, chromatic);
            const double m2 = measurePitch (out2, sr, 0.5, 1.4);
            check (std::abs (m2 - 81.0) < 0.06, std::string ("Soprano A5 ") + tag, "out=" + std::to_string (m2));
        }

        // 10. Out of range notes are left alone
        {
            auto in = makeVoice (sr, 1.5, [] (double) { return 50.3; });
            EngineParams p; p.speedMs = 0.1f; p.rangeLow = 59; p.rangeHigh = 84;
            auto out = runEngine (in, sr, p, chromatic);
            const double m = measurePitch (out, sr, 0.5, 1.4);
            check (std::abs (m - 50.3) < 0.06, std::string ("Out of range untouched ") + tag, "out=" + std::to_string (m));
        }

        // 11. MIDI target pitch: sing A4, hold C5 -> C5
        {
            auto in = makeVoice (sr, 1.5, [] (double) { return 69.2; });
            EngineParams p; p.speedMs = 0.1f; p.transitionMs = 20.0f; p.targetPitchOn = true;
            auto out = runEngine (in, sr, p, chromatic, nullptr,
                                  [] (TuneEngine& e, double t) { if (t < 0.01) e.noteOn (72); });
            const double m = measurePitch (out, sr, 0.6, 1.4);
            check (std::abs (m - 72.0) < 0.06, std::string ("MIDI target pitch ") + tag, "out=" + std::to_string (m));
        }

        // 12. Tolerance cents keeps a borderline note on the old target
        {
            // A4 for 0.6s, then drift to A4 + 58c (would normally flip to A#4)
            auto contour = [] (double t) { return t < 0.6 ? 69.0 : 69.58; };
            auto in = makeVoice (sr, 1.5, contour);
            EngineParams p; p.speedMs = 0.1f; p.transitionMs = 0.1f; p.toleranceCents = 20.0f;
            auto out = runEngine (in, sr, p, chromatic);
            const double m = measurePitch (out, sr, 0.9, 1.4);
            p.toleranceCents = 0.0f;
            auto out2 = runEngine (in, sr, p, chromatic);
            const double m2 = measurePitch (out2, sr, 0.9, 1.4);
            check (std::abs (m - 69.0) < 0.06 && std::abs (m2 - 70.0) < 0.06, std::string ("Tolerance cents ") + tag,
                   "with=" + std::to_string (m) + " without=" + std::to_string (m2));
        }

        // 13. Tolerance time: short excursion ignored, long one accepted
        {
            auto contour = [] (double t) { return (t > 0.6 && t < 0.66) ? 70.0 : (t >= 1.0 ? 70.0 : 69.0); };
            auto in = makeVoice (sr, 1.6, contour);
            EngineParams p; p.speedMs = 0.1f; p.transitionMs = 0.1f; p.toleranceTimeMs = 120.0f;
            auto states = buildScale (0, 0);
            // make A#4 illegal so a real jump is needed to change target only via A4->B4
            auto out = runEngine (in, sr, p, states);
            std::vector<double> fr;
            const double during = measurePitch (out, sr, 0.62, 0.70);
            const double late = measurePitch (out, sr, 1.3, 1.55);
            check (during < 69.6 && std::abs (late - 70.0) < 0.06, std::string ("Tolerance time ") + tag,
                   "excursion=" + std::to_string (during) + " sustained=" + std::to_string (late));
        }

        // 14. Stereo processing stays in sync
        {
            auto in = makeVoice (sr, 1.0, [] (double) { return 64.3; });
            TuneEngine e; e.prepare (sr, 512, 2);
            EngineParams p; p.speedMs = 0.1f; e.setParams (p);
            std::vector<float> L = in, R = in;
            for (size_t pos = 0; pos < in.size(); pos += 256)
            {
                const int n = (int) std::min<size_t> (256, in.size() - pos);
                float* ch[2] = { L.data() + pos, R.data() + pos };
                e.process (ch, 2, n);
            }
            double diff = 0; for (size_t i = 0; i < L.size(); ++i) diff = std::max (diff, (double) std::abs (L[i] - R[i]));
            check (diff < 1e-6, std::string ("Stereo identical channels ") + tag, "maxdiff=" + std::to_string (diff));
        }
    }

    // Performance: 60 s of audio at 48 kHz stereo
    {
        const double sr = 48000.0;
        auto in = makeVoice (sr, 4.0, [] (double t) { return 60 + 7 * std::sin (t) + 0.3 * std::sin (30 * t); });
        TuneEngine e; e.prepare (sr, 512, 2);
        EngineParams p; e.setParams (p);
        std::vector<float> L (512), R (512);
        auto t0 = std::clock();
        size_t total = 0;
        for (int rep = 0; rep < 15; ++rep)
            for (size_t pos = 0; pos + 512 <= in.size(); pos += 512)
            {
                std::memcpy (L.data(), in.data() + pos, 512 * 4);
                std::memcpy (R.data(), in.data() + pos, 512 * 4);
                float* ch[2] = { L.data(), R.data() };
                e.process (ch, 2, 512);
                total += 512;
            }
        const double secs = double (std::clock() - t0) / CLOCKS_PER_SEC;
        const double audio = total / sr;
        std::printf ("[INFO] CPU: %.2f s for %.1f s audio (%.2f%% of one core)\n", secs, audio, 100.0 * secs / audio);
    }

    if (outDir)
    {
        const double sr = 44100.0;
        // A wobbly little melody: C D E G E D C, each note slightly off with scoops and drift
        const double notes[] = { 60.35, 62.0 - 0.3, 64.25, 67.4, 63.75, 62.3, 59.7 };
        auto melody = [&] (double t) {
            const double dur = 0.55;
            int i = std::min (6, (int) (t / dur));
            const double local = t - i * dur;
            const double scoop = -0.6 * std::exp (-local / 0.05);
            const double drift = 0.15 * std::sin (2 * kPi * 0.9 * t);
            const double vib = local > 0.25 ? 0.25 * std::sin (2 * kPi * 5.3 * t) : 0.0;
            return notes[i] + scoop + drift + vib;
        };
        auto in = makeVoice (sr, 0.55 * 7, melody);
        writeWav (std::string (outDir) + "/demo_1_original.wav", in, (int) sr);

        EngineParams natural; natural.vibratoOn = true; // default speed/transition
        int lat = 0;
        auto out = runEngine (in, sr, natural, buildScale (1, 0), &lat);
        std::vector<float> aligned (out.begin() + lat, out.end());
        writeWav (std::string (outDir) + "/demo_2_natural_C_major.wav", aligned, (int) sr);

        EngineParams hard; hard.speedMs = 0.1f; hard.transitionMs = 0.1f;
        auto out2 = runEngine (in, sr, hard, buildScale (1, 0));
        std::vector<float> aligned2 (out2.begin() + lat, out2.end());
        writeWav (std::string (outDir) + "/demo_3_hard_tune_C_major.wav", aligned2, (int) sr);
        std::printf ("[INFO] wrote demo WAVs to %s\n", outDir);
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED",
                 failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
