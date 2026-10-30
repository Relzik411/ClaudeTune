// ClaudeTune editor engine tests (no JUCE needed).
// Pass a folder to also write before/after demo WAVs.

#include "Edit/EditModel.h"
#include "Edit/Renderer.h"
#include "TestSignals.h"

#include <chrono>
#include <cstdio>
#include <string>

using namespace claudetune::edit;

static int failures = 0;

static void check (bool ok, const std::string& name, const std::string& detail = {})
{
    std::printf ("[%s] %s  %s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
    if (! ok) ++failures;
}

static std::string fmt (const char* f, double a, double b = 0, double c = 0)
{
    char buf[256];
    std::snprintf (buf, sizeof buf, f, a, b, c);
    return buf;
}

// median pitch of rendered audio between two output times (middle 70%)
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

static double spread (const std::vector<float>& audio, double sr, double t0, double t1)
{
    const auto tr = analyzePitch (audio.data(), (int64_t) audio.size(), sr);
    const double len = t1 - t0;
    std::vector<double> v;
    for (int f = (int) tr.secondsToFrame (t0 + 0.2 * len); f < (int) tr.secondsToFrame (t1 - 0.1 * len); ++f)
        if (tr.voiced (f)) v.push_back (tr.frames[(size_t) f].midi);
    if (v.size() < 2) return 0;
    double m = 0; for (auto x : v) m += x; m /= v.size();
    double s = 0; for (auto x : v) s += (x - m) * (x - m);
    return std::sqrt (s / v.size());
}

// detected note blobs of rendered audio
static std::vector<DetectedNote> notesOf (const std::vector<float>& audio, double sr, PitchTrack* trackOut = nullptr)
{
    auto tr = analyzePitch (audio.data(), (int64_t) audio.size(), sr);
    auto n = segmentNotes (tr);
    if (trackOut) *trackOut = tr;
    return n;
}

struct Session
{
    EditDocument doc;
    Renderer renderer;
    std::vector<std::vector<float>> out;
    RenderCurves curves;

    void load (const std::vector<float>& mono, double sr)
    {
        doc.load ({ mono }, sr);
        renderer.prepare (doc);
        curves = doc.buildCurves();
        renderer.renderAll (doc, curves, out);
    }

    std::vector<SampleRange> apply()
    {
        auto next = doc.buildCurves();
        auto changed = renderer.update (doc, curves, next, out);
        curves = std::move (next);
        return changed;
    }
};

int main (int argc, char** argv)
{
    const char* outDir = argc > 1 ? argv[1] : nullptr;

    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        const std::string tag = fmt (" @%gk", sr / 1000.0);
        const auto ph = testsig::makePhrase (sr);

        Session s;
        s.load (ph.audio, sr);
        const auto notes = s.doc.notes(); // copy: the originals

        // --- analysis & segmentation -----------------------------------------
        {
            bool ok = notes.size() == ph.notes.size();
            double worstStart = 0, worstEnd = 0, worstPitch = 0;
            if (ok)
                for (size_t i = 0; i < notes.size(); ++i)
                {
                    worstStart = std::max (worstStart, std::abs (notes[i].srcStart - ph.notes[i].start));
                    worstEnd = std::max (worstEnd, std::abs (notes[i].srcEnd - ph.notes[i].end));
                    worstPitch = std::max (worstPitch, (double) std::abs (notes[i].detectedPitch - (float) ph.notes[i].pitch));
                }
            check (ok && worstStart < 0.045 && worstEnd < 0.045 && worstPitch < 0.2, "notes found" + tag,
                   fmt ("count=%g worst start/end err %.0f ms, pitch err %.2f st", (double) notes.size(), 1000 * std::max (worstStart, worstEnd), worstPitch));
        }

        // --- untouched = bit-identical -----------------------------------------
        {
            double err = 0;
            for (size_t i = 0; i < ph.audio.size(); ++i)
                err = std::max (err, (double) std::abs (s.out[0][i] - ph.audio[i]));
            check (err == 0.0 && s.renderer.editedRegions (s.curves).empty(), "no edits = bit-identical output" + tag);
        }

        // --- move one note's pitch ---------------------------------------------
        {
            const auto id = notes[2].id;
            const float before = notes[2].detectedPitch;
            s.doc.beginEdit();
            s.doc.setTargetPitch (id, before + 2.0f);
            const auto changed = s.apply();

            const auto* n = s.doc.findNote (id);
            const double m = measure (s.out[0], sr, n->outStart, n->outEnd);
            const double other = measure (s.out[0], sr, notes[5].outStart, notes[5].outEnd);
            // far-away audio must be untouched
            double far = 0;
            for (size_t i = (size_t) (2.5 * sr); i < ph.audio.size(); ++i)
                far = std::max (far, (double) std::abs (s.out[0][i] - ph.audio[i]));
            int64_t touched = 0;
            for (auto& r : changed) touched += r.second - r.first;
            check (std::abs (m - (before + 2.0)) < 0.06 && std::abs (other - notes[5].detectedPitch) < 0.06 && far == 0.0,
                   "pitch move +2 st" + tag,
                   fmt ("moved note=%+.3f st vs target, re-rendered %.2f s", m - (before + 2.0), touched / sr));
        }

        // --- undo restores the original exactly ------------------------------
        {
            s.doc.undo();
            s.apply();
            double err = 0;
            for (size_t i = 0; i < ph.audio.size(); ++i)
                err = std::max (err, (double) std::abs (s.out[0][i] - ph.audio[i]));
            s.doc.redo();
            s.apply();
            const double m = measure (s.out[0], sr, notes[2].outStart, notes[2].outEnd);
            check (err == 0.0 && std::abs (m - (notes[2].detectedPitch + 2.0)) < 0.06, "undo / redo" + tag,
                   fmt ("undo err=%g, redo pitch=%+.3f", err, m - (notes[2].detectedPitch + 2.0)));
            s.doc.undo();
            s.apply();
        }

        // --- center + variation flatten a wobbly note ---------------------------
        {
            const auto& n = notes[3]; // long note with drift + vibrato
            const double sdBefore = spread (ph.audio, sr, n.srcStart, n.srcEnd);
            s.doc.beginEdit();
            s.doc.setTargetPitch (n.id, std::round (n.detectedPitch));
            s.doc.setNoteShape (n.id, 1.0f, 0.0f);
            s.apply();
            const double sdAfter = spread (s.out[0], sr, n.outStart, n.outEnd);
            const double m = measure (s.out[0], sr, n.outStart, n.outEnd);
            check (sdAfter < 0.25 * sdBefore && std::abs (m - std::round (n.detectedPitch)) < 0.05,
                   "center 100% + variation 0% flattens" + tag,
                   fmt ("spread %.3f -> %.3f st, pitch err %+.3f", sdBefore, sdAfter, m - std::round (n.detectedPitch)));

            // variation 1, center 0: keep the wobble, only shift
            s.doc.setNoteShape (n.id, 0.0f, 1.0f);
            s.apply();
            const double sdKeep = spread (s.out[0], sr, n.outStart, n.outEnd);
            check (std::abs (sdKeep - sdBefore) < 0.25 * sdBefore, "variation 100% keeps vibrato" + tag,
                   fmt ("spread %.3f vs original %.3f", sdKeep, sdBefore));

            // variation 2 exaggerates
            s.doc.setNoteShape (n.id, 0.0f, 2.0f);
            s.apply();
            const double sdMore = spread (s.out[0], sr, n.outStart, n.outEnd);
            check (sdMore > 1.5 * sdBefore, "variation 200% exaggerates" + tag, fmt ("spread %.3f vs %.3f", sdMore, sdBefore));
            s.doc.undo();
            s.apply();
        }

        // --- correct all to C major ------------------------------------------
        {
            std::array<bool, 128> cMajor {};
            for (int n = 0; n < 128; ++n)
            {
                const int pc = n % 12;
                cMajor[(size_t) n] = pc == 0 || pc == 2 || pc == 4 || pc == 5 || pc == 7 || pc == 9 || pc == 11;
            }
            s.doc.beginEdit();
            s.doc.correctAll (cMajor, 0.7f, 0.8f);
            s.apply();
            double worst = 0;
            for (const auto& n : s.doc.notes())
            {
                const double m = measure (s.out[0], sr, n.outStart, n.outEnd);
                const int pc = ((int) std::lround (m)) % 12;
                const bool inScale = cMajor[(size_t) std::lround (m)];
                (void) pc;
                worst = std::max (worst, std::abs (m - std::round (m)) + (inScale ? 0.0 : 1.0));
            }
            check (worst < 0.08, "correct all -> C major" + tag, fmt ("worst note off scale grid by %.3f st", worst));
            s.doc.undo();
            s.apply();
        }

        // --- timing: move a note earlier ---------------------------------------
        {
            const auto n = notes[4]; // ~95 ms gap before it
            const double delta = -0.06;
            s.doc.beginEdit();
            const double applied = s.doc.moveNoteTime (n.id, delta);
            s.apply();
            const auto found = notesOf (s.out[0], sr);
            double bestErr = 1e9, pitchErr = 0, lenErr = 0;
            PitchTrack ot;
            for (const auto& d : notesOf (s.out[0], sr, &ot))
            {
                const double st = ot.frameToSeconds (d.startFrame), en = ot.frameToSeconds (d.endFrame);
                if (std::abs (st - (n.srcStart + delta)) < bestErr)
                {
                    bestErr = std::abs (st - (n.srcStart + delta));
                    pitchErr = d.centre - n.detectedPitch;
                    lenErr = (en - st) - (n.srcEnd - n.srcStart);
                }
            }
            (void) found;
            check (std::abs (applied - delta) < 1e-9 && bestErr < 0.03 && std::abs (pitchErr) < 0.1 && std::abs (lenErr) < 0.03,
                   "move note 60 ms earlier" + tag,
                   fmt ("onset err %.1f ms, length err %.1f ms, pitch err %.3f", 1000 * bestErr, 1000 * lenErr, pitchErr));
            s.doc.undo();
            s.apply();
        }

        // --- timing: stretch a note to 1.5x ------------------------------------
        {
            const auto n = notes[3];
            const double len = n.srcEnd - n.srcStart;
            s.doc.beginEdit();
            // gap after note 3 is ~0.1 s, so stretch the start earlier into the previous gap
            const bool ok = s.doc.setNoteOutEdges (n.id, n.outStart - 0.05, n.outEnd + 0.08);
            s.apply();
            const auto* e = s.doc.findNote (n.id);
            PitchTrack ot;
            double gotLen = 0, gotPitch = 0, best = 1e9;
            for (const auto& d : notesOf (s.out[0], sr, &ot))
            {
                const double st = ot.frameToSeconds (d.startFrame);
                if (std::abs (st - e->outStart) < best)
                {
                    best = std::abs (st - e->outStart);
                    gotLen = ot.frameToSeconds (d.endFrame) - st;
                    gotPitch = d.centre;
                }
            }
            const double wantLen = e->outEnd - e->outStart;
            check (ok && std::abs (gotLen - wantLen) < 0.035 && std::abs (gotPitch - n.detectedPitch) < 0.1,
                   "stretch note" + tag,
                   fmt ("length %.0f -> %.0f ms (wanted %.0f)", 1000 * len, 1000 * gotLen, 1000 * wantLen));
            s.doc.undo();
            s.apply();
        }

        // --- legato neighbour follows a timing move ---------------------------
        {
            const auto a = notes[1], b = notes[2]; // joined by a legato slide
            s.doc.beginEdit();
            s.doc.moveNoteTime (b.id, -0.05);
            const auto* na = s.doc.findNote (a.id);
            const auto* nb = s.doc.findNote (b.id);
            check (std::abs (na->outEnd - nb->outStart) < 1e-9 && na->outEnd < a.outEnd,
                   "legato neighbour shortens to follow" + tag);
            s.apply();
            bool finiteOk = true;
            for (auto v : s.out[0]) if (! std::isfinite (v) || std::abs (v) > 2.0f) finiteOk = false;
            check (finiteOk, "output stays finite after timing edits" + tag);
            s.doc.undo();
            s.apply();
        }

        // --- clamping: can't push through a neighbour ---------------------------
        {
            const auto n = notes[4];
            s.doc.beginEdit();
            const double applied = s.doc.moveNoteTime (n.id, -1.0);
            const auto* prev = s.doc.findNote (notes[3].id);
            const auto* moved = s.doc.findNote (n.id);
            check (applied > -1.0 && moved->outStart >= prev->outEnd - 1e-12, "time move clamps at neighbour" + tag,
                   fmt ("applied %.3f s", applied));
            s.doc.undo();
        }

        // --- split and merge ----------------------------------------------------
        {
            const auto n = notes[3];
            s.doc.beginEdit();
            const auto second = s.doc.splitNote (n.id, 0.5 * (n.srcStart + n.srcEnd));
            const bool splitOk = second != 0 && s.doc.notes().size() == notes.size() + 1;
            const auto merged = s.doc.mergeNotes (n.id, second);
            const auto* m = s.doc.findNote (merged);
            check (splitOk && m && std::abs (m->srcEnd - n.srcEnd) < 1e-9 && s.doc.notes().size() == notes.size(),
                   "split then merge" + tag);
            s.doc.undo();
        }

        // --- gain ----------------------------------------------------------------
        {
            const auto n = notes[0];
            s.doc.beginEdit();
            s.doc.setGain (n.id, 6.0f);
            s.apply();
            double e0 = 0, e1 = 0;
            for (size_t i = (size_t) ((n.srcStart + 0.1) * sr); i < (size_t) ((n.srcEnd - 0.1) * sr); ++i)
            {
                e0 += ph.audio[i] * ph.audio[i];
                e1 += s.out[0][i] * s.out[0][i];
            }
            const double ratioDb = 10 * std::log10 (e1 / e0);
            check (std::abs (ratioDb - 6.0) < 0.5, "gain +6 dB" + tag, fmt ("measured %+.2f dB", ratioDb));
            s.doc.undo();
            s.apply();
        }

        // --- pencil: a hand-drawn line is what you hear; its ends blend in ---------
        {
            const auto n = notes[3]; // long note with vibrato
            const auto& tr = s.doc.pitchTrack();
            const int fa = (int) std::lround (tr.secondsToFrame (n.srcStart + 0.10));
            const int fb = (int) std::lround (tr.secondsToFrame (n.srcEnd - 0.10));
            // a straight rising line, a whole tone above the sung pitch
            std::vector<float> line;
            for (int f = fa; f < fb; ++f)
                line.push_back (n.detectedPitch + 2.0f + 0.5f * (float) (f - fa) / (float) (fb - fa));
            s.doc.beginEdit();
            s.doc.setDrawnPitch (fa, line);
            s.apply();

            // the rendered pitch follows the line in the middle of the stroke
            const auto otr = analyzePitch (s.out[0].data(), (int64_t) s.out[0].size(), sr);
            double worst = 0;
            int counted = 0;
            for (int f = fa + 8; f < fb - 8; ++f)
                if (otr.voiced (f))
                {
                    worst = std::max (worst, (double) std::abs (otr.frames[(size_t) f].midi - line[(size_t) (f - fa)]));
                    ++counted;
                }
            // and the shift never jumps more than ~0.6 st between 5 ms frames at the stroke ends
            const auto cv = s.doc.buildCurves();
            double jump = 0;
            for (int f = fa - 6; f < fa + 6; ++f) jump = std::max (jump, (double) std::abs (cv.shift[(size_t) f + 1] - cv.shift[(size_t) f]));
            for (int f = fb - 6; f < fb + 6; ++f) jump = std::max (jump, (double) std::abs (cv.shift[(size_t) f + 1] - cv.shift[(size_t) f]));
            check (counted > 40 && worst < 0.08 && jump < 0.6, "pencil-drawn pitch is rendered" + tag,
                   fmt ("max error %.3f st over %g frames, largest step at the stroke ends %.2f st", worst, counted, jump));

            s.doc.clearDrawnPitch (fa, fb);
            s.apply();
            double back = 0;
            for (size_t i = 0; i < ph.audio.size(); ++i) back = std::max (back, (double) std::abs (s.out[0][i] - ph.audio[i]));
            check (back == 0.0, "erasing the drawing restores the original" + tag);
            s.doc.undo(); s.doc.undo();
            s.apply();
        }

        // --- glide in: a longer transition reaches the new pitch later -------------
        {
            const auto n = notes[0]; // starts from silence, scoops up
            auto pitchAt = [&] (double t) {
                const auto tr2 = analyzePitch (s.out[0].data(), (int64_t) s.out[0].size(), sr);
                return tr2.frames[(size_t) std::lround (tr2.secondsToFrame (t))].midi;
            };
            s.doc.beginEdit();
            s.doc.setTargetPitch (n.id, n.detectedPitch + 3.0f);
            s.doc.setTransitions (n.id, 0.0f, 30.0f);
            s.apply();
            const float early = pitchAt (n.srcStart + 0.06);
            s.doc.setTransitions (n.id, 250.0f, 30.0f);
            s.apply();
            const float earlySlow = pitchAt (n.srcStart + 0.06);
            check (early - earlySlow > 0.5f, "per-note glide in delays the pitch change" + tag,
                   fmt ("pitch 60 ms in: %.2f (no glide) vs %.2f (250 ms glide)", early, earlySlow));
            s.doc.undo();
            s.apply();
        }

        // --- merge keeps the first note's edits; split keeps them on both halves ----
        {
            const auto a = notes[1], b = notes[2];
            s.doc.beginEdit();
            s.doc.setTargetPitch (a.id, a.detectedPitch + 1.0f);
            s.doc.setNoteShape (a.id, 0.7f, 0.4f);
            s.doc.setGain (a.id, -3.0f);
            const auto keep = s.doc.mergeNotes (a.id, b.id);
            const auto* m = s.doc.findNote (keep);
            const bool mergedOk = m && std::abs (m->center - 0.7f) < 1e-6f && std::abs (m->gainDb + 3.0f) < 1e-6f
                                  && std::abs (m->srcEnd - b.srcEnd) < 1e-9;
            const auto second = s.doc.splitNote (keep, b.srcStart);
            const auto* h1 = s.doc.findNote (keep);
            const auto* h2 = s.doc.findNote (second);
            const bool splitOk = h1 && h2 && std::abs (h2->center - 0.7f) < 1e-6f && std::abs (h2->gainDb + 3.0f) < 1e-6f
                                 && std::abs ((h2->targetPitch - h2->detectedPitch) - (h1->targetPitch - h1->detectedPitch)) < 1e-4f;
            check (mergedOk && splitOk, "merge and split carry the note's edits" + tag);
            s.doc.undo();
            s.apply();
        }

        // --- group time move: a legato pair moves as one block ----------------------
        {
            const auto a = notes[1], b = notes[2], after = notes[3];
            s.doc.beginEdit();
            const double d = s.doc.moveNotesTime ({ a.id, b.id }, 0.05);
            s.apply();
            const auto* na = s.doc.findNote (a.id);
            const auto* nb = s.doc.findNote (b.id);
            const auto* nc = s.doc.findNote (after.id);
            const bool rigid = std::abs ((na->outStart - a.outStart) - d) < 1e-9 && std::abs ((nb->outEnd - b.outEnd) - d) < 1e-9
                               && std::abs (na->outEnd - nb->outStart) < 1e-9
                               && std::abs ((na->outEnd - na->outStart) - (a.outEnd - a.outStart)) < 1e-9;
            const bool neighbourOk = std::abs (nc->outStart - after.outStart) < 1e-9 && nb->outEnd <= nc->outStart + 1e-9;
            // the moved notes still sound at their pitch, now later
            const double ma = measure (s.out[0], sr, na->outStart, na->outEnd);
            const double mb = measure (s.out[0], sr, nb->outStart, nb->outEnd);
            check (std::abs (d - 0.05) < 1e-9 && rigid && neighbourOk && std::abs (ma - a.detectedPitch) < 0.1 && std::abs (mb - b.detectedPitch) < 0.1,
                   "a selected legato pair moves as one block" + tag,
                   fmt ("moved %.0f ms, pitches %+.3f / %+.3f st", 1000 * d, ma - a.detectedPitch, mb - b.detectedPitch));

            // pushing the block into the next note stops at it
            const double more = s.doc.moveNotesTime ({ a.id, b.id }, 1.0);
            nb = s.doc.findNote (b.id);
            nc = s.doc.findNote (after.id);
            check (more < 1.0 && nb->outEnd <= nc->outStart + 1e-9, "a block can't be pushed through its neighbour" + tag,
                   fmt ("only %.0f ms more", 1000 * more));

            // reset puts timing back as well, and the audio returns to the original
            s.doc.resetNote (a.id);
            s.doc.resetNote (b.id);
            s.apply();
            na = s.doc.findNote (a.id);
            nb = s.doc.findNote (b.id);
            double err = 0;
            for (size_t i = 0; i < ph.audio.size(); ++i) err = std::max (err, (double) std::abs (s.out[0][i] - ph.audio[i]));
            check (! na->hasTimingEdit() && ! nb->hasTimingEdit() && err == 0.0, "reset restores timing and the original audio" + tag);
            s.doc.undo();
            s.apply();
        }

        // --- save / restore edits ------------------------------------------------
        {
            s.doc.beginEdit();
            s.doc.setTargetPitch (notes[0].id, notes[0].detectedPitch - 1.0f);
            s.doc.moveNoteTime (notes[4].id, 0.03);
            s.doc.setDrawnPitch (100, { 61.0f, 61.1f, 61.2f });
            const auto text = s.doc.serialiseEdits();
            Session r;
            r.load (ph.audio, sr);
            const bool ok = r.doc.restoreEdits (text);
            bool same = ok && r.doc.notes().size() == s.doc.notes().size();
            for (size_t i = 0; same && i < r.doc.notes().size(); ++i)
            {
                const auto& x = r.doc.notes()[i];
                const auto& y = s.doc.notes()[i];
                same = std::abs (x.outStart - y.outStart) < 1e-6 && std::abs (x.targetPitch - y.targetPitch) < 1e-5f && x.id == y.id;
            }
            same = same && std::abs (r.doc.drawnPitch()[101] - 61.1f) < 1e-5f;
            check (same, "serialise + restore edits" + tag, fmt ("%g bytes", (double) text.size()));
            s.doc.undo();
            s.apply();
        }

        // --- stereo: both channels rendered the same way ----------------------
        {
            EditDocument d;
            std::vector<float> right = ph.audio;
            for (auto& v : right) v *= 0.5f;
            d.load ({ ph.audio, right }, sr);
            Renderer r;
            r.prepare (d);
            d.setTargetPitch (d.notes()[2].id, d.notes()[2].detectedPitch + 1.0f);
            std::vector<std::vector<float>> out;
            r.renderAll (d, d.buildCurves(), out);
            double err = 0;
            for (size_t i = 0; i < out[0].size(); ++i) err = std::max (err, (double) std::abs (out[0][i] * 0.5f - out[1][i]));
            check (err < 1e-5, "stereo channels stay in sync" + tag, fmt ("max diff %g", err));
        }
    }

    // --- performance on a 3-minute vocal ----------------------------------------
    {
        const double sr = 48000.0;
        const auto ph = testsig::makePhrase (sr);
        std::vector<float> longTake;
        while (longTake.size() < (size_t) (180 * sr))
            longTake.insert (longTake.end(), ph.audio.begin(), ph.audio.end());

        using clock = std::chrono::steady_clock;
        auto t0 = clock::now();
        Session s;
        s.doc.load ({ longTake }, sr);
        auto t1 = clock::now();
        s.renderer.prepare (s.doc);
        auto t2 = clock::now();
        s.curves = s.doc.buildCurves();
        s.renderer.renderAll (s.doc, s.curves, s.out);
        std::array<bool, 128> chrom; chrom.fill (true);
        s.doc.correctAll (chrom, 0.6f, 0.9f);
        auto t3 = clock::now();
        s.apply();
        auto t4 = clock::now();
        s.doc.setTargetPitch (s.doc.notes()[100].id, s.doc.notes()[100].targetPitch + 1.0f);
        auto t5 = clock::now();
        auto changed = s.apply();
        auto t6 = clock::now();
        auto ms = [] (auto a, auto b) { return std::chrono::duration<double, std::milli> (b - a).count(); };
        std::printf ("[INFO] 3-min take: analysis %.0f ms (%zu notes), marks %.0f ms, render everything corrected %.0f ms, "
                     "single-note edit %.1f ms (%.2f s re-rendered)\n",
                     ms (t0, t1), s.doc.notes().size(), ms (t1, t2), ms (t3, t4), ms (t5, t6),
                     changed.empty() ? 0.0 : (changed[0].second - changed[0].first) / sr);
        check (ms (t5, t6) < 250.0, "single-note edit re-renders quickly", fmt ("%.1f ms", ms (t5, t6)));
    }

    // --- demo renders --------------------------------------------------------------
    if (outDir)
    {
        const double sr = 44100.0;
        const auto ph = testsig::makePhrase (sr);
        Session s;
        s.load (ph.audio, sr);
        testsig::writeWav (std::string (outDir) + "/edit_1_original.wav", ph.audio, (int) sr);

        std::array<bool, 128> cMajor {};
        for (int n = 0; n < 128; ++n) { const int pc = n % 12; cMajor[(size_t) n] = pc == 0 || pc == 2 || pc == 4 || pc == 5 || pc == 7 || pc == 9 || pc == 11; }
        s.doc.correctAll (cMajor, 0.6f, 0.9f);
        s.apply();
        testsig::writeWav (std::string (outDir) + "/edit_2_correct_all_C_major.wav", s.out[0], (int) sr);

        // a melody rewrite plus a timing nudge
        const auto& n = s.doc.notes();
        s.doc.setTargetPitch (n[3].id, 69.0f);                     // long note up to A
        s.doc.setNoteShape (n[3].id, 0.8f, 1.3f);                  // a bit more vibrato
        s.doc.setTargetPitch (n[5].id, 67.0f);                     // repeated note becomes a step down
        s.doc.moveNoteTime (n[4].id, 0.05);                        // lay back a little
        s.doc.setNoteOutEdges (n[7].id, n[7].outStart, n[7].outEnd + 0.2); // hold the last note longer
        s.apply();
        testsig::writeWav (std::string (outDir) + "/edit_3_new_melody_and_timing.wav", s.out[0], (int) sr);
        std::printf ("[INFO] wrote demo WAVs to %s\n", outDir);
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL EDIT TESTS PASSED" : "SOME EDIT TESTS FAILED",
                 failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
