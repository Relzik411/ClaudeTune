#include "PluginEditor.h"

using namespace claudetune;
namespace C = claudetune::ui::Colours;

namespace
{
    constexpr int kWidth = 1000;
    constexpr int kHeight = 680;
}

//==============================================================================
ClaudeTuneEditor::ClaudeTuneEditor (ClaudeTuneProcessor& p)
    : AudioProcessorEditor (p), proc (p), keyboard (p), editPage (p)
{
    setLookAndFeel (&lnf);

    for (auto* tab : { &liveTab, &editTab })
    {
        tab->setRadioGroupId (1001);
        tab->setClickingTogglesState (false);
        addAndMakeVisible (tab);
    }
    liveTab.onClick = [this] { proc.setMode (ClaudeTuneProcessor::Mode::Live); updateModeVisibility(); };
    editTab.onClick = [this] { proc.setMode (ClaudeTuneProcessor::Mode::Edit); updateModeVisibility(); };
    liveTab.setTooltip ("Live: real-time correction as the vocal plays");
    editTab.setTooltip ("Edit: capture the vocal and edit its notes");
    addChildComponent (editPage);

    setupKnob (speed, ParamIDs::speed);
    setupKnob (transition, ParamIDs::transition);
    setupKnob (tolCents, ParamIDs::tolCents, true);
    setupKnob (tolTime, ParamIDs::tolTime, true);
    setupKnob (correction, ParamIDs::correction);
    setupKnob (vibratoDepth, ParamIDs::vibratoDepth);
    setupKnob (refToneLevel, ParamIDs::refToneLevel, true);

    // Link: dragging one timing knob moves the other by the same ratio
    speed.onDragStart = [this] { linkedDragStarted (true); };
    transition.onDragStart = [this] { linkedDragStarted (false); };
    speed.onValueChange = [this] { linkedValueChanged (true); };
    transition.onValueChange = [this] { linkedValueChanged (false); };

    refHz.setSliderStyle (juce::Slider::IncDecButtons);
    refHz.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 96, 30);
    refHz.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
    addAndMakeVisible (refHz);
    sliderAttachments.push_back (std::make_unique<SliderAttachment> (proc.apvts, ParamIDs::refHz, refHz));
    refHz.setRange (415.30, 466.16, 0.1); // +/- buttons move in 0.1 Hz steps; typing allows 0.01

    setupToggle (link, ParamIDs::link);
    setupToggle (correctionOn, ParamIDs::correctionOn);
    setupToggle (formant, ParamIDs::formant);
    setupToggle (vibratoOn, ParamIDs::vibratoOn);
    setupToggle (flats, ParamIDs::useFlats);
    setupToggle (groupOctaves, ParamIDs::groupOctaves);
    setupToggle (refTone, ParamIDs::refTone);
    setupToggle (targetPitch, ParamIDs::targetPitch);

    link.setButtonText ("LINK");
    groupOctaves.setButtonText ("Group Octaves");
    refTone.setButtonText ("Reference Tone");
    targetPitch.setButtonText ("Target Pitch");

    resetScale.onClick = [this] { proc.resetScale(); keyboard.repaint(); };
    addAndMakeVisible (resetScale);

    for (int i = 0; i < 12; ++i)
        root.addItem (noteName (i, false), i + 1);
    for (int i = 0; i < kNumScales; ++i)
        scale.addItem (kScales[i].name, i + 1);
    addAndMakeVisible (root);
    addAndMakeVisible (scale);
    rootAttachment = std::make_unique<ComboAttachment> (proc.apvts, ParamIDs::root, root);
    scaleAttachment = std::make_unique<ComboAttachment> (proc.apvts, ParamIDs::scale, scale);

    for (int i = 0; i < kNumRangePresets; ++i)
    {
        auto& b = rangeButtons[(size_t) i];
        b.setButtonText (kRangePresets[i].name);
        b.onClick = [this, i] { applyRangePreset (i); };
        addAndMakeVisible (b);
    }

    addAndMakeVisible (tuner);
    addAndMakeVisible (inMeter);
    addAndMakeVisible (outMeter);
    addAndMakeVisible (keyboard);

    liveComponents = { &speed, &transition, &tolCents, &tolTime, &correction, &vibratoDepth, &refToneLevel, &refHz,
                       &link, &correctionOn, &formant, &vibratoOn, &flats, &groupOctaves, &refTone, &targetPitch,
                       &resetScale, &root, &scale, &tuner, &inMeter, &outMeter, &keyboard };
    for (auto& b : rangeButtons)
        liveComponents.push_back (&b);

    setSize (kWidth, kHeight);
    refreshRootNames();
    updateModeVisibility();
    startTimerHz (30);
}

ClaudeTuneEditor::~ClaudeTuneEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

//==============================================================================
void ClaudeTuneEditor::setupKnob (juce::Slider& s, const char* paramID, bool small)
{
    s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    s.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f, juce::MathConstants<float>::pi * 2.8f, true);
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, small ? 64 : 84, 20);
    s.setMouseDragSensitivity (small ? 160 : 220);
    s.setVelocityBasedMode (false);
    addAndMakeVisible (s);
    sliderAttachments.push_back (std::make_unique<SliderAttachment> (proc.apvts, paramID, s));
}

void ClaudeTuneEditor::setupToggle (juce::TextButton& b, const char* paramID)
{
    b.setClickingTogglesState (true);
    addAndMakeVisible (b);
    buttonAttachments.push_back (std::make_unique<ButtonAttachment> (proc.apvts, paramID, b));
}

void ClaudeTuneEditor::linkedDragStarted (bool)
{
    linkLogOffset = std::log (transition.getValue()) - std::log (speed.getValue());
}

void ClaudeTuneEditor::linkedValueChanged (bool fromSpeed)
{
    if (! link.getToggleState())
        return;

    auto& source = fromSpeed ? speed : transition;
    auto& other = fromSpeed ? transition : speed;
    if (! source.isMouseButtonDown())
        return; // only follow the user's own drags, never automation

    const double v = fromSpeed ? std::exp (std::log (speed.getValue()) + linkLogOffset)
                               : std::exp (std::log (transition.getValue()) - linkLogOffset);
    other.setValue (juce::jlimit (0.1, 800.0, v), juce::sendNotificationSync);
}

void ClaudeTuneEditor::applyRangePreset (int index)
{
    const auto& preset = kRangePresets[index];
    for (auto [id, note] : { std::pair { ParamIDs::rangeLow, preset.low }, std::pair { ParamIDs::rangeHigh, preset.high } })
    {
        if (auto* param = proc.apvts.getParameter (id))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->convertTo0to1 ((float) note));
            param->endChangeGesture();
        }
    }
    keyboard.repaint();
}

void ClaudeTuneEditor::refreshRootNames()
{
    const bool useFlats = flats.getToggleState();
    for (int i = 0; i < 12; ++i)
        root.changeItemText (i + 1, noteName (i, useFlats));
    const int id = root.getSelectedId();
    root.setSelectedId (0, juce::dontSendNotification);
    root.setSelectedId (id, juce::dontSendNotification);
    flats.setButtonText (useFlats ? juce::String::fromUTF8 ("\xe2\x99\xad flats") : juce::String::fromUTF8 ("\xe2\x99\xaf sharps"));
    lastFlats = useFlats;
}

void ClaudeTuneEditor::updateModeVisibility()
{
    showingEdit = proc.getMode() == ClaudeTuneProcessor::Mode::Edit;
    for (auto* comp : liveComponents)
        comp->setVisible (! showingEdit);
    editPage.setVisible (showingEdit);
    liveTab.setToggleState (! showingEdit, juce::dontSendNotification);
    editTab.setToggleState (showingEdit, juce::dontSendNotification);
    repaint();
}

//==============================================================================
void ClaudeTuneEditor::timerCallback()
{
    if ((proc.getMode() == ClaudeTuneProcessor::Mode::Edit) != showingEdit)
        updateModeVisibility(); // e.g. changed by a preset or the host

    if (showingEdit)
        editPage.refresh();

    auto& t = proc.getTelemetry();
    inMeter.setLevel (t.inputPeak.exchange (0.0f));
    outMeter.setLevel (t.outputPeak.exchange (0.0f));

    const int target = t.targetNote.load();
    const float sung = t.detectedPitch.load();
    tuner.setState (target, t.correctionCents.load(), sung, flats.getToggleState());
    keyboard.setLive (sung, target);

    if (proc.getNoteStateVersion() != lastNoteVersion)
    {
        lastNoteVersion = proc.getNoteStateVersion();
        keyboard.repaint();
    }

    if (flats.getToggleState() != lastFlats)
        refreshRootNames();

    correctionOn.setButtonText (correctionOn.getToggleState() ? "ON" : "OFF");
    vibratoOn.setButtonText (vibratoOn.getToggleState() ? "ON" : "OFF");
    formant.setButtonText (formant.getToggleState() ? "Formant: Corrected" : "Formant: Shifted");
    correction.setEnabled (correctionOn.getToggleState());
    vibratoDepth.setEnabled (vibratoOn.getToggleState());

    const int low = (int) proc.apvts.getRawParameterValue (ParamIDs::rangeLow)->load();
    const int high = (int) proc.apvts.getRawParameterValue (ParamIDs::rangeHigh)->load();
    for (int i = 0; i < kNumRangePresets; ++i)
        rangeButtons[(size_t) i].setToggleState (kRangePresets[i].low == low && kRangePresets[i].high == high,
                                                 juce::dontSendNotification);

    const int latency = proc.getReportedLatency();
    const bool edited = proc.isScaleModified();
    if (latency != lastLatency || edited != scaleEdited)
    {
        lastLatency = latency;
        scaleEdited = edited;
        const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
        latencyText = "latency " + juce::String (1000.0 * latency / sr, 1) + " ms";
        repaint();
    }
    if (showingEdit && latencyText != "latency 0.0 ms")
    {
        latencyText = "latency 0.0 ms";
        repaint();
    }
}

//==============================================================================
void ClaudeTuneEditor::paint (juce::Graphics& g)
{
    using namespace juce;
    g.fillAll (C::background);

    // header
    auto header = getLocalBounds().removeFromTop (48).reduced (16, 0);
    {
        // logo: a wobbly wave that settles into a straight line
        Path wave;
        const float x0 = (float) header.getX(), cy = (float) header.getCentreY();
        wave.startNewSubPath (x0, cy);
        for (int i = 1; i <= 40; ++i)
        {
            const float t = (float) i / 40.0f;
            const float amp = 7.0f * (1.0f - t) * (1.0f - t);
            wave.lineTo (x0 + t * 34.0f, cy + amp * std::sin (t * 18.0f));
        }
        g.setColour (C::accent);
        g.strokePath (wave, PathStrokeType (2.4f, PathStrokeType::curved, PathStrokeType::rounded));
        g.fillEllipse (x0 + 34.0f - 3.0f, cy - 3.0f, 6.0f, 6.0f);

        g.setColour (C::text);
        g.setFont (ui::font (22.0f, true));
        g.drawText ("ClaudeTune", header.withTrimmedLeft (46), Justification::centredLeft);
        g.setColour (C::textDim);
        g.setFont (ui::font (12.5f));
        g.drawText (latencyText + "   v2 preview", header, Justification::centredRight);
    }

    if (showingEdit)
        return;

    for (auto& r : panels)
    {
        g.setColour (C::panel);
        g.fillRoundedRectangle (r.toFloat(), 12.0f);
        g.setColour (C::outline);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 12.0f, 1.0f);
    }

    for (auto& c : captions)
    {
        g.setColour (c.title ? C::accent : C::textDim);
        g.setFont (ui::font (c.title ? 11.5f : 12.0f, true));
        g.drawText (c.title ? c.text.toUpperCase() : c.text, c.area, c.title ? Justification::centredLeft : Justification::centred);
    }

    if (scaleEdited)
    {
        g.setColour (C::sand);
        g.setFont (ui::font (12.0f, true));
        g.drawText ("* scale edited", scale.getBounds().translated (0, -22).withHeight (18), Justification::centredRight);
    }
}

void ClaudeTuneEditor::resized()
{
    using R = juce::Rectangle<int>;
    panels.clear();
    captions.clear();
    auto caption = [this] (juce::String t, R area, bool title = false) { captions.push_back ({ std::move (t), area, title }); };

    liveTab.setBounds (196, 11, 74, 28);
    editTab.setBounds (274, 11, 74, 28);
    editPage.setBounds (0, 56, kWidth, kHeight - 56);

    // --- top-left: timing ----------------------------------------------------
    const R timing (16, 56, 330, 296);
    panels.push_back (timing);
    caption ("Correction timing", { timing.getX() + 16, timing.getY() + 10, 200, 18 }, true);

    const int big = 116;
    speed.setBounds (timing.getX() + 22, timing.getY() + 56, big, big + 22);
    transition.setBounds (timing.getRight() - 22 - big, timing.getY() + 56, big, big + 22);
    caption ("Speed", { speed.getX(), timing.getY() + 34, big, 18 });
    caption ("Note Transition", { transition.getX() - 10, timing.getY() + 34, big + 20, 18 });
    link.setBounds (timing.getCentreX() - 30, timing.getY() + 104, 60, 26);

    caption ("Tolerance", { timing.getX() + 16, timing.getY() + 208, 120, 18 }, true);
    const int small = 64;
    tolCents.setBounds (timing.getX() + 92, timing.getY() + 206, small, small + 20);
    tolTime.setBounds (timing.getX() + 206, timing.getY() + 206, small, small + 20);
    caption ("Cents", { tolCents.getX() - 50, tolCents.getY() + 22, 50, 18 });
    caption ("Time", { tolTime.getX() - 44, tolTime.getY() + 22, 44, 18 });

    // --- top-centre: meter -----------------------------------------------------
    const R centre (358, 56, 314, 296);
    panels.push_back (centre);
    inMeter.setBounds (centre.getX() + 8, centre.getY() + 14, 38, centre.getHeight() - 28);
    outMeter.setBounds (centre.getRight() - 46, centre.getY() + 14, 38, centre.getHeight() - 28);
    tuner.setBounds (centre.getX() + 50, centre.getY() + 12, centre.getWidth() - 100, centre.getHeight() - 24);

    // --- top-right: amount, formant, vibrato ------------------------------------
    const R amount (684, 56, 300, 296);
    panels.push_back (amount);
    caption ("Correction", { amount.getX() + 16, amount.getY() + 10, 120, 18 }, true);
    caption ("Vibrato", { amount.getX() + 166, amount.getY() + 10, 120, 18 }, true);
    correctionOn.setBounds (amount.getX() + 16, amount.getY() + 36, 56, 24);
    vibratoOn.setBounds (amount.getX() + 166, amount.getY() + 36, 56, 24);
    correction.setBounds (amount.getX() + 18, amount.getY() + 74, big, big + 22);
    vibratoDepth.setBounds (amount.getX() + 168, amount.getY() + 74, big, big + 22);
    caption ("Amount", { correction.getX(), correction.getBottom() + 0, big, 16 });
    caption ("Depth", { vibratoDepth.getX(), vibratoDepth.getBottom() + 0, big, 16 });
    formant.setBounds (amount.getX() + 16, amount.getBottom() - 46, amount.getWidth() - 32, 30);

    // --- keyboard ----------------------------------------------------------
    const R keys (16, 364, kWidth - 32, 166);
    panels.push_back (keys);
    keyboard.setBounds (keys.reduced (12, 12));

    // --- bottom row --------------------------------------------------------
    const R range (16, 542, 384, 122);
    panels.push_back (range);
    caption ("Vocal range", { range.getX() + 16, range.getY() + 10, 200, 18 }, true);
    {
        const int bw = 82, bh = 30, gap = 8;
        for (int i = 0; i < kNumRangePresets; ++i)
        {
            const int row = i < 4 ? 0 : 1;
            const int col = i < 4 ? i : i - 4;
            const int w = (i == 4) ? bw + 30 : bw;
            int x = range.getX() + 16 + col * (bw + gap);
            if (row == 1 && col > 0) x += 30;
            rangeButtons[(size_t) i].setBounds (x, range.getY() + 38 + row * (bh + gap), w, bh);
        }
    }

    const R scaleArea (412, 542, 336, 122);
    panels.push_back (scaleArea);
    caption ("Key & scale", { scaleArea.getX() + 16, scaleArea.getY() + 10, 200, 18 }, true);
    root.setBounds (scaleArea.getX() + 16, scaleArea.getY() + 38, 64, 30);
    scale.setBounds (scaleArea.getX() + 88, scaleArea.getY() + 38, 136, 30);
    flats.setBounds (scaleArea.getX() + 232, scaleArea.getY() + 38, 88, 30);
    resetScale.setBounds (scaleArea.getX() + 16, scaleArea.getY() + 76, 140, 30);
    groupOctaves.setBounds (scaleArea.getX() + 164, scaleArea.getY() + 76, 156, 30);

    const R midi (760, 542, 224, 122);
    panels.push_back (midi);
    caption ("Tuning & MIDI", { midi.getX() + 16, midi.getY() + 10, 200, 18 }, true);
    refHz.setBounds (midi.getX() + 16, midi.getY() + 34, 150, 28);
    caption ("A4", { midi.getX() + 166, midi.getY() + 38, 40, 20 });
    refTone.setBounds (midi.getX() + 16, midi.getY() + 70, 128, 22);
    targetPitch.setBounds (midi.getX() + 16, midi.getY() + 95, 128, 22);
    refToneLevel.setBounds (midi.getX() + 150, midi.getY() + 62, 64, 58);
    refToneLevel.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    refToneLevel.setTooltip ("Reference tone level");
    caption ("Level", { refToneLevel.getX(), midi.getY() + 10, refToneLevel.getWidth(), 18 });
}
