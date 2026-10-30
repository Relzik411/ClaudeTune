#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Engine/TuneEngine.h"
#include "Edit/EditSession.h"

#include <array>
#include <atomic>

namespace ParamIDs
{
    inline constexpr const char* speed          = "speed";
    inline constexpr const char* transition     = "transition";
    inline constexpr const char* link           = "link";
    inline constexpr const char* tolCents       = "tolCents";
    inline constexpr const char* tolTime        = "tolTime";
    inline constexpr const char* vibratoOn      = "vibratoOn";
    inline constexpr const char* vibratoDepth   = "vibratoDepth";
    inline constexpr const char* correctionOn   = "correctionOn";
    inline constexpr const char* correction     = "correction";
    inline constexpr const char* formant        = "formantCorrected";
    inline constexpr const char* refHz          = "refHz";
    inline constexpr const char* root           = "root";
    inline constexpr const char* scale          = "scale";
    inline constexpr const char* useFlats       = "useFlats";
    inline constexpr const char* groupOctaves   = "groupOctaves";
    inline constexpr const char* rangeLow       = "rangeLow";
    inline constexpr const char* rangeHigh      = "rangeHigh";
    inline constexpr const char* refTone        = "refTone";
    inline constexpr const char* targetPitch    = "targetPitch";
    inline constexpr const char* refToneLevel   = "refToneLevel";

    // Edit mode
    inline constexpr const char* mode           = "mode";          // 0 = Live, 1 = Edit
    inline constexpr const char* editCenter     = "editCenter";    // Correct All: centre amount (%)
    inline constexpr const char* editVariation  = "editVariation"; // Correct All: variation kept (%)
    inline constexpr const char* editBypass     = "editBypass";    // hear the original
    inline constexpr const char* editLock       = "editLock";      // stop capturing
}

class ClaudeTuneProcessor : public juce::AudioProcessor,
                            private juce::AudioProcessorValueTreeState::Listener,
                            private juce::AsyncUpdater
{
public:
    enum class Mode { Live = 0, Edit = 1 };

    ClaudeTuneProcessor();
    ~ClaudeTuneProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "ClaudeTune"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    // Note grid (called from the editor on the message thread)
    claudetune::NoteState getNoteState (int note) const noexcept;
    void cycleNoteState (int note, bool backwards);
    void resetScale();
    bool isScaleModified() const noexcept { return scaleModified.load(); }
    int getNoteStateVersion() const noexcept { return noteStateVersion.load(); }

    claudetune::EngineTelemetry& getTelemetry() noexcept { return engine.telemetry; }
    int getReportedLatency() const noexcept { return desiredLatency.load(); }

    //==========================================================================
    // Edit mode
    Mode getMode() const noexcept { return raw.mode->load() > 0.5f ? Mode::Edit : Mode::Live; }
    void setMode (Mode m);
    claudetune::edit::EditSession& getSession() noexcept { return session; }
    // Snaps captured notes (all, or just the given ids) to the Live tab's key/scale,
    // with the Edit tab's Center/Variation.
    void correctAllNotes (const std::vector<uint32_t>& onlyThese = {});
    juce::String describeScale() const;
    std::array<bool, 128> getAllowedNotes() const;   // legal (or bypassed) notes of the Live tab's scale
    float getReferenceOffset() const;                 // semitones from A4 = 440 Hz
    bool usesFlats() const;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void handleAsyncUpdate() override;
    void rebuildScale();
    claudetune::EngineParams readParams() const;
    juce::ValueTree buildParameterState();
    void restoreParameterState (const juce::ValueTree& state);

    claudetune::TuneEngine engine;
    claudetune::edit::EditSession session;
    std::array<std::atomic<uint8_t>, 128> noteStates;
    std::atomic<bool> scaleModified { false };
    std::atomic<int> noteStateVersion { 0 };
    std::atomic<int> desiredLatency { 0 };
    int lastLatency = -1;
    bool wasEditMode = false;

    // the compressed capture is cached between saves
    juce::MemoryBlock cachedAudioBlob;
    uint64_t cachedAudioRevision = 0;

    struct Raw
    {
        std::atomic<float>* speed {};
        std::atomic<float>* transition {};
        std::atomic<float>* tolCents {};
        std::atomic<float>* tolTime {};
        std::atomic<float>* vibratoOn {};
        std::atomic<float>* vibratoDepth {};
        std::atomic<float>* correctionOn {};
        std::atomic<float>* correction {};
        std::atomic<float>* formant {};
        std::atomic<float>* refHz {};
        std::atomic<float>* rangeLow {};
        std::atomic<float>* rangeHigh {};
        std::atomic<float>* refTone {};
        std::atomic<float>* targetPitch {};
        std::atomic<float>* refToneLevel {};
        std::atomic<float>* mode {};
        std::atomic<float>* editBypass {};
        std::atomic<float>* editLock {};
    } raw;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClaudeTuneProcessor)
};
