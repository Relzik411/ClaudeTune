#pragma once
// ClaudeTune editor - splits a pitch track into note blobs.

#include "PitchAnalysis.h"
#include <vector>

namespace claudetune::edit
{

struct DetectedNote
{
    int startFrame = 0;  // inclusive
    int endFrame = 0;    // exclusive
    float centre = 0.0f; // median pitch of the steady part (MIDI)
};

struct SegmentSettings
{
    float changeThreshold = 0.6f;  // semitones the vibrato-free pitch must move to start a new note
    double changeHoldSec  = 0.040; // ... for at least this long
    double minNoteSec     = 0.060; // shorter pieces merge into a neighbour
    double bridgeGapSec   = 0.015; // tiny unvoiced gaps inside a note are bridged
    float energyDipDb     = 9.0f;  // a loudness dip this deep splits repeated notes
};

// Segments the voiced runs of frames [firstFrame, endFrame) (endFrame < 0 = all).
// Runs crossing the range edges are cut at the edges.
std::vector<DetectedNote> segmentNotes (const PitchTrack& track, const SegmentSettings& settings = {},
                                        int firstFrame = 0, int endFrame = -1);

// Widens [first, end) so it doesn't cut through a voiced run (same gap
// bridging as the segmenter).
void expandToVoicedRuns (const PitchTrack& track, const SegmentSettings& settings, int& first, int& end);

// Median pitch of frames [start, end) ignoring the outer 15% on each side.
float steadyCentre (const PitchTrack& track, int start, int end);

} // namespace claudetune::edit
