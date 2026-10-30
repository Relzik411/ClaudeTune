// ClaudeTune capture/playback session tests. A fake DAW plays "regions" of
// audio on a timeline through EditSession, the way Logic would.

#include "Edit/EditSession.h"
#include "TestSignals.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <thread>

using namespace claudetune::edit;
using Audio = std::vector<std::vector<float>>;

static int failures = 0;

static void check (bool ok, const std::string& name, const std::string& detail = {})
{
    std::printf ("[%s] %s  %s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
    if (! ok) ++failures;
}

static std::string fmt (const char* f, double a = 0, double b = 0, double c = 0)
{
    char buf[256];
    std::snprintf (buf, sizeof buf, f, a, b, c);
    return buf;
}

// Plays timeline audio [from, to) through the session in blocks; returns the output.
static Audio play (EditSession& s, const Audio& timeline, int64_t from, int64_t to, int block = 512,
                   bool realtime = false, double inputGain = 1.0)
{
    const int nc = (int) timeline.size();
    Audio outAudio ((size_t) nc, std::vector<float> ((size_t) (to - from), 0.0f));
    std::vector<std::vector<float>> buf ((size_t) nc, std::vector<float> ((size_t) block));
    std::vector<float*> ptrs ((size_t) nc);
    for (int64_t pos = from; pos < to; pos += block)
    {
        const int n = (int) std::min<int64_t> (block, to - pos);
        for (int c = 0; c < nc; ++c)
        {
            for (int i = 0; i < n; ++i)
            {
                const int64_t t = pos + i;
                buf[(size_t) c][(size_t) i] = (t >= 0 && t < (int64_t) timeline[(size_t) c].size())
                                                  ? (float) (timeline[(size_t) c][(size_t) t] * inputGain) : 0.0f;
            }
            ptrs[(size_t) c] = buf[(size_t) c].data();
        }
        Transport tr;
        tr.playing = true;
        tr.hasPosition = true;
        tr.timeInSamples = pos;
        tr.realtime = realtime;
        s.process (ptrs.data(), nc, n, tr);
        for (int c = 0; c < nc; ++c)
            std::copy (buf[(size_t) c].begin(), buf[(size_t) c].begin() + n, outAudio[(size_t) c].begin() + (pos - from));
    }
    Transport stopped;               // the host stops
    s.process (ptrs.data(), nc, 0, stopped);
    return outAudio;
}

static double maxDiff (const std::vector<float>& a, const std::vector<float>& b, size_t from = 0, size_t to = SIZE_MAX)
{
    double d = 0;
    for (size_t i = from; i < std::min ({ to, a.size(), b.size() }); ++i)
        d = std::max (d, (double) std::abs (a[i] - b[i]));
    return d;
}

// median detected pitch over the middle 70% of [t0, t1) (seconds relative to the buffer)
static double measure (const std::vector<float>& audio, double sr, double t0, double t1)
{
    const auto tr = analyzePitch (audio.data(), (int64_t) audio.size(), sr);
    const double len = t1 - t0;
    std::vector<float> v;
    for (int f = (int) tr.secondsToFrame (t0 + 0.15 * len); f < (int) tr.secondsToFrame (t1 - 0.15 * len); ++f)
        if (tr.voiced (f)) v.push_back (tr.frames[(size_t) f].midi);
    if (v.empty()) return -1;
    std::sort (v.begin(), v.end());
    return v[v.size() / 2];
}

static std::array<bool, 128> cMajor()
{
    std::array<bool, 128> a {};
    for (int n = 0; n < 128; ++n)
    {
        const int pc = n % 12;
        a[(size_t) n] = pc == 0 || pc == 2 || pc == 4 || pc == 5 || pc == 7 || pc == 9 || pc == 11;
    }
    return a;
}

static bool onCMajorGrid (double m) { return m > 0 && std::abs (m - std::round (m)) < 0.08 && cMajor()[(size_t) std::lround (m)]; }

int main()
{
    const double offset = 1.5; // the vocal region starts 1.5 s into the song

    for (double sr : { 44100.0, 48000.0 })
    {
        const std::string tag = fmt (" @%gk", sr / 1000.0);
        const auto ph = testsig::makePhrase (sr);
        const int64_t off = (int64_t) (offset * sr);

        Audio timeline (1, std::vector<float> ((size_t) off, 0.0f));
        timeline[0].insert (timeline[0].end(), ph.audio.begin(), ph.audio.end());
        timeline[0].resize (timeline[0].size() + (size_t) (0.5 * sr), 0.0f);
        const auto end = (int64_t) timeline[0].size();

        EditSession s;
        s.prepare (sr, 1, 512);

        // ---- 1. first pass captures and passes audio through -----------------------
        {
            const auto o = play (s, timeline, 0, end);
            const bool idle = s.waitUntilIdle (30.0);
            std::this_thread::sleep_for (std::chrono::milliseconds (60));
            const auto snap = s.snapshot();
            check (idle && maxDiff (o[0], timeline[0]) == 0.0, "capture pass plays input untouched" + tag);
            check (snap->notes.size() == ph.notes.size() && snap->coverage.size() == 1
                       && snap->coverage[0] == SampleRange (0, end),
                   "capture analysed into notes" + tag,
                   fmt ("%g notes, %g coverage ranges", (double) snap->notes.size(), (double) snap->coverage.size()));
            check (s.status() == SessionStatus::Ready, "status ready after capture" + tag);
        }

        const uint64_t revAfterCapture = s.audioRevision();

        // ---- 2. an unedited pass is bit-identical and re-captures nothing -----------
        {
            const auto o = play (s, timeline, 0, end);
            s.waitUntilIdle (10.0);
            check (maxDiff (o[0], timeline[0]) == 0.0 && s.audioRevision() == revAfterCapture,
                   "unedited playback = input, nothing re-captured" + tag);
        }

        // ---- 3. Correct All, then playback is corrected -------------------------------
        Audio corrected;
        {
            s.edit ([] (EditDocument& d) { d.correctAll (cMajor(), 0.6f, 0.9f); });
            s.waitUntilIdle (10.0);
            corrected = play (s, timeline, 0, end);
            s.waitUntilIdle (10.0);
            int good = 0;
            std::string worst;
            for (const auto& n : s.snapshot()->notes)
            {
                const double m = measure (corrected[0], sr, n.outStart, n.outEnd);
                if (onCMajorGrid (m)) ++good;
                else worst += fmt ("%.2f ", m);
            }
            check (good == (int) ph.notes.size() && s.audioRevision() == revAfterCapture,
                   "edited playback is corrected" + tag, fmt ("%g/8 notes on the C major grid ", good) + worst);
        }

        // ---- 4. a fader/clip-gain change before the plug-in keeps the edits ------------
        {
            const auto o = play (s, timeline, 0, end, 512, false, 0.5);
            s.waitUntilIdle (10.0);
            std::vector<float> half = corrected[0];
            for (auto& v : half) v *= 0.5f;
            const double err = maxDiff (o[0], half, (size_t) (0.3 * sr));
            check (err < 2e-3 && s.audioRevision() == revAfterCapture, "input gain change keeps edits" + tag,
                   fmt ("max error vs 0.5 x edited = %.5f", err));
        }

        // ---- 5. starting playback in the middle of an edited note ----------------------
        {
            const auto n = s.snapshot()->notes[3];
            const auto from = (int64_t) (0.5 * (n.outStart + n.outEnd) * sr);
            const auto o = play (s, timeline, from, end, 333);
            std::vector<float> ref (corrected[0].begin() + from, corrected[0].end());
            const double err = maxDiff (o[0], ref);
            check (err < 1e-5, "start mid-note plays the edit immediately" + tag, fmt ("max diff %.2g", err));
        }

        // ---- 6. a muted stretch plays silence and keeps the edits ----------------------
        {
            Audio muted = timeline;
            const auto a = (int64_t) (3.0 * sr), b = (int64_t) (3.6 * sr);
            std::fill (muted[0].begin() + a, muted[0].begin() + b, 0.0f);
            const auto o = play (s, muted, 0, end);
            s.waitUntilIdle (10.0);
            double inside = 0;
            for (int64_t i = a + (int64_t) (0.01 * sr); i < b; ++i) inside = std::max (inside, (double) std::abs (o[0][(size_t) i]));
            const auto again = play (s, timeline, 0, end);
            const double err = maxDiff (again[0], corrected[0]);
            // (the edited note's first grain can spill ~1 ms ahead of its onset, far below -40 dB)
            check (inside < 0.01 && s.audioRevision() == revAfterCapture && err < 1e-5,
                   "muted region: silence, edits kept" + tag, fmt ("peak in mute %.2g, replay diff %.2g", inside, err));
        }

        // ---- 7. a re-take replaces only that stretch; it can be undone ------------------
        {
            const auto notesBefore = s.snapshot()->notes;
            Audio retake = timeline;
            const auto ra = (int64_t) ((offset + 2.40) * sr), rb = (int64_t) ((offset + 3.05) * sr);
            // a different performance: two notes a few semitones lower
            auto other = testsig::voice (sr, (double) (rb - ra) / sr,
                                         [] (double t) { return t < 0.3 ? 60.4 : 58.7; },
                                         [] (double t) { return std::min ({ 1.0, t / 0.015, (0.65 - t) / 0.02 }) * (t < 0.3 || t > 0.34 ? 1.0 : 0.05); });
            testsig::normalise (other, 0.5f);
            for (int64_t i = ra; i < rb; ++i) retake[0][(size_t) i] = other[(size_t) (i - ra)];

            const auto o = play (s, retake, 0, end);
            s.waitUntilIdle (10.0);
            const auto snap = s.snapshot();

            // output during the re-take: the new audio, untouched (after the 20 ms detection)
            const double err = maxDiff (o[0], retake[0], (size_t) (ra + (int64_t) (0.03 * sr)), (size_t) rb);

            int keptEdits = 0, freshInside = 0, inside = 0;
            for (const auto& n : snap->notes)
            {
                const bool in = n.srcEnd * sr > ra && n.srcStart * sr < rb;
                if (in) { ++inside; if (std::abs (n.targetPitch - n.detectedPitch) < 1e-6f) ++freshInside; }
                else if (std::abs (n.targetPitch - n.detectedPitch) > 1e-3f || n.center > 0.0f) ++keptEdits;
            }
            check (err < 1e-5 && inside == 2 && freshInside == 2, "re-take plays and re-captures the new audio" + tag,
                   fmt ("diff after detection %.2g, %g new notes (unedited: %g)", err, inside, freshInside));
            check (keptEdits == (int) notesBefore.size() - 2 && snap->hasBackup, "edits outside the re-take are kept" + tag,
                   fmt ("%g notes kept their edits", keptEdits));

            // corrected playback of the new notes after another Correct All
            s.edit ([] (EditDocument& d) { d.correctAll (cMajor(), 0.6f, 0.9f); });
            s.waitUntilIdle (10.0);
            const auto o2 = play (s, retake, 0, end);
            int good = 0;
            for (const auto& n : s.snapshot()->notes)
                if (n.srcEnd * sr > ra && n.srcStart * sr < rb && onCMajorGrid (measure (o2[0], sr, n.outStart, n.outEnd)))
                    ++good;
            check (good == 2, "re-taken notes can be edited" + tag);

            // bring the old take back
            s.restorePreviousCapture();
            s.waitUntilIdle (10.0);
            const auto restored = s.snapshot();
            const auto o3 = play (s, timeline, 0, end);
            s.waitUntilIdle (10.0);
            const double err3 = maxDiff (o3[0], corrected[0]);
            check (restored->notes.size() == notesBefore.size() && ! restored->hasBackup && err3 < 1e-5,
                   "restore previous capture brings back old audio + edits" + tag, fmt ("replay diff %.2g", err3));
        }

        // ---- 8. capture locked: a different take is played but never captured --------------
        {
            const auto rev = s.audioRevision();
            s.setCaptureLocked (true);
            Audio other = timeline;
            for (auto& v : other[0]) v *= -1.0f; // phase-inverted: definitely different
            play (s, other, 0, end);
            s.waitUntilIdle (10.0);
            s.setCaptureLocked (false);
            check (s.audioRevision() == rev, "locked capture ignores different audio" + tag);
        }

        // ---- 9. save and restore into a new session -----------------------------------
        {
            auto state = s.exportState();
            EditSession r;
            r.prepare (sr, 1, 512);
            r.importState (std::move (state));
            const bool idle = r.waitUntilIdle (30.0);
            const auto o = play (r, timeline, 0, end);
            r.waitUntilIdle (10.0);
            const double err = maxDiff (o[0], corrected[0]);
            check (idle && err < 1e-5 && r.snapshot()->notes.size() == s.snapshot()->notes.size()
                       && r.audioRevision() > 0,
                   "save + restore reproduces the edited playback" + tag, fmt ("max diff %.2g", err));
        }

        // ---- 10. timing edit plays back moved -------------------------------------------
        {
            const auto n = s.snapshot()->notes[4];
            s.edit ([&] (EditDocument& d) { d.moveNoteTime (n.id, -0.06); });
            s.waitUntilIdle (10.0);
            const auto o = play (s, timeline, 0, end);
            const auto tr = analyzePitch (o[0].data(), (int64_t) o[0].size(), sr);
            const auto found = segmentNotes (tr);
            double best = 1e9;
            for (const auto& d : found)
                best = std::min (best, std::abs (tr.frameToSeconds (d.startFrame) - (n.srcStart - 0.06)));
            check (best < 0.03, "timing edit is heard on playback" + tag, fmt ("onset error %.1f ms", 1000 * best));
            s.undo();
            s.waitUntilIdle (10.0);
        }

        // ---- 11. sample-rate change: passes audio through, keeps the capture ---------------
        {
            s.prepare (sr == 44100.0 ? 48000.0 : 44100.0, 1, 512);
            const auto o = play (s, timeline, 0, end);
            s.waitUntilIdle (5.0);
            const bool through = maxDiff (o[0], timeline[0]) == 0.0;
            check (through && s.status() == SessionStatus::SampleRateMismatch, "sample-rate change = safe pass-through" + tag);
            s.prepare (sr, 1, 512);
            const auto o2 = play (s, timeline, 0, end);
            check (maxDiff (o2[0], corrected[0]) < 1e-5, "edits come back at the original rate" + tag);
        }
    }

    const double sr = 48000.0;
    const auto ph = testsig::makePhrase (sr);

    // ---- 12. capture in two passes; the note across the join is whole; edits kept --------------
    {
        Audio timeline { ph.audio };
        const auto end = (int64_t) ph.audio.size();
        EditSession s;
        s.prepare (sr, 1, 512);
        const auto split = (int64_t) (1.95 * sr); // inside the 4th note
        play (s, timeline, 0, split);
        s.waitUntilIdle (10.0);
        const auto first = s.snapshot()->notes;
        s.edit ([&] (EditDocument& d) { d.setTargetPitch (first[0].id, first[0].detectedPitch + 1.0f); });
        s.waitUntilIdle (10.0);
        play (s, timeline, split - (int64_t) (0.2 * sr), end); // overlap a little, like a real pre-roll
        s.waitUntilIdle (10.0);
        const auto snap = s.snapshot();
        const bool editKept = ! snap->notes.empty() && snap->notes[0].id == first[0].id
                              && std::abs (snap->notes[0].targetPitch - snap->notes[0].detectedPitch - 1.0f) < 1e-4f;
        check (snap->notes.size() == ph.notes.size() && editKept && snap->coverage.size() == 1,
               "two-pass capture joins notes and keeps edits",
               fmt ("%g notes, first-note edit kept: %g", (double) snap->notes.size(), editKept ? 1.0 : 0.0));
    }

    // ---- 13. cycle/loop playback over a range --------------------------------------------------
    {
        Audio timeline { ph.audio };
        EditSession s;
        s.prepare (sr, 1, 256);
        for (int k = 0; k < 3; ++k)
            play (s, timeline, (int64_t) (1.0 * sr), (int64_t) (2.0 * sr), 256);
        s.waitUntilIdle (10.0);
        const auto snap = s.snapshot();
        check (snap->coverage.size() == 1 && snap->coverage[0] == SampleRange ((int64_t) (1.0 * sr), (int64_t) (2.0 * sr)),
               "looping a range captures it once", fmt ("%g notes in range", (double) snap->notes.size()));
    }

    // ---- 14. stereo, odd block sizes, offline bounce ---------------------------------------
    {
        Audio timeline { ph.audio, ph.audio };
        for (auto& v : timeline[1]) v *= 0.5f;
        const auto end = (int64_t) ph.audio.size();
        EditSession s;
        s.prepare (sr, 2, 4096);
        play (s, timeline, 0, end, 4096, false); // offline bounce-sized blocks
        s.waitUntilIdle (10.0);
        s.edit ([] (EditDocument& d) { d.correctAll (cMajor(), 1.0f, 0.5f); });
        s.waitUntilIdle (10.0);
        const auto o = play (s, timeline, 0, end, 37);
        std::vector<float> halfL = o[0];
        for (auto& v : halfL) v *= 0.5f;
        const double lr = maxDiff (halfL, o[1]);
        int good = 0;
        for (const auto& n : s.snapshot()->notes)
            if (onCMajorGrid (measure (o[0], sr, n.outStart, n.outEnd))) ++good;
        check (lr < 1e-5 && good == (int) ph.notes.size(), "stereo + 37-sample blocks + offline capture",
               fmt ("L/R mismatch %.2g, %g/8 notes corrected", lr, good));
    }

    // ---- 15. clear all, then re-capture ---------------------------------------------------
    {
        Audio timeline { ph.audio };
        EditSession s;
        s.prepare (sr, 1, 512);
        play (s, timeline, 0, (int64_t) ph.audio.size());
        s.waitUntilIdle (10.0);
        s.clearAll();
        s.waitUntilIdle (10.0);
        std::this_thread::sleep_for (std::chrono::milliseconds (60));
        const bool empty = s.snapshot()->notes.empty() && s.status() == SessionStatus::Empty;
        play (s, timeline, 0, (int64_t) ph.audio.size());
        s.waitUntilIdle (10.0);
        check (empty && s.snapshot()->notes.size() == ph.notes.size(), "clear all, then capture again");
    }

    // ---- 16. editing while the transport runs (thread safety) -------------------------------
    {
        Audio timeline { ph.audio };
        const auto end = (int64_t) ph.audio.size();
        EditSession s;
        s.prepare (sr, 1, 256);
        play (s, timeline, 0, end);
        s.waitUntilIdle (10.0);

        std::atomic<bool> stop { false };
        std::atomic<bool> finite { true };
        std::thread audio ([&] {
            while (! stop.load())
            {
                const auto o = play (s, timeline, 0, end, 256, true);
                for (auto v : o[0]) if (! std::isfinite (v) || std::abs (v) > 4.0f) finite = false;
            }
        });

        std::mt19937 rng (7);
        int edits = 0;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds (3);
        while (std::chrono::steady_clock::now() < until)
        {
            const auto snap = s.snapshot();
            if (snap->notes.empty()) continue;
            const auto& n = snap->notes[rng() % snap->notes.size()];
            const float delta = (float) ((int) (rng() % 5) - 2);
            switch (rng() % 5)
            {
                case 0: s.edit ([&] (EditDocument& d) { d.setTargetPitch (n.id, n.detectedPitch + delta); }); break;
                case 1: s.edit ([&] (EditDocument& d) { d.setNoteShape (n.id, 0.5f, 0.3f); }); break;
                case 2: s.edit ([&] (EditDocument& d) { d.moveNoteTime (n.id, 0.01 * delta); }); break;
                case 3: s.undo(); break;
                default: s.redo(); break;
            }
            ++edits;
            std::this_thread::sleep_for (std::chrono::milliseconds (15));
        }
        stop = true;
        audio.join();
        check (finite.load() && s.waitUntilIdle (10.0), "random edits during playback stay stable",
               fmt ("%g edits applied while playing", edits));
    }

    // ---- 17b. preview playback while the host is stopped ------------------------------------
    {
        Audio timeline { ph.audio };
        const auto end = (int64_t) ph.audio.size();
        EditSession s;
        s.prepare (sr, 1, 256);
        play (s, timeline, 0, end);
        s.waitUntilIdle (10.0);
        s.edit ([] (EditDocument& d) { d.correctAll (cMajor(), 0.8f, 0.5f); });
        s.waitUntilIdle (10.0);
        const auto edited = play (s, timeline, 0, end);

        // stopped host: blocks keep coming (Logic does this for the selected track), input is silence
        auto runStopped = [&] (int64_t samples, bool playing = false, int64_t hostPos = 0) {
            std::vector<float> out;
            std::vector<float> buf (256);
            float* ptr = buf.data();
            for (int64_t done = 0; done < samples; done += 256)
            {
                std::fill (buf.begin(), buf.end(), 0.0f);
                Transport tr;
                tr.playing = playing;
                tr.hasPosition = true;
                tr.timeInSamples = hostPos + done;
                s.process (&ptr, 1, 256, tr);
                out.insert (out.end(), buf.begin(), buf.end());
            }
            return out;
        };

        // preview a range: plays the edited audio (with 5 ms fades at its edges)
        const auto from = (int64_t) (1.0 * sr), to = (int64_t) (2.0 * sr);
        s.startPreview (from, to, false);
        const auto pv = runStopped (to - from + 2048);
        const auto fade = (int64_t) (0.006 * sr);
        double err = 0;
        for (int64_t i = fade; i < to - from - fade; ++i)
            err = std::max (err, (double) std::abs (pv[(size_t) i] - edited[0][(size_t) (from + i)]));
        double after = 0;
        for (size_t i = (size_t) (to - from); i < pv.size(); ++i) after = std::max (after, (double) std::abs (pv[i]));
        check (err < 1e-5 && after == 0.0 && ! s.isPreviewing(), "preview plays the edited audio from the cursor",
               fmt ("max diff %.2g, silent after the end", err));

        // looping a note while it is being edited: the loop follows the new pitch
        const auto n = s.snapshot()->notes[2];
        const auto na = (int64_t) (n.outStart * sr), nb = (int64_t) (n.outEnd * sr);
        s.startPreview (na, nb, true);
        runStopped (nb - na);
        s.edit ([&] (EditDocument& d) { d.setTargetPitch (n.id, n.targetPitch + 2.0f); });
        s.waitUntilIdle (5.0);
        const auto loop2 = runStopped (2 * (nb - na));
        const std::vector<float> second (loop2.begin() + (nb - na), loop2.end());
        const auto tr = analyzePitch (second.data(), (int64_t) second.size(), sr);
        std::vector<float> v;
        for (int f = (int) (0.2 * tr.numFrames()); f < (int) (0.8 * tr.numFrames()); ++f)
            if (tr.voiced (f)) v.push_back (tr.frames[(size_t) f].midi);
        std::sort (v.begin(), v.end());
        const double m = v.empty() ? -1 : v[v.size() / 2];
        check (s.isPreviewing() && std::abs (m - (n.targetPitch + 2.0)) < 0.08, "looped note preview follows a live edit",
               fmt ("heard %.2f, target %.2f", m, n.targetPitch + 2.0));

        // the host starting playback takes over immediately
        const auto hostOut = runStopped (2048, true, 0);
        check (! s.isPreviewing(), "host playback stops the preview");

        // stop fades out cleanly
        s.startPreview (from, to, true);
        runStopped (1024);
        s.stopPreview();
        const auto tail = runStopped ((int64_t) (0.02 * sr));
        double last = 0;
        for (size_t i = (size_t) (0.01 * sr); i < tail.size(); ++i) last = std::max (last, (double) std::abs (tail[i]));
        check (! s.isPreviewing() && last == 0.0, "stopping the preview fades out");

        // tempo reaches the UI
        Transport t;
        t.playing = true; t.hasPosition = true; t.timeInSamples = 96000; t.bpm = 120.0; t.hasPpq = true; t.ppqPosition = 4.0;
        t.timeSigNumerator = 3; t.timeSigDenominator = 4;
        std::vector<float> one (64, 0.0f);
        float* op = one.data();
        s.process (&op, 1, 64, t);
        const auto tempo = s.tempo();
        check (tempo.isValid() && std::abs (tempo.quartersAt (3.0, sr) - 6.0) < 1e-9 && tempo.quartersPerBar() == 3.0,
               "tempo and time signature reach the editor");
        (void) hostOut;
    }

    // ---- 17. audio-thread cost ----------------------------------------------------------------
    {
        Audio timeline { ph.audio };
        const auto end = (int64_t) ph.audio.size();
        EditSession s;
        s.prepare (sr, 1, 256);
        auto t0 = std::chrono::steady_clock::now();
        play (s, timeline, 0, end, 256);
        auto t1 = std::chrono::steady_clock::now();
        s.waitUntilIdle (10.0);
        s.edit ([] (EditDocument& d) { d.correctAll (cMajor(), 0.6f, 0.9f); });
        s.waitUntilIdle (10.0);
        auto t2 = std::chrono::steady_clock::now();
        for (int k = 0; k < 4; ++k) play (s, timeline, 0, end, 256);
        auto t3 = std::chrono::steady_clock::now();
        const double audioSec = (double) end / sr;
        const double capPct = 100.0 * std::chrono::duration<double> (t1 - t0).count() / audioSec;
        const double playPct = 100.0 * std::chrono::duration<double> (t3 - t2).count() / (4 * audioSec);
        std::printf ("[INFO] audio thread: capture pass %.3f%% of real time, edited playback %.3f%%\n", capPct, playPct);
        check (capPct < 2.0 && playPct < 2.0, "audio thread stays cheap");
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL SESSION TESTS PASSED" : "SOME SESSION TESTS FAILED",
                 failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
