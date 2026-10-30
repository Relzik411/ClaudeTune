#pragma once
// ClaudeTune - Edit tab: toolbar (tools, snap, view, undo), the piano-roll note
// editor with its hint line, panels for correcting notes, shaping the selected
// notes and the capture, and the transport the main window shows in its header.

#include "Theme.h"
#include "Icons.h"
#include "NoteEditor.h"
#include "Transport.h"
#include "../PluginProcessor.h"

#include <array>

namespace claudetune::ui
{

class EditPage : public juce::Component
{
public:
    enum KnobIndex { CenterKnob = 0, VariationKnob, GlideInKnob, GlideOutKnob, GainKnob, NumKnobs };

    explicit EditPage (ClaudeTuneProcessor& p) : proc (p), editor (p), transport (p, editor)
    {
        using namespace juce;
        addAndMakeVisible (editor);
        editor.onSelectionChanged = [this] { refreshSelectionInfo(); refreshKnobs(); refreshButtons(); };
        editor.onToolChanged = [this] { refreshToolButtons(); };

        for (auto* b : { &undoButton, &redoButton, &fitButton, &followButton, &auditionButton,
                         &correctButton, &resetButton, &restoreTake, &clearCapture, &bypass, &lock, &mergeButton })
            addAndMakeVisible (b);

        // ---- tools ----------------------------------------------------------------
        const char* toolTips[] = { "Select (1): click notes to select them, drag up/down to retune, sideways to move in time, "
                                   "drag an edge to stretch",
                                   "Split (2): click a note where you want to cut it in two",
                                   "Draw (3): drag across a note to draw its pitch by hand; hold Option to erase" };
        for (int i = 0; i < 3; ++i)
        {
            auto& b = toolButtons[(size_t) i];
            b.setTooltip (toolTips[i]);
            b.setRadioGroupId (2001);
            b.setConnectedEdges ((i > 0 ? Button::ConnectedOnLeft : 0) | (i < 2 ? Button::ConnectedOnRight : 0));
            b.onClick = [this, i] { editor.setTool ((NoteEditor::Tool) i); };
            addAndMakeVisible (b);
        }

        // ---- toolbar --------------------------------------------------------------
        snapBox.addItem ("Snap: semitones", 1);
        snapBox.addItem ("Snap: scale", 2);
        snapBox.addItem ("Snap: off", 3);
        snapBox.setSelectedId (1, dontSendNotification);
        snapBox.onChange = [this] { editor.setSnap ((NoteEditor::Snap) (snapBox.getSelectedId() - 1)); };
        snapBox.setTooltip ("What a note snaps to when you drag it up or down. Hold Option while dragging to move freely.");
        addAndMakeVisible (snapBox);

        fitButton.onClick = [this] { editor.zoomToFit(); };
        followButton.setClickingTogglesState (true);
        followButton.setToggleState (true, dontSendNotification);
        followButton.onClick = [this] { editor.setFollowPlayhead (followButton.getToggleState()); };
        auditionButton.setClickingTogglesState (true);
        auditionButton.setToggleState (true, dontSendNotification);
        auditionButton.onClick = [this] { editor.setAuditionOnClick (auditionButton.getToggleState()); };
        undoButton.onClick = [this] { proc.getSession().undo(); };
        redoButton.onClick = [this] { proc.getSession().redo(); };

        fitButton.setTooltip ("Zoom to show every note (F)");
        followButton.setTooltip ("Scroll the view to follow playback");
        auditionButton.setTooltip ("While Logic is stopped, holding the mouse on a note plays it on a loop, "
                                   "so you hear your changes as you drag");
        undoButton.setTooltip ("Undo (Cmd+Z)");
        redoButton.setTooltip ("Redo (Shift+Cmd+Z)");

        // ---- correct --------------------------------------------------------------
        correctButton.setToggleState (true, dontSendNotification); // drawn as the primary action
        correctButton.onClick = [this] { proc.correctAllNotes (editor.getSelection()); };
        correctButton.setTooltip ("Snap the notes to the scale set on the Live tab, using the two knobs");
        resetButton.onClick = [this] {
            const auto ids = editor.getSelection();
            proc.getSession().edit ([&ids] (edit::EditDocument& d) {
                if (ids.empty()) d.resetAll();
                else for (auto id : ids) d.resetNote (id);
            });
        };
        resetButton.setTooltip ("Undo every change to these notes: pitch, shaping and timing (Delete)");
        for (auto* s : { &center, &variation })
            setupKnob (*s, 50);
        centerAttachment = std::make_unique<SliderAttachment> (proc.apvts, ParamIDs::editCenter, center);
        variationAttachment = std::make_unique<SliderAttachment> (proc.apvts, ParamIDs::editVariation, variation);
        center.setTooltip ("Correct: how strongly each note's slow drift is pulled to its centre");
        variation.setTooltip ("Correct: how much of the fast wobble and vibrato is kept (100% = all of it)");

        // ---- note inspector -----------------------------------------------------------
        struct Spec { const char* name; double lo, hi, step, def; const char* suffix; const char* tip; };
        const Spec specs[NumKnobs] = {
            { "Center",    0, 100, 1, 0,   "%",    "Pulls the note's slow drift onto its centre" },
            { "Variation", 0, 200, 1, 100, "%",    "Keeps this much of the note's wobble and vibrato (0% = flat, 200% = exaggerated)" },
            { "Glide in",  0, 500, 1, 30,  " ms",  "How long the pitch takes to slide into this note" },
            { "Glide out", 0, 500, 1, 30,  " ms",  "How long the pitch takes to slide out of this note" },
            { "Gain",    -24,  12, 0.1, 0, " dB",  "Makes the note louder or quieter" },
        };
        for (int i = 0; i < NumKnobs; ++i)
        {
            auto& k = noteKnobs[(size_t) i];
            setupKnob (k, 58);
            k.setRange (specs[i].lo, specs[i].hi, specs[i].step);
            k.setTextValueSuffix (specs[i].suffix);
            k.setNumDecimalPlacesToDisplay (i == GainKnob ? 1 : 0);
            k.setDoubleClickReturnValue (true, specs[i].def);
            k.setValue (specs[i].def, dontSendNotification);
            k.setTooltip (juce::String (specs[i].tip) + ". Double-click to reset.");
            knobNames[(size_t) i] = specs[i].name;
            k.onDragStart = [this] { inGesture = true; gestureFirst = true; };
            k.onDragEnd = [this] { inGesture = false; };
            k.onValueChange = [this, i] { applyKnob (i); };
        }

        mergeButton.onClick = [this] { editor.mergeSelection(); };
        mergeButton.setTooltip ("Join the selected neighbouring notes into one (M)");

        // ---- capture ------------------------------------------------------------------
        restoreTake.onClick = [this] { proc.getSession().restorePreviousCapture(); };
        clearCapture.onClick = [this] {
            if (clearArmedUntil > Time::getMillisecondCounter())
            {
                proc.getSession().clearAll();
                clearArmedUntil = 0;
            }
            else
                clearArmedUntil = Time::getMillisecondCounter() + 3000;
            refreshButtons();
        };
        clearCapture.setOnColour (Colours::danger);
        bypass.setClickingTogglesState (true);
        bypass.setOnColour (Colours::teal);
        lock.setClickingTogglesState (true);
        bypassAttachment = std::make_unique<ButtonAttachment> (proc.apvts, ParamIDs::editBypass, bypass);
        lockAttachment = std::make_unique<ButtonAttachment> (proc.apvts, ParamIDs::editLock, lock);
        bypass.setTooltip ("Hear the original vocal without your edits");
        lock.setTooltip ("Stop capturing: new or changed audio is played but not recorded");
        restoreTake.setTooltip ("Bring back the audio and edits that the last re-take replaced");
        clearCapture.setTooltip ("Forget the captured vocal and all edits (click twice)");

        refreshToolButtons();
        refreshSelectionInfo();
        refreshKnobs();
    }

    NoteEditor& getNoteEditor() noexcept { return editor; }
    Transport& getTransport() noexcept { return transport; }
    juce::Slider& getNoteKnob (int index) noexcept { return noteKnobs[(size_t) index]; }

    // called by the editor's timer
    void refresh()
    {
        auto& s = proc.getSession();
        editor.refresh();

        auto snap = s.snapshot();
        const bool snapChanged = snap != lastSnap;
        lastSnap = snap;

        if (s.consumeOverflow())
            overflowUntil = juce::Time::getMillisecondCounter() + 6000;

        // preview that never starts: Logic isn't processing this track while stopped
        if (s.isPreviewing())
        {
            const auto pos = s.previewPosition();
            if (pos != lastPreviewPos) { lastPreviewPos = pos; previewStalledSince = 0; }
            else if (previewStalledSince == 0) previewStalledSince = juce::Time::getMillisecondCounter();
        }
        else
            previewStalledSince = 0;
        const bool previewStalled = previewStalledSince != 0 && juce::Time::getMillisecondCounter() - previewStalledSince > 400;
        transport.refresh (previewStalled);

        const auto st = s.status();
        juce::String text;
        juce::Colour dot = Colours::textDim;
        const double sr = snap && snap->sampleRate > 0 ? snap->sampleRate : 48000.0;
        int64_t captured = 0;
        if (snap) for (const auto& r : snap->coverage) captured += r.second - r.first;
        const auto dur = [] (double sec) { const int t = (int) std::lround (sec); return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2); };
        const auto dot3 = juce::String::fromUTF8 ("  \xc2\xb7  ");

        switch (st)
        {
            case edit::SessionStatus::Empty:
                text = "Press play in Logic";
                break;
            case edit::SessionStatus::Capturing:
                text = "Capturing" + dot3 + dur ((double) captured / sr);
                dot = Colours::danger;
                break;
            case edit::SessionStatus::Analysing:
                text = "Analysing...";
                dot = Colours::sand;
                break;
            case edit::SessionStatus::Ready:
            {
                int edited = 0;
                if (snap) for (const auto& n : snap->notes) if (n.hasAnyEdit()) ++edited;
                text = juce::String (snap ? (int) snap->notes.size() : 0) + " notes" + dot3 + juce::String (edited) + " edited";
                if (snap && snap->hasBackup) text << dot3 << "re-take";
                dot = Colours::teal;
                break;
            }
            case edit::SessionStatus::SampleRateMismatch:
                text = "Captured at " + juce::String (s.sessionSampleRate() / 1000.0, 1) + " kHz";
                dot = Colours::danger;
                break;
        }
        if (overflowUntil > juce::Time::getMillisecondCounter())
        {
            text = "Audio dropped: play it again";
            dot = Colours::danger;
        }
        if (bypass.getToggleState())
            text = "Comparing" + dot3 + text;

        if (text != statusText || dot != statusDot)
        {
            statusText = text;
            statusDot = dot;
            repaint (statusArea);
        }

        const auto scaleText = proc.describeScale();
        if (scaleText != scaleName)
        {
            scaleName = scaleText;
            repaint (scaleTextArea);
        }

        if (snapChanged)
        {
            refreshSelectionInfo();
            refreshKnobs();
        }
        refreshButtons();
    }

    void paint (juce::Graphics& g) override
    {
        using namespace juce;
        for (auto& r : panels)
        {
            g.setColour (Colours::panel);
            g.fillRoundedRectangle (r.toFloat(), 10.0f);
            g.setColour (Colours::outline);
            g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 10.0f, 1.0f);
        }

        g.setColour (statusDot);
        g.fillEllipse (statusArea.getX() + 2.0f, statusArea.getCentreY() - 5.0f, 10.0f, 10.0f);
        g.setColour (Colours::text);
        g.setFont (font (13.0f, true));
        g.drawFittedText (statusText, statusArea.withTrimmedLeft (20), Justification::centredLeft, 1, 0.75f);

        // magnet next to the snap menu
        drawIcon (g, Icon::Magnet, snapIconArea.toFloat(), Colours::textDim);

        g.setColour (Colours::accent);
        g.setFont (font (11.5f, true));
        g.drawText (correctTitle.toUpperCase(), correctTitleArea, Justification::centredLeft);
        g.drawText (noteTitle.toUpperCase(), noteTitleArea, Justification::centredLeft);
        g.drawText ("CAPTURE", captureTitleArea, Justification::centredLeft);

        g.setColour (Colours::textDim);
        g.setFont (font (11.5f));
        g.drawFittedText ("Snaps to " + scaleName + " (Live tab)", scaleTextArea, Justification::topLeft, 2);

        // knob captions
        g.setFont (font (11.5f, true));
        auto caption = [&] (const juce::Slider& s, const juce::String& t, bool enabled) {
            g.setColour (enabled ? Colours::textDim : Colours::textDim.withAlpha (0.4f));
            g.drawText (t, s.getBounds().withY (s.getBottom() - 1).withHeight (14).expanded (8, 0), Justification::centred);
        };
        caption (center, "Center", true);
        caption (variation, "Variation", true);
        const bool hasSel = ! editor.getSelection().empty();
        for (int i = 0; i < NumKnobs; ++i)
            caption (noteKnobs[(size_t) i], knobNames[(size_t) i], hasSel);

        // what's selected
        g.setColour (hasSel ? Colours::text : Colours::textDim);
        g.setFont (font (12.5f, true));
        g.drawFittedText (noteHeadline, noteHeadlineArea, Justification::centredLeft, 1, 0.8f);
        if (! hasSel)
        {
            g.setColour (Colours::background.withAlpha (0.55f));
            g.fillRoundedRectangle (knobArea.toFloat().expanded (4.0f, 2.0f), 8.0f);
            g.setColour (Colours::textDim);
            g.setFont (font (12.0f));
            g.drawFittedText ("Click a note (or drag across several) to shape it here.\nCmd+A selects every note.",
                              knobArea, Justification::centred, 2);
        }
    }

    void resized() override
    {
        using R = juce::Rectangle<int>;
        panels.clear();
        const int w = getWidth(), h = getHeight();

        // ---- toolbar ----------------------------------------------------------------
        const R bar (16, 0, w - 32, 42);
        panels.push_back (bar);
        int x = bar.getRight() - 8;
        auto place = [&] (juce::Component& c, int width, int gap = 6) { x -= width; c.setBounds (x, bar.getY() + 7, width, 28); x -= gap; };
        place (redoButton, 34, 4);
        place (undoButton, 34, 14);
        place (auditionButton, auditionButton.getIdealWidth (28));
        place (followButton, followButton.getIdealWidth (28));
        place (fitButton, fitButton.getIdealWidth (28), 14);
        place (snapBox, 140, 4);
        snapIconArea = R (x - 18, bar.getY() + 12, 18, 18);
        x -= 18 + 14;
        for (int i = 2; i >= 0; --i)
            place (toolButtons[(size_t) i], toolButtons[(size_t) i].getIdealWidth (28), 0);
        statusArea = R (bar.getX() + 12, bar.getY(), x - bar.getX() - 20, bar.getHeight());

        // ---- editor -------------------------------------------------------------------
        const int bottomH = 132;
        editor.setBounds (16, bar.getBottom() + 8, w - 32, h - bar.getBottom() - 8 - bottomH - 8);

        // ---- bottom panels ---------------------------------------------------------------
        const int y = h - bottomH;
        const R correctPanel (16, y, 316, bottomH - 2);
        const R notePanel (340, y, 404, bottomH - 2);
        const R controlPanel (752, y, w - 16 - 752, bottomH - 2);
        for (auto r : { correctPanel, notePanel, controlPanel }) panels.push_back (r);

        correctTitleArea = R (correctPanel.getX() + 12, y + 8, 180, 16);
        scaleTextArea = R (correctPanel.getX() + 12, y + 28, 180, 32);
        correctButton.setBounds (correctPanel.getX() + 12, correctPanel.getBottom() - 12 - 28, 92, 28);
        resetButton.setBounds (correctButton.getRight() + 6, correctButton.getY(), 92, 28);
        center.setBounds (correctPanel.getX() + 204, y + 10, 52, 92);
        variation.setBounds (correctPanel.getX() + 258, y + 10, 52, 92);

        noteTitleArea = R (notePanel.getX() + 12, y + 8, 120, 16);
        mergeButton.setBounds (notePanel.getRight() - 12 - 80, y + 6, 80, 22);
        noteHeadlineArea = R (notePanel.getX() + 12, y + 26, notePanel.getWidth() - 24, 18);
        knobArea = R (notePanel.getX() + 12, y + 46, notePanel.getWidth() - 24, 70);
        const int kw = knobArea.getWidth() / NumKnobs;
        for (int i = 0; i < NumKnobs; ++i)
            noteKnobs[(size_t) i].setBounds (knobArea.getX() + i * kw + (kw - 64) / 2, knobArea.getY() - 2, 64, 70);

        captureTitleArea = R (controlPanel.getX() + 12, y + 8, 120, 16);
        const int bw = (controlPanel.getWidth() - 12 * 2 - 8) / 2;
        bypass.setBounds (controlPanel.getX() + 12, y + 32, bw, 28);
        lock.setBounds (bypass.getRight() + 8, y + 32, bw, 28);
        restoreTake.setBounds (controlPanel.getX() + 12, y + 68, bw, 28);
        clearCapture.setBounds (restoreTake.getRight() + 8, y + 68, bw, 28);
    }

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    void setupKnob (juce::Slider& s, int textWidth)
    {
        using namespace juce;
        s.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
        s.setRotaryParameters (MathConstants<float>::pi * 1.2f, MathConstants<float>::pi * 2.8f, true);
        s.setTextBoxStyle (Slider::TextBoxBelow, false, textWidth, 16);
        s.setMouseDragSensitivity (180);
        addAndMakeVisible (s);
    }

    // apply one inspector knob to every selected note (one undo step per drag)
    void applyKnob (int index)
    {
        if (updatingKnobs) return;
        const auto ids = editor.getSelection();
        if (ids.empty()) return;
        const double v = noteKnobs[(size_t) index].getValue();
        const bool undoable = inGesture ? gestureFirst : true;
        gestureFirst = false;
        proc.getSession().edit ([&ids, index, v] (edit::EditDocument& d) {
            for (const auto id : ids)
            {
                const auto* n = d.findNote (id);
                if (n == nullptr) continue;
                switch (index)
                {
                    case CenterKnob:    d.setNoteShape (id, (float) v / 100.0f, n->variation); break;
                    case VariationKnob: d.setNoteShape (id, n->center, (float) v / 100.0f); break;
                    case GlideInKnob:   d.setTransitions (id, (float) v, n->transitionOutMs); break;
                    case GlideOutKnob:  d.setTransitions (id, n->transitionInMs, (float) v); break;
                    case GainKnob:      d.setGain (id, (float) v); break;
                    default: break;
                }
            }
        }, undoable);
    }

    void refreshKnobs()
    {
        if (inGesture) return;
        const auto& sel = editor.getSelection();
        const auto snap = proc.getSession().snapshot();
        const edit::Note* n = nullptr;
        if (snap && ! sel.empty())
            for (const auto& note : snap->notes) if (note.id == sel.front()) n = &note;

        updatingKnobs = true;
        for (auto& k : noteKnobs) k.setEnabled (n != nullptr);
        if (n != nullptr)
        {
            noteKnobs[CenterKnob].setValue (n->center * 100.0, juce::dontSendNotification);
            noteKnobs[VariationKnob].setValue (n->variation * 100.0, juce::dontSendNotification);
            noteKnobs[GlideInKnob].setValue (n->transitionInMs, juce::dontSendNotification);
            noteKnobs[GlideOutKnob].setValue (n->transitionOutMs, juce::dontSendNotification);
            noteKnobs[GainKnob].setValue (n->gainDb, juce::dontSendNotification);
        }
        updatingKnobs = false;
    }

    void refreshToolButtons()
    {
        for (int i = 0; i < 3; ++i)
            toolButtons[(size_t) i].setToggleState ((int) editor.getTool() == i, juce::dontSendNotification);
    }

    void refreshSelectionInfo()
    {
        const auto& sel = editor.getSelection();
        const auto snap = proc.getSession().snapshot();
        const float ref = proc.getReferenceOffset();
        const bool flats = proc.usesFlats();
        auto name = [&] (float m) {
            const int n = (int) std::lround (m - ref);
            return juce::String (noteName (n, flats)) + juce::String (n / 12 - 1);
        };
        auto cents = [&] (float m) {
            const int c = (int) std::lround ((m - ref - std::round (m - ref)) * 100.0f);
            return (c > 0 ? "+" : "") + juce::String (c) + "c";
        };

        correctTitle = sel.empty() ? "Correct all notes" : "Correct " + juce::String ((int) sel.size()) + (sel.size() == 1 ? " note" : " notes");
        resetButton.setButtonText (sel.empty() ? "Reset all" : "Reset");

        if (sel.empty() || snap == nullptr)
        {
            noteTitle = "Note";
            noteHeadline = "Nothing selected";
        }
        else if (sel.size() == 1)
        {
            noteTitle = "Note";
            for (const auto& n : snap->notes)
                if (n.id == sel[0])
                    noteHeadline = "Sung " + name (n.detectedPitch) + " " + cents (n.detectedPitch)
                                   + juce::String::fromUTF8 ("   \xe2\x86\x92   ") + name (n.targetPitch) + " " + cents (n.targetPitch)
                                   + juce::String::fromUTF8 ("   \xc2\xb7   ") + juce::String (n.outEnd - n.outStart, 2) + " s";
        }
        else
        {
            noteTitle = juce::String ((int) sel.size()) + " notes";
            noteHeadline = "Knobs change all " + juce::String ((int) sel.size()) + " (showing the first)";
        }
        repaint();
    }

    void refreshButtons()
    {
        const auto snap = lastSnap;
        const bool hasNotes = snap && ! snap->notes.empty();
        undoButton.setEnabled (snap && snap->canUndo);
        redoButton.setEnabled (snap && snap->canRedo);
        correctButton.setEnabled (hasNotes);
        resetButton.setEnabled (hasNotes);
        fitButton.setEnabled (hasNotes);
        mergeButton.setEnabled (editor.getSelection().size() >= 2);
        restoreTake.setEnabled (snap && snap->hasBackup);
        clearCapture.setEnabled (snap && ! snap->coverage.empty());
        const bool armed = clearArmedUntil > juce::Time::getMillisecondCounter();
        clearCapture.setButtonText (armed ? "Confirm" : "Clear");
        clearCapture.setToggleState (armed, juce::dontSendNotification);
        bypass.setButtonText (bypass.getToggleState() ? "Comparing" : "Compare");
        lock.setButtonText (lock.getToggleState() ? "Locked" : "Lock");
    }

    ClaudeTuneProcessor& proc;
    NoteEditor editor;
    Transport transport;
    std::array<IconButton, 3> toolButtons { IconButton { "Select", Icon::Select },
                                            IconButton { "Split", Icon::Split },
                                            IconButton { "Draw", Icon::Draw } };
    juce::ComboBox snapBox;
    IconButton fitButton { "Fit", Icon::Fit }, followButton { "Follow", Icon::Follow }, auditionButton { "Audition", Icon::Audition };
    IconButton undoButton { {}, Icon::Undo }, redoButton { {}, Icon::Redo };
    IconButton correctButton { "Correct", Icon::Correct }, resetButton { "Reset all", Icon::Reset };
    IconButton restoreTake { "Restore take", Icon::Restore }, clearCapture { "Clear", Icon::Clear };
    IconButton bypass { "Compare", Icon::Compare }, lock { "Lock", Icon::Unlock, Icon::Lock };
    IconButton mergeButton { "Merge", Icon::Merge };
    juce::Slider center, variation;
    std::array<juce::Slider, NumKnobs> noteKnobs;
    std::array<juce::String, NumKnobs> knobNames;
    std::unique_ptr<SliderAttachment> centerAttachment, variationAttachment;
    std::unique_ptr<ButtonAttachment> bypassAttachment, lockAttachment;

    bool inGesture = false, gestureFirst = false, updatingKnobs = false;

    std::shared_ptr<const edit::SessionSnapshot> lastSnap;
    std::vector<juce::Rectangle<int>> panels;
    juce::Rectangle<int> statusArea, snapIconArea, correctTitleArea, scaleTextArea, noteTitleArea, noteHeadlineArea, knobArea, captureTitleArea;
    juce::String statusText, scaleName, correctTitle, noteTitle, noteHeadline;
    juce::Colour statusDot { Colours::textDim };
    juce::uint32 clearArmedUntil = 0, overflowUntil = 0, previewStalledSince = 0;
    int64_t lastPreviewPos = -1;
};

} // namespace claudetune::ui
