#include "NoteSegmenter.h"

#include <algorithm>
#include <cmath>

namespace claudetune::edit
{

float steadyCentre (const PitchTrack& track, int start, int end)
{
    std::vector<float> v;
    const int len = end - start;
    const int trim = (int) (len * 0.15);
    for (int f = start + trim; f < end - trim; ++f)
        if (track.voiced (f))
            v.push_back (track.frames[(size_t) f].midi);
    if (v.empty())
        for (int f = start; f < end; ++f)
            if (track.voiced (f))
                v.push_back (track.frames[(size_t) f].midi);
    if (v.empty())
        return -1.0f;
    std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
    return v[v.size() / 2];
}

namespace
{
    struct Run { int start, end; };

    std::vector<Run> voicedRuns (const PitchTrack& t, int bridge)
    {
        std::vector<Run> runs;
        const int n = t.numFrames();
        for (int f = 0; f < n;)
        {
            if (! t.voiced (f)) { ++f; continue; }
            int e = f;
            while (e < n && t.voiced (e)) ++e;
            if (! runs.empty())
            {
                auto& last = runs.back();
                const int gap = f - last.end;
                if (gap <= bridge && std::abs (t.frames[(size_t) f].midi - t.frames[(size_t) last.end - 1].midi) < 1.0f)
                {
                    last.end = e;
                    f = e;
                    continue;
                }
            }
            runs.push_back ({ f, e });
            f = e;
        }
        return runs;
    }

    float median (std::vector<float> v)
    {
        if (v.empty()) return 0.0f;
        std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
        return v[v.size() / 2];
    }
}

void expandToVoicedRuns (const PitchTrack& track, const SegmentSettings& s, int& first, int& end)
{
    const double frameSec = track.frameToSeconds (1.0);
    const int bridge = std::max (1, (int) std::lround (s.bridgeGapSec / frameSec));
    first = std::clamp (first, 0, track.numFrames());
    end = std::clamp (end, first, track.numFrames());
    for (const auto& run : voicedRuns (track, bridge))
    {
        if (run.end <= first || run.start >= end)
            continue;
        first = std::min (first, run.start);
        end = std::max (end, run.end);
    }
    // runs touching the edges also count (a note can't be split by the range edge)
    for (const auto& run : voicedRuns (track, bridge))
    {
        if (run.end == first) first = run.start;
        if (run.start == end) end = run.end;
    }
}

std::vector<DetectedNote> segmentNotes (const PitchTrack& track, const SegmentSettings& s, int firstFrame, int endFrame)
{
    const double frameSec = track.frameToSeconds (1.0);
    const int bridge = std::max (1, (int) std::lround (s.bridgeGapSec / frameSec));
    const int hold = std::max (2, (int) std::lround (s.changeHoldSec / frameSec));
    const int minNote = std::max (3, (int) std::lround (s.minNoteSec / frameSec));
    const int avgLen = std::max (5, (int) std::lround (0.180 / frameSec));
    const int dipReach = std::max (4, (int) std::lround (0.120 / frameSec));
    if (endFrame < 0) endFrame = track.numFrames();

    std::vector<DetectedNote> notes;

    for (auto run : voicedRuns (track, bridge))
    {
        run.start = std::max (run.start, firstFrame);
        run.end = std::min (run.end, endFrame);
        if (run.end <= run.start)
            continue;
        const int len = run.end - run.start;

        // pitch with bridged gaps filled in
        std::vector<float> p ((size_t) len);
        float lastVal = -1.0f;
        for (int i = 0; i < len; ++i)
        {
            const int f = run.start + i;
            if (track.voiced (f)) lastVal = track.frames[(size_t) f].midi;
            p[(size_t) i] = lastVal;
        }

        // median-7 smoothing
        std::vector<float> ps ((size_t) len);
        for (int i = 0; i < len; ++i)
        {
            std::vector<float> w;
            for (int k = std::max (0, i - 3); k <= std::min (len - 1, i + 3); ++k)
                w.push_back (p[(size_t) k]);
            ps[(size_t) i] = median (w);
        }

        // vibrato-free centre track: centred moving average
        std::vector<float> cs ((size_t) len);
        {
            std::vector<double> prefix ((size_t) len + 1, 0.0);
            for (int i = 0; i < len; ++i) prefix[(size_t) i + 1] = prefix[(size_t) i] + ps[(size_t) i];
            for (int i = 0; i < len; ++i)
            {
                const int a = std::max (0, i - avgLen / 2), b = std::min (len, i + avgLen / 2 + 1);
                cs[(size_t) i] = (float) ((prefix[(size_t) b] - prefix[(size_t) a]) / (b - a));
            }
        }

        // --- pitch-change boundaries
        std::vector<int> bounds { 0 };
        double sum = 0.0;
        int count = 0, over = 0;
        for (int i = 0; i < len; ++i)
        {
            if (count > 0 && std::abs (cs[(size_t) i] - sum / count) > s.changeThreshold)
                ++over;
            else
            {
                over = 0;
                sum += cs[(size_t) i];
                ++count;
            }

            if (over >= hold)
            {
                // the real change is where the (smoothed) pitch moves fastest
                const int from = std::max (bounds.back() + 1, i - over - avgLen / 2 - 6);
                int best = std::max (from, 1);
                float bestVel = -1.0f;
                for (int k = std::max (from, 1); k < std::min (i, len - 1); ++k)
                {
                    const float vel = std::abs (ps[(size_t) k + 1] - ps[(size_t) k - 1]);
                    if (vel > bestVel) { bestVel = vel; best = k; }
                }
                bounds.push_back (best);
                sum = 0.0;
                count = 0;
                for (int k = best; k <= i; ++k) { sum += cs[(size_t) k]; ++count; }
                over = 0;
            }
        }
        bounds.push_back (len);

        // --- energy dips split repeated notes
        std::vector<int> all;
        for (size_t b = 0; b + 1 < bounds.size(); ++b)
        {
            all.push_back (bounds[b]);
            const int a = bounds[b], e = bounds[b + 1];
            for (int i = a + minNote; i < e - minNote; ++i)
            {
                const float r = track.frames[(size_t) (run.start + i)].rmsDb;
                float leftMax = -200.0f, rightMax = -200.0f;
                bool isMin = true;
                for (int k = std::max (a, i - dipReach); k < i; ++k)
                {
                    leftMax = std::max (leftMax, track.frames[(size_t) (run.start + k)].rmsDb);
                    if (track.frames[(size_t) (run.start + k)].rmsDb < r) isMin = false;
                }
                for (int k = i + 1; k <= std::min (e - 1, i + dipReach); ++k)
                {
                    rightMax = std::max (rightMax, track.frames[(size_t) (run.start + k)].rmsDb);
                    if (track.frames[(size_t) (run.start + k)].rmsDb < r) isMin = false;
                }
                if (isMin && leftMax - r >= s.energyDipDb && rightMax - r >= s.energyDipDb)
                {
                    all.push_back (i);
                    i += minNote;
                }
            }
        }
        all.push_back (len);

        // --- build notes for this run, merging slivers into a neighbour
        std::vector<DetectedNote> local;
        for (size_t b = 0; b + 1 < all.size(); ++b)
            if (all[b + 1] > all[b])
                local.push_back ({ run.start + all[b], run.start + all[b + 1], 0.0f });

        for (auto& n : local)
            n.centre = steadyCentre (track, n.startFrame, n.endFrame);

        bool merged = true;
        while (merged && local.size() > 1)
        {
            merged = false;
            for (size_t i = 0; i < local.size(); ++i)
            {
                if (local[i].endFrame - local[i].startFrame >= minNote)
                    continue;
                size_t target;
                if (i == 0) target = 1;
                else if (i + 1 == local.size()) target = i - 1;
                else
                    target = std::abs (local[i - 1].centre - local[i].centre) <= std::abs (local[i + 1].centre - local[i].centre)
                                 ? i - 1 : i + 1;
                auto& t = local[target];
                t.startFrame = std::min (t.startFrame, local[i].startFrame);
                t.endFrame = std::max (t.endFrame, local[i].endFrame);
                t.centre = steadyCentre (track, t.startFrame, t.endFrame);
                local.erase (local.begin() + (long) i);
                merged = true;
                break;
            }
        }

        for (auto& n : local)
            if (n.endFrame - n.startFrame >= minNote * 2 / 3 && n.centre >= 0.0f)
                notes.push_back (n);
    }

    return notes;
}

} // namespace claudetune::edit
