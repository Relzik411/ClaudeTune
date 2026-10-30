#pragma once
// ClaudeTune - Edit tab transport, shown in the header next to the LIVE / EDIT tabs.
//
// Plays the captured vocal, with your edits, inside ClaudeTune while Logic is stopped,
// starting from the teal play marker in the editor. Logic's own transport can't be
// driven by a plug-in, so this is ClaudeTune's own playback; pressing play in Logic
// always takes over.

#include "Icons.h"
#include "NoteEditor.h"

namespace claudetune::ui
{

class Transport : public juce::Component
{
public:
    Transport (ClaudeTuneProcessor& p, NoteEditor& e) : proc (p), editor (e)
    {
        playButton.setKeycap (Icon::Return);
        playButton.onClick = [this] { editor.togglePreviewFromCursor(); refresh (stalled); };
        playButton.setTooltip ("Play the edited vocal on its own, inside ClaudeTune, from the teal marker (Return). "
                               "Works while Logic is stopped. Space is Logic's, so it plays Logic.");
        addAndMakeVisible (playButton);
    }

    juce::Button& getPlayButton() noexcept { return playButton; }
    const juce::String& getStateText() const noexcept { return stateText; }

    // called by the Edit page ~30 times a second; `previewStalled` = preview asked to play
    // but Logic isn't running the plug-in (the track isn't selected)
    void refresh (bool previewStalled)
    {
        stalled = previewStalled;
        auto& s = proc.getSession();
        const auto snap = s.snapshot();
        const bool hasAudio = snap && ! snap->coverage.empty() && snap->sampleRate > 0.0;
        const bool previewing = s.isPreviewing();
        const bool hostPlaying = s.isPlaying();
        const double sr = hasAudio ? snap->sampleRate : 48000.0;

        playButton.setEnabled (hasAudio && (previewing || ! hostPlaying));
        playButton.setToggleState (true, juce::dontSendNotification); // always drawn filled
        playButton.setOnColour (previewing ? Colours::teal : Colours::accent);
        playButton.setButtonText (previewing ? "Stop" : "Play");
        playButton.setIcon (previewing ? Icon::Stop : Icon::Play);

        double t = editor.getCursorSeconds();
        if (previewing)        t = (double) s.previewPosition() / sr;
        else if (hostPlaying)  t = (double) s.playheadSamples() / sr;

        juce::String pos, state;
        juce::Colour stateColour = Colours::textDim;
        if (hasAudio)
        {
            pos = clock (t);
            const auto tempo = s.tempo();
            if (tempo.isValid())
            {
                const double q = std::max (0.0, tempo.quartersAt (t, sr));
                const double qpb = tempo.quartersPerBar(), beatQ = 4.0 / tempo.denominator;
                const int bar = (int) std::floor (q / qpb + 1e-9) + 1;
                const int beat = (int) std::floor (std::fmod (q, qpb) / beatQ + 1e-9) + 1;
                pos = "Bar " + juce::String (bar) + "." + juce::String (beat) + "   " + pos;
            }
        }

        if (! hasAudio)            state = "Play the vocal in Logic first";
        else if (stalled)        { state = "Select this track in Logic to hear it"; stateColour = Colours::sand; }
        else if (previewing)     { state = editor.isAuditioningNote() ? "Playing the note you're holding" : "Playing in ClaudeTune"; stateColour = Colours::teal; }
        else if (hostPlaying)      state = "Logic is playing";
        else                       state = "Plays this vocal on its own, from the teal marker";

        if (pos != posText || state != stateText || stateColour != stateCol)
        {
            posText = pos;
            stateText = state;
            stateCol = stateColour;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        using namespace juce;
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (Colours::panel);
        g.fillRoundedRectangle (r, 10.0f);
        g.setColour (Colours::outline);
        g.drawRoundedRectangle (r, 10.0f, 1.0f);

        auto text = getLocalBounds().withTrimmedLeft (playButton.getRight() + 12).withTrimmedRight (10).toFloat();
        g.setColour (Colours::text);
        g.setFont (font (13.5f, true));
        g.drawText (posText.isEmpty() ? juce::String ("--:--") : posText, text.withTrimmedBottom (text.getHeight() * 0.48f).translated (0, 2),
                    Justification::bottomLeft, true);
        g.setColour (stateCol);
        g.setFont (font (11.0f));
        g.drawFittedText (stateText, text.withTrimmedTop (text.getHeight() * 0.54f).toNearestInt(), Justification::topLeft, 1, 0.8f);
    }

    void resized() override
    {
        playButton.setBounds (4, 4, 104, getHeight() - 8);
    }

private:
    static juce::String clock (double sec)
    {
        if (sec < 0.0) sec = 0.0;
        const int m = (int) (sec / 60.0);
        const double s = sec - m * 60.0;
        return juce::String (m) + ":" + juce::String (s, 1).paddedLeft ('0', 4);
    }

    ClaudeTuneProcessor& proc;
    NoteEditor& editor;
    IconButton playButton { "Play", Icon::Play };
    juce::String posText, stateText;
    juce::Colour stateCol { Colours::textDim };
    bool stalled = false;
};

} // namespace claudetune::ui
