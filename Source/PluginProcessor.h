#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Engine/TuneEngine.h"

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
}

class ClaudeTuneProcessor : public juce::AudioProcessor,
                            private juce::AudioProcessorValueTreeState::Listener,
                            private juce::AsyncUpdater
{
public:
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
    int getReportedLatency() const noexcept { return engine.telemetry.latency.load(); }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void handleAsyncUpdate() override;
    void rebuildScale();
    claudetune::EngineParams readParams() const;

    claudetune::TuneEngine engine;
    std::array<std::atomic<uint8_t>, 128> noteStates;
    std::atomic<bool> scaleModified { false };
    std::atomic<int> noteStateVersion { 0 };
    int lastLatency = -1;

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
    } raw;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClaudeTuneProcessor)
};
