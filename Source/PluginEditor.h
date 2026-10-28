#pragma once

#include "PluginProcessor.h"
#include "UI/Theme.h"
#include "UI/Components.h"

class ClaudeTuneEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit ClaudeTuneEditor (ClaudeTuneProcessor&);
    ~ClaudeTuneEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void setupKnob (juce::Slider& s, const char* paramID, bool small = false);
    void setupToggle (juce::TextButton& b, const char* paramID);
    void linkedDragStarted (bool fromSpeed);
    void linkedValueChanged (bool fromSpeed);
    void applyRangePreset (int index);
    void refreshRootNames();

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAttachment  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    ClaudeTuneProcessor& proc;
    claudetune::ui::LookAndFeel lnf;

    juce::Slider speed, transition, tolCents, tolTime, correction, vibratoDepth, refToneLevel, refHz;
    juce::TextButton link, correctionOn, formant, vibratoOn, flats, groupOctaves, refTone, targetPitch;
    juce::TextButton resetScale { "Reset Scale" };
    juce::ComboBox root, scale;
    std::array<juce::TextButton, claudetune::kNumRangePresets> rangeButtons;

    claudetune::ui::TunerDisplay tuner;
    claudetune::ui::LevelMeter inMeter { "IN" }, outMeter { "OUT" };
    claudetune::ui::NoteKeyboard keyboard;

    std::vector<std::unique_ptr<SliderAttachment>> sliderAttachments;
    std::vector<std::unique_ptr<ButtonAttachment>> buttonAttachments;
    std::unique_ptr<ComboAttachment> rootAttachment, scaleAttachment;

    struct Caption { juce::String text; juce::Rectangle<int> area; bool title; };
    std::vector<Caption> captions;
    std::vector<juce::Rectangle<int>> panels;

    double linkLogOffset = 0.0;
    bool lastFlats = false;
    int lastLatency = -1;
    juce::String latencyText;
    bool scaleEdited = false;
    int lastNoteVersion = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClaudeTuneEditor)
};
