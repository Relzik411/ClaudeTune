#pragma once
// ClaudeTune editor - renders the edited output with pitch-synchronous
// overlap-add (PSOLA), including timing edits.
//
// Audio the user hasn't touched is copied straight from the source, so it
// stays bit-identical. Edited stretches are rendered as whole regions and
// crossfaded in over 20 ms of untouched audio, which keeps re-renders seamless.

#include "EditModel.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace claudetune::edit
{

class Renderer
{
public:
    // Builds the analysis marks for the whole document.
    void prepare (const EditDocument& doc);

    // After EditDocument::replaceAudio(): rebuilds the marks around the
    // changed samples (and for any growth) without touching the rest.
    void refreshMarks (const EditDocument& doc, SampleRange changed);

    // Renders the whole output. out is resized to (channels x numSamples).
    void renderAll (const EditDocument& doc, const RenderCurves& curves,
                    std::vector<std::vector<float>>& out);

    // Re-renders only what changed between two sets of curves, plus any
    // forced ranges (e.g. where the source audio changed). Grows out if the
    // document grew. Returns the output ranges that were rewritten.
    std::vector<SampleRange> update (const EditDocument& doc, const RenderCurves& before,
                                     const RenderCurves& after, std::vector<std::vector<float>>& out,
                                     std::vector<SampleRange> forced = {});

    // Output ranges whose audio differs from a plain copy of the source.
    std::vector<SampleRange> editedRegions (const RenderCurves& curves) const;

    size_t numMarks() const noexcept { return markPos.size(); }
    // For tests: true if marks match a fresh prepare() exactly.
    bool marksEqual (const Renderer& other) const noexcept
    {
        return markPos == other.markPos && markPeriod == other.markPeriod && markVoiced == other.markVoiced;
    }

private:
    struct Mark
    {
        double pos;
        float period;
        bool voiced;
    };

    void configure (const EditDocument& doc);
    Mark makeMark (const EditDocument& doc, double pos) const noexcept;
    double nextMarkPos (const EditDocument& doc, const Mark& m) const noexcept;
    std::vector<float> editedWeight (const RenderCurves& curves) const; // per output frame
    void renderSpan (const EditDocument& doc, const RenderCurves& curves, const std::vector<float>& weight,
                     SampleRange span, std::vector<std::vector<float>>& out);
    void renderRegion (const EditDocument& doc, const RenderCurves& curves, const std::vector<float>& weight,
                       SampleRange region, std::vector<std::vector<float>>& out);
    size_t nearestMark (double pos) const noexcept;

    double sr = 48000.0;
    int hop = 240;
    int64_t length = 0;
    int maxPeriod = 800;
    int unvoicedPeriod = 240;
    std::vector<double> markPos;
    std::vector<float> markPeriod;
    std::vector<uint8_t> markVoiced;
    std::vector<std::vector<float>> scratch;

    static constexpr int kRampFrames = 4;  // 20 ms crossfade into edited regions
};

} // namespace claudetune::edit
