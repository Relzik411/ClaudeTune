#pragma once
// ClaudeTune - scale definitions and per-note state helpers.

#include <array>
#include <cstdint>

namespace claudetune
{

enum class NoteState : uint8_t
{
    Legal       = 0, // can be a correction target
    Illegal     = 1, // corrected to the nearest legal note
    IllegalUp   = 2, // corrected to the nearest legal note above
    IllegalDown = 3, // corrected to the nearest legal note below
    Bypass      = 4  // passes through uncorrected
};

constexpr int kNumNoteStates = 5;

inline NoteState nextNoteState (NoteState s) noexcept
{
    return (NoteState) (((int) s + 1) % kNumNoteStates);
}

struct ScaleDef
{
    const char* name;
    int numNotes;
    int intervals[12];
};

// Index order is stored in the "scale" parameter - only append, never reorder.
inline constexpr ScaleDef kScales[] = {
    { "Chromatic",         12, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } },
    { "Major",              7, { 0, 2, 4, 5, 7, 9, 11 } },
    { "Natural Minor",      7, { 0, 2, 3, 5, 7, 8, 10 } },
    { "Harmonic Minor",     7, { 0, 2, 3, 5, 7, 8, 11 } },
    { "Melodic Minor",      7, { 0, 2, 3, 5, 7, 9, 11 } },
    { "Major Pentatonic",   5, { 0, 2, 4, 7, 9 } },
    { "Minor Pentatonic",   5, { 0, 3, 5, 7, 10 } },
    { "Blues",              6, { 0, 3, 5, 6, 7, 10 } },
    { "Dorian",             7, { 0, 2, 3, 5, 7, 9, 10 } },
    { "Phrygian",           7, { 0, 1, 3, 5, 7, 8, 10 } },
    { "Lydian",             7, { 0, 2, 4, 6, 7, 9, 11 } },
    { "Mixolydian",         7, { 0, 2, 4, 5, 7, 9, 10 } },
    { "Locrian",            7, { 0, 1, 3, 5, 6, 8, 10 } },
    { "Whole Tone",         6, { 0, 2, 4, 6, 8, 10 } },
    { "User",               0, {} }, // every note starts illegal; click notes to allow them
};

inline constexpr int kNumScales = (int) (sizeof (kScales) / sizeof (kScales[0]));

using NoteStateArray = std::array<NoteState, 128>;

inline NoteStateArray buildScale (int scaleIndex, int root) noexcept
{
    NoteStateArray states {};
    bool inScale[12] = {};
    const auto& def = kScales[(scaleIndex >= 0 && scaleIndex < kNumScales) ? scaleIndex : 0];
    for (int i = 0; i < def.numNotes; ++i)
        inScale[(def.intervals[i] + root) % 12] = true;

    for (int n = 0; n < 128; ++n)
        states[(size_t) n] = inScale[n % 12] ? NoteState::Legal : NoteState::Illegal;
    return states;
}

inline const char* noteName (int midiNote, bool useFlats) noexcept
{
    static const char* sharps[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static const char* flats[]  = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
    const int pc = ((midiNote % 12) + 12) % 12;
    return useFlats ? flats[pc] : sharps[pc];
}

// Vocal range presets as {lowest MIDI note, highest MIDI note}
struct RangePreset
{
    const char* name;
    int low, high;
};

inline constexpr RangePreset kRangePresets[] = {
    { "Bass",          36, 64 }, // C2 - E4
    { "Baritone",      41, 67 }, // F2 - G4
    { "Tenor",         47, 72 }, // B2 - C5
    { "Alto",          52, 77 }, // E3 - F5
    { "Mezzo Soprano", 55, 81 }, // G3 - A5
    { "Soprano",       59, 84 }, // B3 - C6
    { "Generic",       36, 84 }, // C2 - C6
};

inline constexpr int kNumRangePresets = (int) (sizeof (kRangePresets) / sizeof (kRangePresets[0]));

constexpr int kKeyboardLow  = 24; // C1
constexpr int kKeyboardHigh = 96; // C7

} // namespace claudetune
