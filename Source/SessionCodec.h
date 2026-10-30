#pragma once
// ClaudeTune - stores a captured editing session inside the host project.
// Audio is FLAC-compressed (24-bit, peak-normalised so nothing clips); the
// pitch analysis is gzip-compressed so a project opens without re-analysing.

#include <juce_audio_formats/juce_audio_formats.h>
#include "Edit/EditSession.h"

namespace claudetune
{

struct SessionCodec
{
    // Audio + analysis (expensive: cache it by audio revision).
    static juce::MemoryBlock encodeAudio (const edit::SessionState& state);
    // Notes and drawn pitch (cheap).
    static juce::MemoryBlock encodeEdits (const std::string& edits);

    // Reads what encodeAudio + encodeEdits wrote (concatenated). Returns false on damage.
    static bool decode (const void* data, size_t size, edit::SessionState& state);
};

} // namespace claudetune
