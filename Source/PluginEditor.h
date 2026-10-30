#pragma once

#include "PluginProcessor.h"
#include "UI/Theme.h"
#include "UI/Components.h"
#include "UI/EditPage.h"

class ClaudeTuneEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit ClaudeTuneEditor (ClaudeTuneProcessor&);
    ~ClaudeTuneEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    claudetune::ui::EditPage& getEditPage() noexcept { return editPage; }

private:
    void timerCallback() override;
    void setupKnob (juce::Slider& s, const char* paramID, bool small = false);
    void setupToggle (juce::TextButton& b, const char* paramID);
    void linkedDragStarted (bool fromSpeed);
    void linkedValueChanged (bool fromSpeed);
    void applyRangePreset (int index);
    void refreshRootNames();
    void updateModeVisibility();

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAttachment  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    ClaudeTuneProcessor& proc;
    claudetune::ui::LookAndFeel lnf;

    // header tabs
    juce::TextButton liveTab { "LIVE" }, editTab { "EDIT" };
    bool showingEdit = false;

    // Live tab
    juce::Slider speed, transition, tolCents, tolTime, correction, vibratoDepth, refToneLevel, refHz;
    juce::TextButton link, correctionOn, formant, vibratoOn, flats, groupOctaves, refTone, targetPitch;
    juce::TextButton resetScale { "Reset Scale" };
    juce::ComboBox root, scale;
    std::array<juce::TextButton, claudetune::kNumRangePresets> rangeButtons;

    claudetune::ui::TunerDisplay tuner;
    claudetune::ui::LevelMeter inMeter { "IN" }, outMeter { "OUT" };
    claudetune::ui::NoteKeyboard keyboard;
    std::vector<juce::Component*> liveComponents;

    // Edit tab
    claudetune::ui::EditPage editPage;

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
    juce::TooltipWindow tooltips { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClaudeTuneEditor)
};
