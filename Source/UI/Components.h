#pragma once
// ClaudeTune - custom display components.

#include "Theme.h"
#include "../PluginProcessor.h"

namespace claudetune::ui
{

//==============================================================================
// Correction meter: shows the target note and how far (and which way) the
// voice is being moved, from -50 to +50 cents.
class TunerDisplay : public juce::Component
{
public:
    void setState (int newTarget, float newCents, float newSung, bool newFlats)
    {
        if (newTarget == target && std::abs (newCents - cents) < 0.5f
            && std::abs (newSung - sung) < 0.01f && newFlats == flats)
            return;
        target = newTarget;
        cents = newCents;
        sung = newSung;
        flats = newFlats;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        using namespace juce;
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::background);
        g.fillRoundedRectangle (r, 10.0f);
        g.setColour (Colours::outline);
        g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, 1.0f);

        const auto centre = Point<float> (r.getCentreX(), r.getY() + r.getHeight() * 0.47f);
        const float outer = jmin (r.getWidth() * 0.42f, r.getHeight() * 0.36f);
        const float inner = outer * 0.80f;
        const float span = MathConstants<float>::pi * 0.78f;
        constexpr int segments = 25; // 4 cents each

        const float shown = jlimit (-50.0f, 50.0f, cents);
        for (int i = 0; i < segments; ++i)
        {
            const float segCentre = -50.0f + (i + 0.5f) * (100.0f / segments);
            const float a0 = -span + (2.0f * span) * (float) i / segments + 0.012f;
            const float a1 = -span + (2.0f * span) * (float) (i + 1) / segments - 0.012f;

            const bool lit = target >= 0
                             && ((shown > 0 && segCentre > 0 && segCentre - 2.0f < shown)
                                 || (shown < 0 && segCentre < 0 && segCentre + 2.0f > shown));
            const bool zero = std::abs (segCentre) < 2.1f;

            Path p;
            p.addCentredArc (centre.x, centre.y, outer, outer, 0.0f, a0, a1, true);
            p.addCentredArc (centre.x, centre.y, inner, inner, 0.0f, a1, a0, false);
            p.closeSubPath();
            g.setColour (lit ? Colours::accent : (zero ? Colours::outline.brighter (0.3f) : Colours::outline));
            g.fillPath (p);
        }

        g.setColour (Colours::textDim);
        g.setFont (font (11.0f, true));
        const auto lblL = centre.getPointOnCircumference (outer + 12.0f, -span);
        const auto lblR = centre.getPointOnCircumference (outer + 12.0f, span);
        g.drawText ("-50", Rectangle<float> (30, 14).withCentre (lblL), Justification::centred);
        g.drawText ("+50", Rectangle<float> (30, 14).withCentre (lblR), Justification::centred);
        g.drawText ("0", Rectangle<float> (30, 14).withCentre (centre.getPointOnCircumference (outer + 10.0f, 0.0f)),
                    Justification::centred);

        // big note name
        const auto noteArea = Rectangle<float> (inner * 1.6f, inner * 0.9f).withCentre (centre.translated (0, -inner * 0.18f));
        if (target >= 0)
        {
            g.setColour (Colours::text);
            g.setFont (font (noteArea.getHeight() * 0.78f, true));
            g.drawText (noteName (target, flats), noteArea, Justification::centred);

            g.setColour (Colours::textDim);
            g.setFont (font (13.0f, true));
            g.drawText ("octave " + String (target / 12 - 1), noteArea.translated (0, noteArea.getHeight() * 0.55f),
                        Justification::centred);
        }
        else
        {
            g.setColour (Colours::outline.brighter (0.4f));
            g.setFont (font (noteArea.getHeight() * 0.5f, true));
            g.drawText ("--", noteArea, Justification::centred);
        }

        // correction readout
        auto readout = r.removeFromBottom (26.0f).reduced (12.0f, 2.0f);
        g.setFont (font (13.0f, true));
        if (target >= 0)
        {
            const int c = roundToInt (cents);
            g.setColour (Colours::accent);
            const String dir = c > 0 ? "up" : (c < 0 ? "down" : "");
            g.drawText (c == 0 ? String ("in tune") : String (std::abs (c)) + " cents " + dir, readout, Justification::centredRight);
        }
        if (sung >= 0.0f)
        {
            const int nearest = roundToInt (sung);
            const int off = roundToInt ((sung - (float) nearest) * 100.0f);
            g.setColour (Colours::sand);
            g.drawText (String ("sung ") + noteName (nearest, flats) + String (nearest / 12 - 1)
                            + (off >= 0 ? " +" : " ") + String (off) + "c",
                        readout, Justification::centredLeft);
        }
    }

private:
    int target = -1;
    float cents = 0.0f, sung = -1.0f;
    bool flats = false;
};

//==============================================================================
class LevelMeter : public juce::Component
{
public:
    explicit LevelMeter (juce::String labelText) : label (std::move (labelText)) {}

    void setLevel (float linearPeak)
    {
        const float db = juce::Decibels::gainToDecibels (linearPeak, -60.0f);
        const float decayed = displayDb - 1.2f; // ~36 dB/s at 30 fps
        const float next = juce::jmax (db, decayed);
        if (std::abs (next - displayDb) > 0.05f)
        {
            displayDb = next;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        using namespace juce;
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::textDim);
        g.setFont (font (11.0f, true));
        g.drawText (label, r.removeFromTop (16.0f), Justification::centred);

        auto bar = r.withTrimmedBottom (4.0f).withWidth (10.0f).withX (r.getX() + 3.0f);
        g.setColour (Colours::background);
        g.fillRoundedRectangle (bar, 3.0f);

        constexpr float minDb = -28.0f;
        const float norm = jlimit (0.0f, 1.0f, (displayDb - minDb) / -minDb);
        constexpr int segs = 18;
        for (int i = 0; i < segs; ++i)
        {
            const float segNorm = (float) (i + 1) / segs;
            auto seg = bar.withTrimmedTop (bar.getHeight() * (1.0f - segNorm))
                           .withHeight (bar.getHeight() / segs - 2.0f)
                           .reduced (1.5f, 0.0f);
            const bool lit = norm >= segNorm - 0.5f / segs;
            Colour c = i >= segs - 2 ? Colours::danger : (i >= segs - 5 ? Colours::sand : Colours::accent);
            g.setColour (lit ? c : Colours::outline.withAlpha (0.6f));
            g.fillRoundedRectangle (seg, 1.5f);
        }

        g.setColour (Colours::textDim.withAlpha (0.8f));
        g.setFont (font (9.5f));
        for (int db : { 0, -12, -28 })
        {
            const float y = bar.getY() + bar.getHeight() * (1.0f - ((float) db - minDb) / -minDb);
            g.drawText (String (db), Rectangle<float> (bar.getRight() + 3.0f, y - 6.0f, 24.0f, 12.0f), Justification::centredLeft);
        }
    }

private:
    juce::String label;
    float displayDb = -60.0f;
};

//==============================================================================
// Keyboard with the note-state grid above it and draggable range edges.
class NoteKeyboard : public juce::Component
{
public:
    explicit NoteKeyboard (ClaudeTuneProcessor& p) : proc (p)
    {
        lowParam = dynamic_cast<juce::RangedAudioParameter*> (proc.apvts.getParameter (ParamIDs::rangeLow));
        highParam = dynamic_cast<juce::RangedAudioParameter*> (proc.apvts.getParameter (ParamIDs::rangeHigh));
    }

    void setLive (float sungPitch, int targetNote)
    {
        const int sungNote = sungPitch >= 0.0f ? juce::roundToInt (sungPitch) : -1;
        if (sungNote != sung || targetNote != target)
        {
            sung = sungNote;
            target = targetNote;
            repaint();
        }
    }

    void resized() override
    {
        int numWhite = 0;
        for (int n = kKeyboardLow; n <= kKeyboardHigh; ++n)
            if (! isBlack (n))
                ++numWhite;

        const float w = (float) getWidth() / (float) numWhite;
        const float top = (float) cellHeight;
        const float h = (float) getHeight() - top;
        int wi = 0;
        for (int n = kKeyboardLow; n <= kKeyboardHigh; ++n)
        {
            if (! isBlack (n))
            {
                keys[(size_t) n] = { wi * w, top, w, h };
                ++wi;
            }
            else
            {
                const float bw = w * 0.62f;
                keys[(size_t) n] = { wi * w - bw * 0.5f, top, bw, h * 0.6f };
            }
        }
    }

    void paint (juce::Graphics& g) override
    {
        using namespace juce;
        const int low = rangeLow(), high = rangeHigh();

        // cells strip
        g.setColour (Colours::background);
        g.fillRoundedRectangle (Rectangle<float> (0, 0, (float) getWidth(), (float) cellHeight - 3.0f), 5.0f);

        for (int n = kKeyboardLow; n <= kKeyboardHigh; ++n)
            drawCell (g, n, cellRect (n));

        // white keys
        for (int n = kKeyboardLow; n <= kKeyboardHigh; ++n)
        {
            if (isBlack (n)) continue;
            auto k = keys[(size_t) n].reduced (0.5f, 0.0f);
            Colour fill = Colour (0xffe9e6e0);
            if (n == sung)   fill = Colours::sand;
            g.setColour (fill);
            g.fillRoundedRectangle (k.withTrimmedTop (-4.0f), 3.0f);
            if (n == target)
            {
                g.setColour (Colours::accent);
                g.fillRoundedRectangle (k.withTrimmedTop (k.getHeight() - 8.0f).reduced (3.0f, 1.5f), 2.0f);
            }
            if (n % 12 == 0)
            {
                g.setColour (Colour (0xff6b6f78));
                g.setFont (font (10.0f, true));
                g.drawText ("C" + String (n / 12 - 1), k.withTrimmedTop (k.getHeight() - 22.0f).withTrimmedBottom (8.0f),
                            Justification::centred);
            }
        }

        // black keys
        for (int n = kKeyboardLow; n <= kKeyboardHigh; ++n)
        {
            if (! isBlack (n)) continue;
            auto k = keys[(size_t) n];
            g.setColour (n == sung ? Colours::sand.darker (0.3f) : Colour (0xff23252a));
            g.fillRoundedRectangle (k, 2.5f);
            if (n == target)
            {
                g.setColour (Colours::accent);
                g.fillRoundedRectangle (k.withTrimmedTop (k.getHeight() - 7.0f).reduced (2.5f, 1.5f), 2.0f);
            }
        }

        // range shading
        const float x0 = rangeEdgeX (low, true), x1 = rangeEdgeX (high, false);
        g.setColour (Colours::background.withAlpha (0.72f));
        g.fillRect (Rectangle<float> (0, 0, x0, (float) getHeight()));
        g.fillRect (Rectangle<float> (x1, 0, (float) getWidth() - x1, (float) getHeight()));

        for (float x : { x0, x1 })
        {
            g.setColour (Colours::accent);
            g.fillRoundedRectangle (Rectangle<float> (x - 2.0f, 0.0f, 4.0f, (float) getHeight()), 2.0f);
            g.fillRoundedRectangle (Rectangle<float> (x - 5.0f, (float) cellHeight + 10.0f, 10.0f, 24.0f), 3.0f);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (edgeAt (e.position.x) != 0 && e.position.y > (float) cellHeight
                            ? juce::MouseCursor::LeftRightResizeCursor
                            : juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = 0;
        if (e.position.y < (float) cellHeight)
        {
            const int n = cellAt (e.position);
            if (n >= 0)
                proc.cycleNoteState (n, e.mods.isShiftDown() || e.mods.isPopupMenu());
            repaint();
            return;
        }

        dragging = edgeAt (e.position.x);
        if (dragging != 0)
            (dragging < 0 ? lowParam : highParam)->beginChangeGesture();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging == 0) return;
        const int n = noteAtX (e.position.x);
        if (dragging < 0)
            setParamNote (lowParam, juce::jlimit (kKeyboardLow, rangeHigh() - 6, n));
        else
            setParamNote (highParam, juce::jlimit (rangeLow() + 6, kKeyboardHigh, n));
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging != 0)
            (dragging < 0 ? lowParam : highParam)->endChangeGesture();
        dragging = 0;
    }

private:
    static bool isBlack (int n) noexcept
    {
        const int pc = n % 12;
        return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
    }

    int rangeLow() const  { return (int) proc.apvts.getRawParameterValue (ParamIDs::rangeLow)->load(); }
    int rangeHigh() const { return (int) proc.apvts.getRawParameterValue (ParamIDs::rangeHigh)->load(); }

    // left edge of the lowest note / right edge of the highest note
    float rangeEdgeX (int note, bool left) const
    {
        const auto& k = keys[(size_t) note];
        if (! isBlack (note))
            return left ? k.getX() : k.getRight();
        return k.getCentreX();
    }

    int edgeAt (float x) const
    {
        if (std::abs (x - rangeEdgeX (rangeLow(), true)) < 7.0f)   return -1;
        if (std::abs (x - rangeEdgeX (rangeHigh(), false)) < 7.0f) return 1;
        return 0;
    }

    int noteAtX (float x) const
    {
        for (int n = kKeyboardLow; n <= kKeyboardHigh; ++n)
            if (! isBlack (n) && x >= keys[(size_t) n].getX() && x < keys[(size_t) n].getRight())
                return n;
        return x < 0 ? kKeyboardLow : kKeyboardHigh;
    }

    void setParamNote (juce::RangedAudioParameter* p, int note)
    {
        p->setValueNotifyingHost (p->convertTo0to1 ((float) note));
    }

    juce::Rectangle<float> cellRect (int n) const
    {
        const auto& k = keys[(size_t) n];
        const float half = ((float) cellHeight - 4.0f) * 0.5f;
        if (isBlack (n))
            return { k.getCentreX() - half * 0.5f - 1.0f, 1.0f, half + 2.0f, half - 1.0f };
        return juce::Rectangle<float> (k.getX(), half + 1.0f, k.getWidth(), half - 1.0f).reduced (2.0f, 0.5f);
    }

    int cellAt (juce::Point<float> p) const
    {
        for (int n = kKeyboardLow; n <= kKeyboardHigh; ++n)
            if (isBlack (n) && cellRect (n).expanded (1.0f).contains (p))
                return n;
        for (int n = kKeyboardLow; n <= kKeyboardHigh; ++n)
            if (! isBlack (n) && cellRect (n).expanded (1.0f).contains (p))
                return n;
        return -1;
    }

    void drawCell (juce::Graphics& g, int n, juce::Rectangle<float> r) const
    {
        using namespace juce;
        const auto s = proc.getNoteState (n);
        switch (s)
        {
            case NoteState::Legal:
                g.setColour (Colours::accent);
                g.fillRoundedRectangle (r, 2.5f);
                break;
            case NoteState::Bypass:
                g.setColour (Colours::teal);
                g.fillRoundedRectangle (r, 2.5f);
                g.setColour (Colours::background);
                g.setFont (font (9.0f, true));
                g.drawText ("B", r, Justification::centred);
                break;
            case NoteState::Illegal:
            case NoteState::IllegalUp:
            case NoteState::IllegalDown:
            {
                g.setColour (Colours::outline.brighter (0.25f));
                g.drawRoundedRectangle (r.reduced (0.5f), 2.5f, 1.0f);
                if (s != NoteState::Illegal)
                {
                    const auto c = r.getCentre();
                    const float a = jmin (r.getWidth(), r.getHeight()) * 0.28f;
                    Path tri;
                    if (s == NoteState::IllegalUp)
                        tri.addTriangle (c.x - a, c.y + a * 0.7f, c.x + a, c.y + a * 0.7f, c.x, c.y - a * 0.9f);
                    else
                        tri.addTriangle (c.x - a, c.y - a * 0.7f, c.x + a, c.y - a * 0.7f, c.x, c.y + a * 0.9f);
                    g.setColour (Colours::sand);
                    g.fillPath (tri);
                }
                break;
            }
        }
    }

    ClaudeTuneProcessor& proc;
    juce::RangedAudioParameter* lowParam = nullptr;
    juce::RangedAudioParameter* highParam = nullptr;
    std::array<juce::Rectangle<float>, 128> keys {};
    static constexpr int cellHeight = 34;
    int sung = -1, target = -1;
    int dragging = 0;
};

} // namespace claudetune::ui
