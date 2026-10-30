#include "Renderer.h"

#include <algorithm>
#include <cmath>

namespace claudetune::edit
{

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    inline double midiToHz (double m) noexcept { return 440.0 * std::pow (2.0, (m - 69.0) / 12.0); }

    inline float sampleAt (const std::vector<float>& x, double pos) noexcept
    {
        const double fl = std::floor (pos);
        const int64_t i0 = (int64_t) fl;
        if (i0 < 0 || i0 + 1 >= (int64_t) x.size())
            return (i0 >= 0 && i0 < (int64_t) x.size()) ? x[(size_t) i0] : 0.0f;
        const float f = (float) (pos - fl);
        return x[(size_t) i0] + f * (x[(size_t) i0 + 1] - x[(size_t) i0]);
    }

    std::vector<SampleRange> mergeRanges (std::vector<SampleRange> r)
    {
        std::sort (r.begin(), r.end());
        std::vector<SampleRange> out;
        for (const auto& x : r)
        {
            if (x.second <= x.first) continue;
            if (! out.empty() && x.first <= out.back().second)
                out.back().second = std::max (out.back().second, x.second);
            else
                out.push_back (x);
        }
        return out;
    }
}

//==============================================================================
// Analysis marks: one per pitch period in voiced audio (locked to the
// waveform), and on a fixed grid in unvoiced audio. The grid means two mark
// chains always agree again after the next unvoiced stretch, which is what
// lets refreshMarks() stop early.

void Renderer::configure (const EditDocument& doc)
{
    sr = doc.sampleRate();
    hop = doc.pitchTrack().hop;
    length = doc.numSamples();
    maxPeriod = (int) std::ceil (sr / 55.0);
    unvoicedPeriod = std::max (2, (int) std::lround (sr / 200.0));
    if (scratch.size() != (size_t) doc.numChannels())
        scratch.assign ((size_t) doc.numChannels(), {});
}

Renderer::Mark Renderer::makeMark (const EditDocument& doc, double pos) const noexcept
{
    const float p = doc.pitchTrack().pitchAtSample (pos);
    if (p < 0.0f)
        return { pos, (float) unvoicedPeriod, false };
    return { pos, (float) std::clamp (sr / midiToHz (p), 2.0, (double) maxPeriod), true };
}

double Renderer::nextMarkPos (const EditDocument& doc, const Mark& m) const noexcept
{
    if (! m.voiced)
        return (std::floor (m.pos / unvoicedPeriod) + 1.0) * unvoicedPeriod;

    const auto& x = doc.monoAudio();
    double next = m.pos + m.period;
    if (doc.pitchTrack().pitchAtSample (next) >= 0.0f)
    {
        // lock the next mark to the waveform shape around this one
        const int half = std::max (1, (int) (m.period * 0.5f));
        const int s = std::min (std::max (1, (int) (m.period / 16.0f)), 64);
        double scores[2 * 64 + 1];
        int bestIdx = 0;
        for (int idx = 0; idx <= 2 * s; ++idx)
        {
            const double cand = next + (idx - s);
            double dot = 0.0, energy = 1e-12;
            for (int k = -half; k < half; ++k)
            {
                const float a = sampleAt (x, m.pos + k);
                const float b = sampleAt (x, cand + k);
                dot += (double) a * b;
                energy += (double) b * b;
            }
            scores[idx] = dot / std::sqrt (energy);
            if (scores[idx] > scores[bestIdx]) bestIdx = idx;
        }
        double refined = bestIdx - s;
        if (bestIdx > 0 && bestIdx < 2 * s)
        {
            const double y0 = scores[bestIdx - 1], y1 = scores[bestIdx], y2 = scores[bestIdx + 1];
            const double den = y0 - 2.0 * y1 + y2;
            if (std::abs (den) > 1e-12) refined += std::clamp (0.5 * (y0 - y2) / den, -0.5, 0.5);
        }
        next += refined;
    }
    return std::max (next, m.pos + 1.0);
}

void Renderer::prepare (const EditDocument& doc)
{
    configure (doc);
    markPos.clear();
    markPeriod.clear();
    markVoiced.clear();

    double pos = 0.0;
    while (pos < (double) length || markPos.empty())
    {
        const auto m = makeMark (doc, pos);
        markPos.push_back (m.pos);
        markPeriod.push_back (m.period);
        markVoiced.push_back (m.voiced ? 1 : 0);
        pos = nextMarkPos (doc, m);
        if (length == 0) break;
    }
}

void Renderer::refreshMarks (const EditDocument& doc, SampleRange changed)
{
    if (markPos.empty())
    {
        prepare (doc);
        return;
    }

    const int64_t oldLength = length;
    configure (doc);

    // marks near the change depend on the re-analysed pitch (up to ~0.35 s
    // either side) and on the waveform within one period
    const double pad = 0.6 * sr + 2.0 * maxPeriod;
    const double lo = (double) std::min<int64_t> (changed.first, oldLength) - pad;
    const double hi = (double) changed.second + pad;

    // keep every mark before lo; restart the chain from the last of them
    auto it = std::lower_bound (markPos.begin(), markPos.end(), lo);
    size_t keep = (size_t) (it - markPos.begin());
    if (keep == 0)
    {
        prepare (doc);
        return;
    }

    std::vector<double> oldPos (markPos.begin() + (long) keep, markPos.end());
    std::vector<float> oldPeriod (markPeriod.begin() + (long) keep, markPeriod.end());
    std::vector<uint8_t> oldVoiced (markVoiced.begin() + (long) keep, markVoiced.end());
    markPos.resize (keep);
    markPeriod.resize (keep);
    markVoiced.resize (keep);

    const double oldEnd = (double) oldLength;
    Mark m { markPos.back(), markPeriod.back(), markVoiced.back() != 0 };
    double pos = nextMarkPos (doc, m);
    size_t oi = 0;

    while (pos < (double) length)
    {
        // past the change and back on the shared grid: the old chain is valid again
        if (pos > hi && pos < oldEnd - pad)
        {
            while (oi < oldPos.size() && oldPos[oi] < pos - 1e-6) ++oi;
            if (oi < oldPos.size() && std::abs (oldPos[oi] - pos) < 1e-6 && oldVoiced[oi] == 0)
            {
                const auto nm = makeMark (doc, pos);
                if (! nm.voiced)
                {
                    markPos.insert (markPos.end(), oldPos.begin() + (long) oi, oldPos.end());
                    markPeriod.insert (markPeriod.end(), oldPeriod.begin() + (long) oi, oldPeriod.end());
                    markVoiced.insert (markVoiced.end(), oldVoiced.begin() + (long) oi, oldVoiced.end());
                    // the old chain stopped at the old end; continue it if the document grew
                    if (length > oldLength)
                    {
                        Mark last { markPos.back(), markPeriod.back(), markVoiced.back() != 0 };
                        double p = nextMarkPos (doc, last);
                        while (p < (double) length)
                        {
                            const auto mm = makeMark (doc, p);
                            markPos.push_back (mm.pos);
                            markPeriod.push_back (mm.period);
                            markVoiced.push_back (mm.voiced ? 1 : 0);
                            p = nextMarkPos (doc, mm);
                        }
                    }
                    return;
                }
            }
        }

        m = makeMark (doc, pos);
        markPos.push_back (m.pos);
        markPeriod.push_back (m.period);
        markVoiced.push_back (m.voiced ? 1 : 0);
        pos = nextMarkPos (doc, m);
    }
}

size_t Renderer::nearestMark (double pos) const noexcept
{
    const auto it = std::lower_bound (markPos.begin(), markPos.end(), pos);
    if (it == markPos.begin()) return 0;
    if (it == markPos.end()) return markPos.size() - 1;
    const size_t j = (size_t) (it - markPos.begin());
    return (pos - markPos[j - 1] <= markPos[j] - pos) ? j - 1 : j;
}

//==============================================================================
std::vector<float> Renderer::editedWeight (const RenderCurves& c) const
{
    const int nf = (int) (length / hop) + 2;
    std::vector<uint8_t> raw ((size_t) nf, 0);
    for (int g = 0; g < nf; ++g)
    {
        const double t = (double) g * hop;
        const double src = c.map.outToSrc (t);
        const bool warped = std::abs (src - t) > 0.25 || std::abs (c.map.slopeAtOut (t) - 1.0) > 1e-6;
        const bool shifted = std::abs (c.shiftAtSample (src)) > 1e-4f;
        const bool gained = std::abs (c.gainAtSample (src)) > 1e-3f;
        raw[(size_t) g] = (warped || shifted || gained) ? 1 : 0;
    }

    // dilate, then average: weight is exactly 1 on every edited frame and
    // ramps to 0 across untouched audio next to it
    std::vector<float> core ((size_t) nf, 0.0f);
    for (int g = 0; g < nf; ++g)
        if (raw[(size_t) g])
            for (int k = std::max (0, g - kRampFrames); k <= std::min (nf - 1, g + kRampFrames); ++k)
                core[(size_t) k] = 1.0f;

    std::vector<float> w ((size_t) nf, 0.0f);
    for (int g = 0; g < nf; ++g)
    {
        float s = 0.0f;
        for (int k = g - kRampFrames; k <= g + kRampFrames; ++k)
            s += (k >= 0 && k < nf) ? core[(size_t) k] : 0.0f;
        w[(size_t) g] = s / (float) (2 * kRampFrames + 1);
    }
    return w;
}

std::vector<SampleRange> Renderer::editedRegions (const RenderCurves& c) const
{
    const auto w = editedWeight (c);
    std::vector<SampleRange> regions;
    const int nf = (int) w.size();
    for (int g = 0; g < nf;)
    {
        if (w[(size_t) g] <= 0.0f) { ++g; continue; }
        int e = g;
        while (e < nf && w[(size_t) e] > 0.0f) ++e;
        const int64_t a = std::max<int64_t> (0, (int64_t) (g - 1) * hop);
        const int64_t b = std::min<int64_t> (length, (int64_t) e * hop);
        if (b > a) regions.push_back ({ a, b });
        g = e;
    }
    return regions;
}

//==============================================================================
void Renderer::renderAll (const EditDocument& doc, const RenderCurves& curves, std::vector<std::vector<float>>& out)
{
    configure (doc);
    out.resize ((size_t) doc.numChannels());
    for (auto& ch : out)
        ch.assign ((size_t) length, 0.0f);
    renderSpan (doc, curves, editedWeight (curves), { 0, length }, out);
}

std::vector<SampleRange> Renderer::update (const EditDocument& doc, const RenderCurves& before,
                                           const RenderCurves& after, std::vector<std::vector<float>>& out,
                                           std::vector<SampleRange> forced)
{
    configure (doc);

    if (out.size() != (size_t) doc.numChannels() || out.empty())
    {
        renderAll (doc, after, out);
        return { { 0, length } };
    }

    // grow the output with untouched source audio
    const auto oldLen = (int64_t) out[0].size();
    if (oldLen != length)
    {
        const auto& src = doc.sourceAudio();
        for (size_t c = 0; c < out.size(); ++c)
        {
            out[c].resize ((size_t) length, 0.0f);
            if (length > oldLen)
                std::copy (src[c].begin() + oldLen, src[c].begin() + length, out[c].begin() + oldLen);
        }
        if (length > oldLen)
            forced.push_back ({ oldLen, length });
    }

    // 1. output frames whose (source position, shift, gain) changed
    const int nf = (int) (length / hop) + 2;
    const int pad = kRampFrames * 2 + 2;
    std::vector<SampleRange> dirty;
    auto differs = [&] (int f) {
        const double t = (double) f * hop;
        const double sb = before.map.outToSrc (t), sa = after.map.outToSrc (t);
        return std::abs (sb - sa) > 0.01
               || std::abs (before.shiftAtSample (sb) - after.shiftAtSample (sa)) > 1e-5f
               || std::abs (before.gainAtSample (sb) - after.gainAtSample (sa)) > 1e-4f;
    };
    for (int g = 0; g < nf;)
    {
        if (! differs (g)) { ++g; continue; }
        int e = g;
        while (e < nf && differs (e)) ++e;
        dirty.push_back ({ std::max<int64_t> (0, (int64_t) (g - pad) * hop),
                           std::min<int64_t> (length, (int64_t) (e + pad) * hop) });
        g = e;
    }

    // forced ranges are in source time: cover the output positions that read them
    for (const auto& f : forced)
    {
        const double a = after.map.srcToOut ((double) f.first), b = after.map.srcToOut ((double) f.second);
        const double a0 = before.map.srcToOut ((double) f.first), b0 = before.map.srcToOut ((double) f.second);
        const double lo = std::min ({ a, a0, (double) f.first }), hi = std::max ({ b, b0, (double) f.second });
        dirty.push_back ({ std::max<int64_t> (0, (int64_t) std::floor (lo) - (int64_t) pad * hop),
                           std::min<int64_t> (length, (int64_t) std::ceil (hi) + (int64_t) pad * hop) });
    }

    dirty = mergeRanges (dirty);
    if (dirty.empty())
        return {};

    // 2. grow each dirty span to whole edited regions so every region is
    //    rendered from its own start (seamless, deterministic)
    const auto weight = editedWeight (after);
    const auto regions = editedRegions (after);
    bool grew = true;
    while (grew)
    {
        grew = false;
        for (auto& d : dirty)
            for (const auto& r : regions)
                if (r.first < d.second && r.second > d.first && (r.first < d.first || r.second > d.second))
                {
                    d.first = std::min (d.first, r.first);
                    d.second = std::max (d.second, r.second);
                    grew = true;
                }
        dirty = mergeRanges (dirty);
    }

    for (const auto& d : dirty)
        renderSpan (doc, after, weight, d, out);
    return dirty;
}

void Renderer::renderSpan (const EditDocument& doc, const RenderCurves& curves, const std::vector<float>& weight,
                           SampleRange span, std::vector<std::vector<float>>& out)
{
    const auto& src = doc.sourceAudio();
    for (size_t c = 0; c < out.size(); ++c)
        std::copy (src[c].begin() + span.first, src[c].begin() + span.second, out[c].begin() + span.first);

    const int nf = (int) weight.size();
    for (int g = 0; g < nf;)
    {
        if (weight[(size_t) g] <= 0.0f) { ++g; continue; }
        int e = g;
        while (e < nf && weight[(size_t) e] > 0.0f) ++e;
        const SampleRange region { std::max<int64_t> (0, (int64_t) (g - 1) * hop),
                                   std::min<int64_t> (length, (int64_t) e * hop) };
        if (region.first >= span.first && region.second <= span.second && region.second > region.first)
            renderRegion (doc, curves, weight, region, out);
        g = e;
    }
}

void Renderer::renderRegion (const EditDocument& doc, const RenderCurves& curves, const std::vector<float>& weight,
                             SampleRange region, std::vector<std::vector<float>>& out)
{
    const auto& src = doc.sourceAudio();
    const auto& track = doc.pitchTrack();
    const int channels = (int) out.size();
    const int64_t pad = 2 * (int64_t) maxPeriod;
    const int64_t base = region.first - pad;
    const int64_t len = (region.second - region.first) + 2 * pad;

    for (int c = 0; c < channels; ++c)
        scratch[(size_t) c].assign ((size_t) len, 0.0f);

    // synthesis grid anchored at the region start
    double t = (double) base;
    while (t < (double) (region.second + pad))
    {
        const double srcPos = curves.map.outToSrc (t);
        const size_t m = nearestMark (srcPos);
        const bool voiced = markVoiced[m] != 0;
        // voiced grains come from a pitch mark; unvoiced ones straight from the source position
        const double mark = voiced ? markPos[m] : srcPos;
        double synthPeriod = voiced ? (double) markPeriod[m] : (double) unvoicedPeriod;

        if (voiced)
        {
            const float p = track.pitchAtSample (srcPos);
            if (p >= 0.0f)
                synthPeriod = sr / midiToHz (p + curves.shiftAtSample (srcPos));
        }
        synthPeriod = std::clamp (synthPeriod, 2.0, (double) maxPeriod);

        const float gain = std::pow (10.0f, curves.gainAtSample (srcPos) / 20.0f);
        const double half = synthPeriod;
        const int64_t t0 = (int64_t) std::floor (t);
        const double frac = t - (double) t0;
        const double invHalf = kPi / half;

        for (int k = (int) std::ceil (-half + frac); k <= (int) std::floor (half + frac); ++k)
        {
            const double u = (double) k - frac;
            if (u <= -half || u >= half) continue;
            const int64_t o = t0 + k - base;
            if (o < 0 || o >= len) continue;
            const float w = gain * (float) (0.5 + 0.5 * std::cos (u * invHalf));
            for (int c = 0; c < channels; ++c)
                scratch[(size_t) c][(size_t) o] += w * sampleAt (src[(size_t) c], mark + u);
        }

        t += synthPeriod;
    }

    // blend into the output with the edit weight
    for (int64_t i = region.first; i < region.second; ++i)
    {
        const double fpos = (double) i / hop;
        const auto g0 = (size_t) fpos;
        const float fr = (float) (fpos - (double) g0);
        const float w0 = weight[std::min (g0, weight.size() - 1)];
        const float w1 = weight[std::min (g0 + 1, weight.size() - 1)];
        const float w = w0 + fr * (w1 - w0);
        for (int c = 0; c < channels; ++c)
        {
            const float rendered = scratch[(size_t) c][(size_t) (i - base)];
            const float orig = src[(size_t) c][(size_t) i];
            out[(size_t) c][(size_t) i] = orig + w * (rendered - orig);
        }
    }
}

} // namespace claudetune::edit
