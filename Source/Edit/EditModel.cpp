#include "EditModel.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>

namespace claudetune::edit
{

namespace
{
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    constexpr double kSpliceMarginSec = 0.25; // re-analysed frames kept either side of new audio
    constexpr double kContextSec = 1.0;       // extra audio the re-analysis looks at
    constexpr float kSilence = 1e-6f;

    // audio revisions are unique across documents, so a cache keyed by one can't be fooled
    std::atomic<uint64_t> globalAudioRevision { 0 };

    // exact (bitwise) sample comparison: "changed" means any difference at all
    inline bool differs (float a, float b) noexcept
    {
        return std::memcmp (&a, &b, sizeof (float)) != 0
               && ! (std::fpclassify (a) == FP_ZERO && std::fpclassify (b) == FP_ZERO); // +0 and -0 are the same
    }
}

//==============================================================================
// TimeMap

double TimeMap::outToSrc (double out) const noexcept
{
    if (points.empty()) return out;
    const auto j = (size_t) (std::upper_bound (points.begin(), points.end(), out,
                                               [] (double v, const std::pair<double, double>& p) { return v < p.first; })
                             - points.begin());
    if (j == 0) return points.front().second + (out - points.front().first);
    if (j == points.size()) return points.back().second + (out - points.back().first);
    const auto& a = points[j - 1];
    const auto& b = points[j];
    const double span = b.first - a.first;
    return span > 0.0 ? a.second + (out - a.first) * (b.second - a.second) / span : b.second;
}

double TimeMap::srcToOut (double src) const noexcept
{
    if (points.empty()) return src;
    const auto j = (size_t) (std::upper_bound (points.begin(), points.end(), src,
                                               [] (double v, const std::pair<double, double>& p) { return v < p.second; })
                             - points.begin());
    if (j == 0) return points.front().first + (src - points.front().second);
    if (j == points.size()) return points.back().first + (src - points.back().second);
    const auto& a = points[j - 1];
    const auto& b = points[j];
    const double span = b.second - a.second;
    return span > 0.0 ? a.first + (src - a.second) * (b.first - a.first) / span : b.first;
}

double TimeMap::slopeAtOut (double out) const noexcept
{
    if (points.size() < 2) return 1.0;
    const auto j = (size_t) (std::upper_bound (points.begin(), points.end(), out,
                                               [] (double v, const std::pair<double, double>& p) { return v < p.first; })
                             - points.begin());
    if (j == 0 || j == points.size()) return 1.0;
    const auto& a = points[j - 1];
    const auto& b = points[j];
    const double span = b.first - a.first;
    return span > 0.0 ? (b.second - a.second) / span : 1.0;
}

//==============================================================================
// RenderCurves

namespace
{
    inline float interpFrames (const std::vector<float>& v, double frame) noexcept
    {
        if (v.empty()) return 0.0f;
        if (frame <= 0.0) return v.front();
        const auto f0 = (size_t) frame;
        if (f0 + 1 >= v.size()) return v.back();
        const float t = (float) (frame - (double) f0);
        return v[f0] + t * (v[f0 + 1] - v[f0]);
    }
}

float RenderCurves::shiftAtSample (double srcPos) const noexcept { return interpFrames (shift, srcPos / hop); }
float RenderCurves::gainAtSample (double srcPos) const noexcept  { return interpFrames (gainDb, srcPos / hop); }

//==============================================================================
// Loading

void EditDocument::load (std::vector<std::vector<float>> channels, double sampleRate,
                         const AnalysisSettings& analysis, const SegmentSettings& segmentation)
{
    sr = sampleRate;
    analysisSettings = analysis;
    segmentSettings = segmentation;
    source = std::move (channels);
    if (source.empty())
        source.emplace_back();
    length = (int64_t) source[0].size();
    for (auto& ch : source)
        ch.resize ((size_t) length, 0.0f);

    mono.assign ((size_t) length, 0.0f);
    recomputeMono (0, length);

    track = analyzePitch (mono.data(), length, sr, analysis);

    noteList.clear();
    nextId = 1;
    for (const auto& d : segmentNotes (track, segmentation))
    {
        Note n;
        n.id = nextId++;
        n.srcStart = n.outStart = track.frameToSeconds (d.startFrame);
        n.srcEnd = n.outEnd = std::min (track.frameToSeconds (d.endFrame), (double) length / sr);
        n.detectedPitch = n.targetPitch = d.centre;
        noteList.push_back (n);
    }

    drawn.assign ((size_t) track.numFrames(), kNaN);
    undoStack.clear();
    redoStack.clear();
    backup = {};
    audioRev = ++globalAudioRevision;
    touch();
}

void EditDocument::initEmpty (int numChannels, double sampleRate,
                              const AnalysisSettings& analysis, const SegmentSettings& segmentation)
{
    sr = sampleRate;
    analysisSettings = analysis;
    segmentSettings = segmentation;
    source.assign ((size_t) std::max (1, numChannels), {});
    mono.clear();
    length = 0;
    track = {};
    track.sampleRate = sr;
    track.hop = analysisHop (sr, analysis);
    track.numSamples = 0;
    track.frames.assign ((size_t) numFramesFor (0, track.hop), PitchFrame {});
    noteList.clear();
    drawn.assign (track.frames.size(), kNaN);
    nextId = 1;
    undoStack.clear();
    redoStack.clear();
    backup = {};
    audioRev = ++globalAudioRevision;
    touch();
}

void EditDocument::loadWithTrack (std::vector<std::vector<float>> channels, double sampleRate,
                                  std::vector<PitchFrame> frames,
                                  const AnalysisSettings& analysis, const SegmentSettings& segmentation)
{
    sr = sampleRate;
    analysisSettings = analysis;
    segmentSettings = segmentation;
    source = std::move (channels);
    if (source.empty())
        source.emplace_back();
    length = (int64_t) source[0].size();
    for (auto& ch : source)
        ch.resize ((size_t) length, 0.0f);
    mono.assign ((size_t) length, 0.0f);
    recomputeMono (0, length);

    track = {};
    track.sampleRate = sr;
    track.hop = analysisHop (sr, analysis);
    track.numSamples = length;
    track.frames = std::move (frames);
    track.frames.resize ((size_t) numFramesFor (length, track.hop));

    noteList.clear();
    drawn.assign (track.frames.size(), kNaN);
    nextId = 1;
    undoStack.clear();
    redoStack.clear();
    backup = {};
    audioRev = ++globalAudioRevision;
    touch();
}

void EditDocument::recomputeMono (int64_t start, int64_t end)
{
    const float scale = 1.0f / (float) source.size();
    for (int64_t i = start; i < end; ++i)
    {
        float v = 0.0f;
        for (const auto& ch : source)
            v += ch[(size_t) i];
        mono[(size_t) i] = v * scale;
    }
}

void EditDocument::growTo (int64_t newLength)
{
    if (newLength <= length) return;
    for (auto& ch : source)
        ch.resize ((size_t) newLength, 0.0f);
    mono.resize ((size_t) newLength, 0.0f);
    length = newLength;
    track.numSamples = length;
    const auto nf = (size_t) numFramesFor (length, track.hop);
    if (track.frames.size() < nf) track.frames.resize (nf, PitchFrame {});
    if (drawn.size() < nf) drawn.resize (nf, kNaN);
}

int EditDocument::noteStartFrame (const Note& n) const noexcept
{
    return (int) std::lround (n.srcStart * sr / track.hop);
}

int EditDocument::noteEndFrame (const Note& n) const noexcept
{
    return (int) std::lround (n.srcEnd * sr / track.hop);
}

//==============================================================================
// Capture

void EditDocument::extendBackup (int64_t start, int64_t end)
{
    // Must run before the new audio is written: the document still holds the old audio.
    const int hop = track.hop;
    if (backup.valid && backup.open)
    {
        const int64_t bs = backup.start, be = backup.start + backupLength();
        const int64_t ns = std::min (bs, start), ne = std::max (be, end);
        if (ns == bs && ne == be)
            return;

        std::vector<std::vector<float>> audio (source.size(), std::vector<float> ((size_t) (ne - ns)));
        for (size_t c = 0; c < source.size(); ++c)
            for (int64_t i = ns; i < ne; ++i)
                audio[c][(size_t) (i - ns)] = (i >= bs && i < be) ? backup.audio[c][(size_t) (i - bs)]
                                                                 : (i < length ? source[c][(size_t) i] : 0.0f);

        const int fs = (int) (ns / hop), fe = std::min (track.numFrames(), (int) (ne / hop) + 2);
        const int obs = backup.frameStart, obe = backup.frameStart + (int) backup.frames.size();
        std::vector<PitchFrame> frames ((size_t) std::max (0, fe - fs));
        for (int f = fs; f < fe; ++f)
            frames[(size_t) (f - fs)] = (f >= obs && f < obe) ? backup.frames[(size_t) (f - obs)] : track.frames[(size_t) f];

        backup.start = ns;
        backup.audio = std::move (audio);
        backup.frameStart = fs;
        backup.frames = std::move (frames);
        return;
    }

    backup = {};
    backup.valid = backup.open = true;
    backup.start = start;
    backup.audio.assign (source.size(), {});
    for (size_t c = 0; c < source.size(); ++c)
        backup.audio[c].assign (source[c].begin() + start, source[c].begin() + end);
    backup.frameStart = (int) (start / hop);
    const int fe = std::min (track.numFrames(), (int) (end / hop) + 2);
    backup.frames.assign (track.frames.begin() + backup.frameStart, track.frames.begin() + std::max (backup.frameStart, fe));
    backup.state = { noteList, drawn, nextId };
}

ReplaceResult EditDocument::replaceAudio (int64_t start, const std::vector<std::vector<float>>& audio)
{
    ReplaceResult r;
    r.oldLength = r.newLength = length;
    if (audio.empty() || audio[0].empty() || start < 0 || source.empty())
        return r;

    const auto n = (int64_t) audio[0].size();
    const int64_t end = start + n;
    growTo (end);
    r.newLength = length;

    auto newSample = [&] (size_t c, int64_t i) {
        const auto& ch = audio[std::min (c, audio.size() - 1)];
        return ch[(size_t) (i - start)];
    };

    // ---- 1. which samples actually change? ------------------------------------
    int64_t cs = -1, ce = -1;
    bool hadSignal = false;
    for (int64_t i = start; i < end; ++i)
        for (size_t c = 0; c < source.size(); ++c)
            if (differs (source[c][(size_t) i], newSample (c, i)))
            {
                if (cs < 0) cs = i;
                ce = i + 1;
                break;
            }

    if (cs < 0)
        return r; // identical (or only growth with silence)

    for (int64_t i = cs; i < ce && ! hadSignal; ++i)
        for (size_t c = 0; c < source.size(); ++c)
            if (std::abs (source[c][(size_t) i]) > kSilence) { hadSignal = true; break; }

    if (hadSignal)
        extendBackup (cs, ce);

    for (size_t c = 0; c < source.size(); ++c)
        for (int64_t i = cs; i < ce; ++i)
            source[c][(size_t) i] = newSample (c, i);
    recomputeMono (cs, ce);

    r.audioChanged = true;
    r.changed = { cs, ce };
    r.replacedSignal = hadSignal;

    // ---- 2. re-analyse around the change ----------------------------------------
    const int hop = track.hop;
    const int nf = track.numFrames();
    const int margin = (int) std::ceil (kSpliceMarginSec * sr / hop);
    const int context = (int) std::ceil (kContextSec * sr / hop);
    const int fa = (int) (cs / hop), fb = (int) ((ce + hop - 1) / hop) + 1;
    const int sa = std::max (0, fa - margin), sb = std::min (nf, fb + margin);
    const int aa = std::max (0, sa - context), ab = std::min (nf, sb + context);

    const auto frames = analyzeFrames (mono.data(), length, sr, analysisSettings, aa, ab);
    for (int f = sa; f < sb; ++f)
        track.frames[(size_t) f] = frames[(size_t) (f - aa)];

    // ---- 3. re-segment the affected voiced runs ---------------------------------
    int ga = sa, gb = sb;
    for (bool grew = true; grew;)
    {
        grew = false;
        expandToVoicedRuns (track, segmentSettings, ga, gb);
        for (const auto& note : noteList)
        {
            const int s = noteStartFrame (note), e = noteEndFrame (note);
            if (s < gb && e > ga && (s < ga || e > gb))
            {
                ga = std::min (ga, s);
                gb = std::max (gb, e);
                grew = true;
            }
        }
    }

    std::vector<Note> removed, kept;
    for (const auto& note : noteList)
    {
        if (noteStartFrame (note) < gb && noteEndFrame (note) > ga)
            removed.push_back (note);
        else
            kept.push_back (note);
    }

    const double csSec = (double) cs / sr, ceSec = (double) ce / sr;
    std::vector<bool> used (removed.size(), false);

    for (const auto& d : segmentNotes (track, segmentSettings, ga, gb))
    {
        Note nn;
        nn.srcStart = nn.outStart = track.frameToSeconds (d.startFrame);
        nn.srcEnd = nn.outEnd = std::min (track.frameToSeconds (d.endFrame), (double) length / sr);
        nn.detectedPitch = nn.targetPitch = d.centre;

        // carry over the edits of an old note on unchanged audio that this one replaces
        // (never onto a note that contains part of a new performance)
        const bool onNewTake = hadSignal && nn.srcStart < ceSec && nn.srcEnd > csSec;
        int best = -1;
        double bestOverlap = 0.0;
        for (size_t k = 0; k < removed.size() && ! onNewTake; ++k)
        {
            const auto& o = removed[k];
            if (used[k]) continue;
            if (o.srcStart < ceSec && o.srcEnd > csSec) continue; // its audio changed
            const double overlap = std::min (o.srcEnd, nn.srcEnd) - std::max (o.srcStart, nn.srcStart);
            if (overlap >= 0.5 * (o.srcEnd - o.srcStart) && overlap > bestOverlap)
            {
                bestOverlap = overlap;
                best = (int) k;
            }
        }

        if (best >= 0)
        {
            const auto& o = removed[(size_t) best];
            used[(size_t) best] = true;
            nn.id = o.id;
            nn.targetPitch = nn.detectedPitch + (o.targetPitch - o.detectedPitch);
            nn.center = o.center;
            nn.variation = o.variation;
            nn.transitionInMs = o.transitionInMs;
            nn.transitionOutMs = o.transitionOutMs;
            nn.gainDb = o.gainDb;
            nn.outStart = nn.srcStart + (o.outStart - o.srcStart);
            nn.outEnd = nn.srcEnd + (o.outEnd - o.srcEnd);
        }
        else
            nn.id = nextId++;

        kept.push_back (nn);
    }

    noteList = std::move (kept);
    sortNotes();

    for (int f = std::max (0, fa); f < std::min ((int) drawn.size(), fb); ++f)
        drawn[(size_t) f] = kNaN;

    sanitiseTiming();
    undoStack.clear();
    redoStack.clear();
    audioRev = ++globalAudioRevision;
    touch();
    return r;
}

SampleRange EditDocument::restoreCaptureBackup()
{
    if (! backup.valid)
        return { 0, 0 };

    const int64_t bs = backup.start, be = backup.start + backupLength();
    growTo (be);
    for (size_t c = 0; c < source.size() && c < backup.audio.size(); ++c)
        std::copy (backup.audio[c].begin(), backup.audio[c].end(), source[c].begin() + bs);
    recomputeMono (bs, be);

    for (size_t k = 0; k < backup.frames.size(); ++k)
    {
        const size_t f = (size_t) backup.frameStart + k;
        if (f < track.frames.size())
            track.frames[f] = backup.frames[k];
    }

    // notes: the backed-up ones inside the range, the current ones outside it
    const double bsSec = (double) bs / sr, beSec = (double) be / sr;
    std::vector<Note> merged;
    for (const auto& n : noteList)
        if (! (n.srcStart < beSec && n.srcEnd > bsSec))
            merged.push_back (n);
    for (const auto& n : backup.state.notes)
        if (n.srcStart < beSec && n.srcEnd > bsSec)
            merged.push_back (n);

    // avoid id clashes between the two sets
    nextId = std::max (nextId, backup.state.nextId);
    std::sort (merged.begin(), merged.end(), [] (const Note& a, const Note& b) { return a.id < b.id; });
    for (size_t i = 1; i < merged.size(); ++i)
        if (merged[i].id == merged[i - 1].id)
            merged[i].id = nextId++;
    noteList = std::move (merged);
    sortNotes();

    const int fs = (int) (bs / track.hop), fe = (int) (be / track.hop) + 2;
    for (int f = std::max (0, fs); f < std::min ((int) drawn.size(), fe); ++f)
        drawn[(size_t) f] = (size_t) f < backup.state.drawn.size() ? backup.state.drawn[(size_t) f] : kNaN;

    sanitiseTiming();
    backup = {};
    undoStack.clear();
    redoStack.clear();
    audioRev = ++globalAudioRevision;
    touch();
    return { bs, be };
}

void EditDocument::sanitiseTiming()
{
    const double total = (double) length / sr;
    auto resetTiming = [] (Note& n) { n.outStart = n.srcStart; n.outEnd = n.srcEnd; };

    for (auto& n : noteList)
        if (n.outEnd - n.outStart < kMinNoteSeconds * 0.5 || n.outStart < 0.0 || n.outEnd > total + 1e-9)
            resetTiming (n);

    for (int pass = 0; pass < 1000; ++pass)
    {
        bool fixed = false;
        for (size_t i = 1; i < noteList.size(); ++i)
        {
            auto& a = noteList[i - 1];
            auto& b = noteList[i];
            const bool joined = legatoJoined (a, b);
            const bool bad = joined ? std::abs (a.outEnd - b.outStart) > 1e-9 : (b.outStart < a.outEnd - 1e-9);
            if (bad)
            {
                resetTiming (a);
                resetTiming (b);
                fixed = true;
            }
        }
        if (! fixed) break;
    }
}

//==============================================================================
const Note* EditDocument::findNote (uint32_t id) const noexcept
{
    for (const auto& n : noteList)
        if (n.id == id) return &n;
    return nullptr;
}

Note* EditDocument::find (uint32_t id) noexcept
{
    for (auto& n : noteList)
        if (n.id == id) return &n;
    return nullptr;
}

int EditDocument::indexOf (uint32_t id) const noexcept
{
    for (size_t i = 0; i < noteList.size(); ++i)
        if (noteList[i].id == id) return (int) i;
    return -1;
}

void EditDocument::sortNotes()
{
    std::sort (noteList.begin(), noteList.end(), [] (const Note& a, const Note& b) { return a.srcStart < b.srcStart; });
}

bool EditDocument::legatoJoined (const Note& a, const Note& b) const noexcept
{
    return b.srcStart - a.srcEnd < 0.5 * track.hop / sr;
}

//==============================================================================
void EditDocument::beginEdit()
{
    undoStack.push_back ({ noteList, drawn, nextId });
    if (undoStack.size() > kUndoLimit)
        undoStack.erase (undoStack.begin());
    redoStack.clear();
}

bool EditDocument::undo()
{
    if (undoStack.empty()) return false;
    redoStack.push_back ({ noteList, drawn, nextId });
    noteList = std::move (undoStack.back().notes);
    drawn = std::move (undoStack.back().drawn);
    nextId = undoStack.back().nextId;
    undoStack.pop_back();
    touch();
    return true;
}

bool EditDocument::redo()
{
    if (redoStack.empty()) return false;
    undoStack.push_back ({ noteList, drawn, nextId });
    noteList = std::move (redoStack.back().notes);
    drawn = std::move (redoStack.back().drawn);
    nextId = redoStack.back().nextId;
    redoStack.pop_back();
    touch();
    return true;
}

//==============================================================================
bool EditDocument::setTargetPitch (uint32_t id, float target)
{
    auto* n = find (id);
    if (! n) return false;
    n->targetPitch = std::clamp (target, n->detectedPitch - 24.0f, n->detectedPitch + 24.0f);
    touch();
    return true;
}

bool EditDocument::setNoteShape (uint32_t id, float center, float variation)
{
    auto* n = find (id);
    if (! n) return false;
    n->center = std::clamp (center, 0.0f, 1.0f);
    n->variation = std::clamp (variation, 0.0f, 2.0f);
    touch();
    return true;
}

bool EditDocument::setTransitions (uint32_t id, float inMs, float outMs)
{
    auto* n = find (id);
    if (! n) return false;
    n->transitionInMs = std::clamp (inMs, 0.0f, 500.0f);
    n->transitionOutMs = std::clamp (outMs, 0.0f, 500.0f);
    touch();
    return true;
}

bool EditDocument::setGain (uint32_t id, float gainDb)
{
    auto* n = find (id);
    if (! n) return false;
    n->gainDb = std::clamp (gainDb, -48.0f, 12.0f);
    touch();
    return true;
}

double EditDocument::moveNoteTime (uint32_t id, double delta)
{
    const int i = indexOf (id);
    if (i < 0) return 0.0;
    auto& n = noteList[(size_t) i];
    const double total = (double) length / sr;

    double lo = -n.outStart;                  // can't start before the clip
    double hi = total - n.outEnd;             // or end after it

    if (i > 0)
    {
        const auto& p = noteList[(size_t) i - 1];
        if (legatoJoined (p, n))
        {
            const double pLen = p.srcEnd - p.srcStart;
            lo = std::max (lo, p.outStart + std::max (kMinNoteSeconds, pLen / kMaxStretch) - n.outStart);
            hi = std::min (hi, p.outStart + pLen * kMaxStretch - n.outStart);
        }
        else
            lo = std::max (lo, p.outEnd - n.outStart);
    }
    if (i + 1 < (int) noteList.size())
    {
        const auto& q = noteList[(size_t) i + 1];
        if (legatoJoined (n, q))
        {
            const double qLen = q.srcEnd - q.srcStart;
            hi = std::min (hi, q.outEnd - std::max (kMinNoteSeconds, qLen / kMaxStretch) - n.outEnd);
            lo = std::max (lo, q.outEnd - qLen * kMaxStretch - n.outEnd);
        }
        else
            hi = std::min (hi, q.outStart - n.outEnd);
    }

    if (lo > hi) return 0.0;
    const double d = std::clamp (delta, lo, hi);
    if (std::abs (d) < 1e-12) return 0.0;

    n.outStart += d;
    n.outEnd += d;
    if (i > 0 && legatoJoined (noteList[(size_t) i - 1], n))
        noteList[(size_t) i - 1].outEnd = n.outStart;
    if (i + 1 < (int) noteList.size() && legatoJoined (n, noteList[(size_t) i + 1]))
        noteList[(size_t) i + 1].outStart = n.outEnd;
    touch();
    return d;
}

double EditDocument::moveNotesTime (const std::vector<uint32_t>& ids, double delta)
{
    if (ids.size() == 1)
        return moveNoteTime (ids.front(), delta);

    std::vector<int> idx;
    for (const auto id : ids)
        if (const int i = indexOf (id); i >= 0)
            idx.push_back (i);
    if (idx.empty()) return 0.0;
    std::sort (idx.begin(), idx.end());
    idx.erase (std::unique (idx.begin(), idx.end()), idx.end());

    // runs of neighbouring selected notes move as rigid blocks
    std::vector<std::pair<int, int>> blocks;
    for (const int i : idx)
    {
        if (! blocks.empty() && blocks.back().second == i - 1) blocks.back().second = i;
        else blocks.push_back ({ i, i });
    }

    const double total = (double) length / sr;
    double lo = -1e9, hi = 1e9;
    for (const auto& [first, last] : blocks)
    {
        const auto& a = noteList[(size_t) first];
        const auto& b = noteList[(size_t) last];
        lo = std::max (lo, -a.outStart);
        hi = std::min (hi, total - b.outEnd);
        if (first > 0)
        {
            const auto& p = noteList[(size_t) first - 1];
            if (legatoJoined (p, a))
            {
                const double pLen = p.srcEnd - p.srcStart;
                lo = std::max (lo, p.outStart + std::max (kMinNoteSeconds, pLen / kMaxStretch) - a.outStart);
                hi = std::min (hi, p.outStart + pLen * kMaxStretch - a.outStart);
            }
            else
                lo = std::max (lo, p.outEnd - a.outStart);
        }
        if (last + 1 < (int) noteList.size())
        {
            const auto& q = noteList[(size_t) last + 1];
            if (legatoJoined (b, q))
            {
                const double qLen = q.srcEnd - q.srcStart;
                hi = std::min (hi, q.outEnd - std::max (kMinNoteSeconds, qLen / kMaxStretch) - b.outEnd);
                lo = std::max (lo, q.outEnd - qLen * kMaxStretch - b.outEnd);
            }
            else
                hi = std::min (hi, q.outStart - b.outEnd);
        }
    }
    if (lo > hi) return 0.0;
    const double d = std::clamp (delta, lo, hi);
    if (std::abs (d) < 1e-12) return 0.0;

    for (const auto& [first, last] : blocks)
    {
        for (int k = first; k <= last; ++k)
        {
            noteList[(size_t) k].outStart += d;
            noteList[(size_t) k].outEnd += d;
        }
        if (first > 0 && legatoJoined (noteList[(size_t) first - 1], noteList[(size_t) first]))
            noteList[(size_t) first - 1].outEnd = noteList[(size_t) first].outStart;
        if (last + 1 < (int) noteList.size() && legatoJoined (noteList[(size_t) last], noteList[(size_t) last + 1]))
            noteList[(size_t) last + 1].outStart = noteList[(size_t) last].outEnd;
    }
    touch();
    return d;
}

bool EditDocument::setNoteOutEdges (uint32_t id, double outStart, double outEnd)
{
    const int i = indexOf (id);
    if (i < 0) return false;
    auto& n = noteList[(size_t) i];
    const double total = (double) length / sr;
    const double srcLen = n.srcEnd - n.srcStart;

    double sLo = 0.0, sHi = total;
    double eLo = 0.0, eHi = total;
    if (i > 0)
    {
        const auto& p = noteList[(size_t) i - 1];
        if (legatoJoined (p, n))
        {
            const double pLen = p.srcEnd - p.srcStart;
            sLo = p.outStart + std::max (kMinNoteSeconds, pLen / kMaxStretch);
            sHi = p.outStart + pLen * kMaxStretch;
        }
        else
            sLo = p.outEnd;
    }
    if (i + 1 < (int) noteList.size())
    {
        const auto& q = noteList[(size_t) i + 1];
        if (legatoJoined (n, q))
        {
            const double qLen = q.srcEnd - q.srcStart;
            eHi = q.outEnd - std::max (kMinNoteSeconds, qLen / kMaxStretch);
            eLo = q.outEnd - qLen * kMaxStretch;
        }
        else
            eHi = q.outStart;
    }

    const double minLen = std::max (kMinNoteSeconds, srcLen / kMaxStretch);
    const double maxLen = srcLen * kMaxStretch;

    double s = std::clamp (outStart, sLo, std::min (sHi, outEnd - minLen));
    double e = std::clamp (outEnd, std::max (eLo, s + minLen), std::min (eHi, s + maxLen));
    if (e - s < minLen) return false;
    if (std::abs (s - n.outStart) < 1e-12 && std::abs (e - n.outEnd) < 1e-12) return false;

    n.outStart = s;
    n.outEnd = e;
    if (i > 0 && legatoJoined (noteList[(size_t) i - 1], n))
        noteList[(size_t) i - 1].outEnd = s;
    if (i + 1 < (int) noteList.size() && legatoJoined (n, noteList[(size_t) i + 1]))
        noteList[(size_t) i + 1].outStart = e;
    touch();
    return true;
}

uint32_t EditDocument::splitNote (uint32_t id, double t)
{
    const int i = indexOf (id);
    if (i < 0) return 0;
    Note a = noteList[(size_t) i];
    if (t <= a.srcStart + kMinNoteSeconds || t >= a.srcEnd - kMinNoteSeconds) return 0;

    const double frac = (t - a.srcStart) / (a.srcEnd - a.srcStart);
    const double outT = a.outStart + frac * (a.outEnd - a.outStart);
    const float offset = a.targetPitch - a.detectedPitch;

    Note b = a;
    b.id = nextId++;
    a.srcEnd = t;
    a.outEnd = outT;
    b.srcStart = t;
    b.outStart = outT;

    for (Note* n : { &a, &b })
    {
        const float c = steadyCentre (track, (int) std::floor (track.secondsToFrame (n->srcStart)),
                                      (int) std::ceil (track.secondsToFrame (n->srcEnd)));
        if (c >= 0.0f) n->detectedPitch = c;
        n->targetPitch = n->detectedPitch + offset;
    }

    noteList[(size_t) i] = a;
    noteList.insert (noteList.begin() + i + 1, b);
    touch();
    return b.id;
}

uint32_t EditDocument::mergeNotes (uint32_t firstId, uint32_t secondId)
{
    int i = indexOf (firstId), j = indexOf (secondId);
    if (i < 0 || j < 0) return 0;
    if (i > j) std::swap (i, j);
    if (j != i + 1) return 0;

    auto& a = noteList[(size_t) i];
    const auto& b = noteList[(size_t) j];
    const float offset = a.targetPitch - a.detectedPitch;
    a.srcEnd = b.srcEnd;
    a.outEnd = b.outEnd;
    const float c = steadyCentre (track, (int) std::floor (track.secondsToFrame (a.srcStart)),
                                  (int) std::ceil (track.secondsToFrame (a.srcEnd)));
    if (c >= 0.0f) a.detectedPitch = c;
    a.targetPitch = a.detectedPitch + offset;
    a.transitionOutMs = b.transitionOutMs;
    const uint32_t keep = a.id;
    noteList.erase (noteList.begin() + j);
    touch();
    return keep;
}

float EditDocument::nearestAllowed (float pitch, const std::array<bool, 128>& allowed, float refOffset)
{
    const float p = pitch - refOffset;
    const int nearest = std::clamp ((int) std::lround (p), 0, 127);
    for (int d = 0; d <= 12; ++d)
    {
        const int up = nearest + d, down = nearest - d;
        const bool okUp = up < 128 && allowed[(size_t) up];
        const bool okDown = down >= 0 && allowed[(size_t) down];
        if (okUp && okDown) return (std::abs (p - (float) up) <= std::abs (p - (float) down) ? (float) up : (float) down) + refOffset;
        if (okUp) return (float) up + refOffset;
        if (okDown) return (float) down + refOffset;
    }
    return -1.0f;
}

void EditDocument::correctAll (const std::array<bool, 128>& allowed, float center, float variation,
                               float refOffset, const std::vector<uint32_t>* onlyThese)
{
    for (auto& n : noteList)
    {
        if (onlyThese != nullptr && std::find (onlyThese->begin(), onlyThese->end(), n.id) == onlyThese->end())
            continue;
        const float best = nearestAllowed (n.detectedPitch, allowed, refOffset);
        if (best >= 0.0f)
            n.targetPitch = best;
        n.center = std::clamp (center, 0.0f, 1.0f);
        n.variation = std::clamp (variation, 0.0f, 2.0f);
    }
    touch();
}

void EditDocument::resetNote (uint32_t id)
{
    const int i = indexOf (id);
    if (i < 0) return;
    auto& n = noteList[(size_t) i];
    Note fresh;
    fresh.id = n.id;
    fresh.srcStart = fresh.outStart = n.srcStart;
    fresh.srcEnd = fresh.outEnd = n.srcEnd;
    fresh.detectedPitch = fresh.targetPitch = n.detectedPitch;
    n = fresh;

    // legato neighbours meet it at its original edges again
    if (i > 0 && legatoJoined (noteList[(size_t) i - 1], n))
        noteList[(size_t) i - 1].outEnd = n.outStart;
    if (i + 1 < (int) noteList.size() && legatoJoined (n, noteList[(size_t) i + 1]))
        noteList[(size_t) i + 1].outStart = n.outEnd;
    // a neighbour squeezed too short (or overlapping) goes back to where it was sung
    for (int k : { i - 1, i + 1 })
        if (k >= 0 && k < (int) noteList.size())
        {
            auto& m = noteList[(size_t) k];
            if (m.outEnd - m.outStart < std::max (kMinNoteSeconds, (m.srcEnd - m.srcStart) / kMaxStretch) - 1e-9
                || m.outEnd - m.outStart > (m.srcEnd - m.srcStart) * kMaxStretch + 1e-9)
            {
                m.outStart = m.srcStart;
                m.outEnd = m.srcEnd;
            }
        }
    sanitiseTiming();

    clearDrawnPitch ((int) track.secondsToFrame (n.srcStart), (int) std::ceil (track.secondsToFrame (n.srcEnd)));
    touch();
}

void EditDocument::resetAll()
{
    for (auto& n : noteList)
    {
        Note fresh;
        fresh.id = n.id;
        fresh.srcStart = fresh.outStart = n.srcStart;
        fresh.srcEnd = fresh.outEnd = n.srcEnd;
        fresh.detectedPitch = fresh.targetPitch = n.detectedPitch;
        n = fresh;
    }
    std::fill (drawn.begin(), drawn.end(), kNaN);
    touch();
}

void EditDocument::setDrawnPitch (int startFrame, const std::vector<float>& values)
{
    for (size_t k = 0; k < values.size(); ++k)
    {
        const int f = startFrame + (int) k;
        if (f >= 0 && f < (int) drawn.size())
            drawn[(size_t) f] = values[k];
    }
    touch();
}

void EditDocument::clearDrawnPitch (int startFrame, int endFrame)
{
    for (int f = std::max (0, startFrame); f < std::min ((int) drawn.size(), endFrame); ++f)
        drawn[(size_t) f] = kNaN;
    touch();
}

//==============================================================================
TimeMap EditDocument::buildTimeMap() const
{
    TimeMap m;
    m.points.push_back ({ 0.0, 0.0 });
    for (const auto& n : noteList)
    {
        const std::pair<double, double> a { n.outStart * sr, n.srcStart * sr };
        const std::pair<double, double> b { n.outEnd * sr, n.srcEnd * sr };
        for (const auto& p : { a, b })
        {
            // keep both coordinates non-decreasing
            if (p.first >= m.points.back().first && p.second >= m.points.back().second)
                m.points.push_back (p);
        }
    }
    const double end = (double) length;
    if (end >= m.points.back().first && end >= m.points.back().second)
        m.points.push_back ({ end, end });
    return m;
}

RenderCurves EditDocument::buildCurves() const
{
    RenderCurves c;
    c.sampleRate = sr;
    c.hop = track.hop;
    c.numSamples = length;
    c.map = buildTimeMap();

    const int nf = track.numFrames();
    std::vector<float> raw ((size_t) nf, 0.0f);
    std::vector<float> gain ((size_t) nf, 0.0f);
    const double frameSec = track.frameToSeconds (1.0);
    const int driftHalf = std::max (2, (int) std::lround (0.060 / frameSec)); // 120 ms window

    // hand-drawn pitch fades in and out (S-curve, ~40 ms) at the ends of each stroke,
    // so a drawn line never jumps against the pitch around it
    std::vector<float> drawWeight ((size_t) nf, 0.0f);
    {
        const float blend = (float) std::max (1.0, 0.040 / frameSec);
        for (int f = 0; f < nf && f < (int) drawn.size();)
        {
            if (std::isnan (drawn[(size_t) f])) { ++f; continue; }
            int e = f;
            while (e < nf && e < (int) drawn.size() && ! std::isnan (drawn[(size_t) e])) ++e;
            for (int g = f; g < e; ++g)
            {
                const float x = std::min (1.0f, (float) std::min (g - f + 1, e - g) / blend);
                drawWeight[(size_t) g] = 0.5f - 0.5f * std::cos (3.14159265f * x);
            }
            f = e;
        }
    }

    for (const auto& n : noteList)
    {
        const int a = std::clamp ((int) std::lround (track.secondsToFrame (n.srcStart)), 0, nf);
        const int b = std::clamp ((int) std::lround (track.secondsToFrame (n.srcEnd)), a, nf);
        const float base = n.targetPitch - n.detectedPitch;

        // running sums give the drift (slow component) in O(1) per frame
        std::vector<double> sum ((size_t) (b - a) + 1, 0.0);
        std::vector<int> cnt ((size_t) (b - a) + 1, 0);
        for (int f = a; f < b; ++f)
        {
            const bool v = track.voiced (f);
            sum[(size_t) (f - a) + 1] = sum[(size_t) (f - a)] + (v ? track.frames[(size_t) f].midi : 0.0);
            cnt[(size_t) (f - a) + 1] = cnt[(size_t) (f - a)] + (v ? 1 : 0);
        }

        for (int f = a; f < b; ++f)
        {
            gain[(size_t) f] = n.gainDb;
            if (! track.voiced (f))
            {
                raw[(size_t) f] = base;
                continue;
            }

            const float p = track.frames[(size_t) f].midi;
            const int lo = std::max (a, f - driftHalf) - a, hi = std::min (b, f + driftHalf + 1) - a;
            const int count = cnt[(size_t) hi] - cnt[(size_t) lo];
            const float drift = count > 0 ? (float) ((sum[(size_t) hi] - sum[(size_t) lo]) / count) : p;

            raw[(size_t) f] = base - n.center * (drift - n.detectedPitch) + (n.variation - 1.0f) * (p - drift);

            if (drawWeight[(size_t) f] > 0.0f)
                raw[(size_t) f] += drawWeight[(size_t) f] * ((drawn[(size_t) f] - p) - raw[(size_t) f]);
        }
    }

    // transitions: glide the shift across each note edge
    std::vector<float> shift = raw;
    auto glide = [&] (int edge, float ms) {
        const int half = (int) std::lround (0.5 * ms / 1000.0 / frameSec);
        if (half < 1) return;
        const int l = std::max (0, edge - half), r = std::min (nf - 1, edge + half);
        if (r <= l) return;
        const float vl = raw[(size_t) l], vr = raw[(size_t) r];
        for (int f = l + 1; f < r; ++f)
            shift[(size_t) f] = vl + (vr - vl) * (float) (f - l) / (float) (r - l);
    };
    for (const auto& n : noteList)
    {
        glide ((int) std::lround (track.secondsToFrame (n.srcStart)), n.transitionInMs);
        glide ((int) std::lround (track.secondsToFrame (n.srcEnd)), n.transitionOutMs);
    }

    // short ramps on gain edges
    c.gainDb.assign ((size_t) nf, 0.0f);
    for (const auto& n : noteList)
    {
        if (std::abs (n.gainDb) < 1e-6f) continue;
        const int a = std::clamp ((int) std::lround (track.secondsToFrame (n.srcStart)) - 2, 0, nf);
        const int b = std::clamp ((int) std::lround (track.secondsToFrame (n.srcEnd)) + 3, a, nf);
        for (int f = a; f < b; ++f)
        {
            float s = 0.0f;
            int k = 0;
            for (int g = std::max (0, f - 2); g <= std::min (nf - 1, f + 2); ++g) { s += gain[(size_t) g]; ++k; }
            c.gainDb[(size_t) f] = s / (float) k;
        }
    }

    c.shift = std::move (shift);
    return c;
}

//==============================================================================
std::string EditDocument::serialiseEdits() const
{
    std::ostringstream os;
    os.precision (10);
    os << "claudetune-edits 1\n";
    os << "nextid " << nextId << "\n";
    for (const auto& n : noteList)
        os << "note " << n.id << ' ' << n.srcStart << ' ' << n.srcEnd << ' ' << n.outStart << ' ' << n.outEnd << ' '
           << n.detectedPitch << ' ' << n.targetPitch << ' ' << n.center << ' ' << n.variation << ' '
           << n.transitionInMs << ' ' << n.transitionOutMs << ' ' << n.gainDb << "\n";
    for (size_t f = 0; f < drawn.size();)
    {
        if (std::isnan (drawn[f])) { ++f; continue; }
        size_t e = f;
        while (e < drawn.size() && ! std::isnan (drawn[e])) ++e;
        os << "drawn " << f << ' ' << (e - f);
        for (size_t k = f; k < e; ++k) os << ' ' << drawn[k];
        os << "\n";
        f = e;
    }
    return os.str();
}

bool EditDocument::restoreEdits (const std::string& text)
{
    std::istringstream is (text);
    std::string tag;
    int version = 0;
    if (! (is >> tag >> version) || tag != "claudetune-edits") return false;

    std::vector<Note> notes;
    std::vector<float> newDrawn ((size_t) track.numFrames(), kNaN);
    uint32_t newNext = 1;

    while (is >> tag)
    {
        if (tag == "nextid") is >> newNext;
        else if (tag == "note")
        {
            Note n;
            is >> n.id >> n.srcStart >> n.srcEnd >> n.outStart >> n.outEnd >> n.detectedPitch >> n.targetPitch
               >> n.center >> n.variation >> n.transitionInMs >> n.transitionOutMs >> n.gainDb;
            if (! is) return false;
            notes.push_back (n);
            newNext = std::max (newNext, n.id + 1);
        }
        else if (tag == "drawn")
        {
            size_t start = 0, count = 0;
            is >> start >> count;
            for (size_t k = 0; k < count; ++k)
            {
                float v;
                is >> v;
                if (start + k < newDrawn.size()) newDrawn[start + k] = v;
            }
            if (! is) return false;
        }
        else
            return false;
    }

    noteList = std::move (notes);
    drawn = std::move (newDrawn);
    nextId = newNext;
    sortNotes();
    undoStack.clear();
    redoStack.clear();
    touch();
    return true;
}

} // namespace claudetune::edit
