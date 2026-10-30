#pragma once
// ClaudeTune editor - the editable document: source audio, its analysis,
// the note blobs and every edit made to them. Edits are non-destructive:
// the renderer turns (source + notes) into the output audio.
//
// The document's sample 0 is the host timeline's sample 0. Audio arrives in
// pieces (replaceAudio) as the host plays; anything not captured yet is
// silence. Re-analysis after new audio is local, and edits on notes whose
// audio didn't change are kept.

#include "PitchAnalysis.h"
#include "NoteSegmenter.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace claudetune::edit
{

using SampleRange = std::pair<int64_t, int64_t>; // [start, end) in samples

struct Note
{
    uint32_t id = 0;

    // where the note sits in the source audio (seconds)
    double srcStart = 0.0, srcEnd = 0.0;
    // where it plays in the output (seconds) - differs after timing edits
    double outStart = 0.0, outEnd = 0.0;

    float detectedPitch = 0.0f; // steady pitch the singer produced (MIDI)
    float targetPitch = 0.0f;   // where the note should sit (MIDI); = detected when untouched

    float center = 0.0f;        // 0..1: removes slow drift around the note centre
    float variation = 1.0f;     // 0..2: scales fast wobble/vibrato (1 = keep, 0 = flat)
    float transitionInMs = 30.0f;
    float transitionOutMs = 30.0f;
    float gainDb = 0.0f;

    bool operator== (const Note&) const = default;

    bool hasTimingEdit() const noexcept
    {
        return std::abs (outStart - srcStart) > 1e-9 || std::abs (outEnd - srcEnd) > 1e-9;
    }
    bool hasAnyEdit() const noexcept
    {
        return hasTimingEdit() || std::abs (targetPitch - detectedPitch) > 1e-6f || std::abs (center) > 1e-6f
               || std::abs (variation - 1.0f) > 1e-6f || std::abs (transitionInMs - 30.0f) > 1e-3f
               || std::abs (transitionOutMs - 30.0f) > 1e-3f || std::abs (gainDb) > 1e-6f;
    }
};

// Piecewise-linear mapping between output time and source time (samples).
class TimeMap
{
public:
    std::vector<std::pair<double, double>> points; // (out, src), sorted by out

    double outToSrc (double out) const noexcept;
    double srcToOut (double src) const noexcept;
    double slopeAtOut (double out) const noexcept; // d src / d out
};

// Everything the renderer needs, sampled per analysis frame (source time).
struct RenderCurves
{
    double sampleRate = 48000.0;
    int hop = 240;
    int64_t numSamples = 0;
    std::vector<float> shift;   // semitones to add, per source frame
    std::vector<float> gainDb;  // per source frame
    TimeMap map;

    float shiftAtSample (double srcPos) const noexcept;
    float gainAtSample (double srcPos) const noexcept;
};

struct ReplaceResult
{
    bool audioChanged = false;     // some samples differ from before
    SampleRange changed { 0, 0 };  // samples whose audio changed
    int64_t oldLength = 0, newLength = 0;
    bool replacedSignal = false;   // the changed audio wasn't silence before (a re-take)
};

class EditDocument
{
public:
    // Loads audio (one vector per channel) and analyses it. Clears all edits.
    void load (std::vector<std::vector<float>> channels, double sampleRate,
               const AnalysisSettings& analysis = {}, const SegmentSettings& segmentation = {});

    // Empty document that grows as audio is captured.
    void initEmpty (int numChannels, double sampleRate,
                    const AnalysisSettings& analysis = {}, const SegmentSettings& segmentation = {});

    // Restores audio and an existing analysis without re-analysing (project load).
    // Follow with restoreEdits().
    void loadWithTrack (std::vector<std::vector<float>> channels, double sampleRate, std::vector<PitchFrame> frames,
                        const AnalysisSettings& analysis = {}, const SegmentSettings& segmentation = {});

    // Writes audio at a timeline position (growing the document if needed),
    // re-analyses around it and keeps the edits of notes whose audio didn't change.
    // audio may have fewer channels than the document; the last one is repeated.
    ReplaceResult replaceAudio (int64_t start, const std::vector<std::vector<float>>& audio);

    bool isEmpty() const noexcept { return length == 0; }
    double sampleRate() const noexcept { return sr; }
    int64_t numSamples() const noexcept { return length; }
    int numChannels() const noexcept { return (int) source.size(); }
    const std::vector<std::vector<float>>& sourceAudio() const noexcept { return source; }
    const std::vector<float>& monoAudio() const noexcept { return mono; }
    const PitchTrack& pitchTrack() const noexcept { return track; }
    const std::vector<Note>& notes() const noexcept { return noteList; }
    const Note* findNote (uint32_t id) const noexcept;
    int indexOf (uint32_t id) const noexcept;
    uint64_t revision() const noexcept { return rev; }
    uint64_t audioRevision() const noexcept { return audioRev; }

    // ---- undo: call beginEdit() once before each user gesture --------------
    void beginEdit();
    bool undo();
    bool redo();
    bool canUndo() const noexcept { return ! undoStack.empty(); }
    bool canRedo() const noexcept { return ! redoStack.empty(); }

    // ---- re-take safety net ----------------------------------------------------
    // When captured audio is replaced by different audio, the old audio and
    // edits are kept so the user can bring them back. Consecutive replacements
    // extend the same backup until sealCaptureBackup() is called.
    bool hasCaptureBackup() const noexcept { return backup.valid; }
    SampleRange captureBackupRange() const noexcept { return { backup.start, backup.start + backupLength() }; }
    void sealCaptureBackup() noexcept { backup.open = false; }
    // Puts the previous audio and its edits back; returns the restored range.
    SampleRange restoreCaptureBackup();

    // ---- edits ---------------------------------------------------------------
    bool setTargetPitch (uint32_t id, float target);
    bool setNoteShape (uint32_t id, float center, float variation);
    bool setTransitions (uint32_t id, float inMs, float outMs);
    bool setGain (uint32_t id, float gainDb);
    // Moves the note in time; neighbours joined to it (legato) stretch to follow.
    // Returns the delta actually applied (clamped to what is possible).
    double moveNoteTime (uint32_t id, double deltaSeconds);
    // Moves several notes together. Runs of neighbouring selected notes move as
    // one rigid block; the notes either side of a block stretch (legato) or
    // limit how far it can go. Returns the delta actually applied.
    double moveNotesTime (const std::vector<uint32_t>& ids, double deltaSeconds);
    // Sets the note's output start/end (stretch). Clamped; returns false if nothing changed.
    bool setNoteOutEdges (uint32_t id, double outStart, double outEnd);
    // Splits at a source time; returns the id of the new second half (0 on failure).
    uint32_t splitNote (uint32_t id, double srcTime);
    // Merges two neighbouring notes; returns the surviving id (0 on failure).
    uint32_t mergeNotes (uint32_t firstId, uint32_t secondId);
    // Snaps every note to the nearest allowed pitch and applies the macros.
    // allowed[n] says whether MIDI note n is a legal target.
    // Pass a list of note ids to correct only those notes.
    void correctAll (const std::array<bool, 128>& allowed, float center, float variation,
                     float referenceOffsetSemitones = 0.0f, const std::vector<uint32_t>* onlyThese = nullptr);
    // Nearest allowed note to a pitch (MIDI units, reference offset applied); -1 if none within an octave.
    static float nearestAllowed (float pitch, const std::array<bool, 128>& allowed, float referenceOffsetSemitones = 0.0f);
    void resetNote (uint32_t id);              // pitch, shape, gain, drawing and timing
    void resetAll();

    // hand-drawn pitch (per source frame, NaN = none)
    void setDrawnPitch (int startFrame, const std::vector<float>& values);
    void clearDrawnPitch (int startFrame, int endFrame);
    const std::vector<float>& drawnPitch() const noexcept { return drawn; }

    RenderCurves buildCurves() const;
    TimeMap buildTimeMap() const;

    // ---- persistence ---------------------------------------------------------
    std::string serialiseEdits() const;              // notes + drawn curve
    bool restoreEdits (const std::string& text);     // after load() of the same audio

    static constexpr double kMinNoteSeconds = 0.03;
    static constexpr double kMaxStretch = 4.0;

private:
    struct Snapshot
    {
        std::vector<Note> notes;
        std::vector<float> drawn;
        uint32_t nextId = 1;
    };

    struct CaptureBackup
    {
        bool valid = false, open = false;
        int64_t start = 0;
        std::vector<std::vector<float>> audio;
        int frameStart = 0;
        std::vector<PitchFrame> frames;
        Snapshot state;
    };

    Note* find (uint32_t id) noexcept;
    void touch() noexcept { ++rev; }
    void sortNotes();
    bool legatoJoined (const Note& a, const Note& b) const noexcept;
    void growTo (int64_t newLength);
    void recomputeMono (int64_t start, int64_t end);
    void sanitiseTiming();
    void extendBackup (int64_t start, int64_t end);
    int64_t backupLength() const noexcept { return backup.audio.empty() ? 0 : (int64_t) backup.audio[0].size(); }
    int noteStartFrame (const Note& n) const noexcept;
    int noteEndFrame (const Note& n) const noexcept;

    double sr = 48000.0;
    int64_t length = 0;
    AnalysisSettings analysisSettings;
    SegmentSettings segmentSettings;
    std::vector<std::vector<float>> source;
    std::vector<float> mono;
    PitchTrack track;
    std::vector<Note> noteList;
    std::vector<float> drawn;
    uint32_t nextId = 1;
    uint64_t rev = 0;
    uint64_t audioRev = 0;
    CaptureBackup backup;

    std::vector<Snapshot> undoStack, redoStack;
    static constexpr size_t kUndoLimit = 200;
};

} // namespace claudetune::edit
