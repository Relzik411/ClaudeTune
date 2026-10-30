#pragma once
// ClaudeTune - small vector icons and a button that shows an icon next to its label.
//
// Icons are drawn on a 24 x 24 grid and scaled to fit, so they stay sharp at any
// size and on Retina screens.

#include "Theme.h"

namespace claudetune::ui
{

enum class Icon
{
    None, Select, Split, Draw, Magnet, Fit, Follow, Audition, Undo, Redo, Play, Stop,
    Compare, Lock, Unlock, Restore, Clear, Correct, Reset, Merge, Return
};

namespace detail
{
    inline juce::Path arcArrow (bool withHands)
    {
        // a circle open on the left, with an arrow head at its upper end (counter-clockwise)
        juce::Path p;
        p.addCentredArc (12.0f, 12.0f, 8.0f, 8.0f, 0.0f, -1.15f, 3.75f, true);
        p.startNewSubPath (1.6f, 8.0f);
        p.lineTo (4.6f, 9.3f);
        p.lineTo (6.8f, 6.0f);
        if (withHands)
        {
            p.startNewSubPath (12.0f, 7.5f);
            p.lineTo (12.0f, 12.0f);
            p.lineTo (15.0f, 14.0f);
        }
        return p;
    }
}

// Draws an icon centred in `area` (which should be roughly square).
inline void drawIcon (juce::Graphics& g, Icon icon, juce::Rectangle<float> area, juce::Colour colour)
{
    using namespace juce;
    if (icon == Icon::None) return;

    const float size = jmin (area.getWidth(), area.getHeight());
    const float k = size / 24.0f;
    const auto t = AffineTransform::scale (k).translated (area.getCentreX() - 12.0f * k, area.getCentreY() - 12.0f * k);
    const PathStrokeType stroke (jmax (1.2f, 2.0f * k), PathStrokeType::curved, PathStrokeType::rounded);

    Path s, f; // stroked, filled
    switch (icon)
    {
        case Icon::Select:
            f.startNewSubPath (6, 3); f.lineTo (6, 19); f.lineTo (10, 15.2f); f.lineTo (12.8f, 21);
            f.lineTo (15.4f, 19.8f); f.lineTo (12.6f, 14.2f); f.lineTo (18, 14); f.closeSubPath();
            break;
        case Icon::Split:
            s.addEllipse (3.0f, 15.0f, 6.0f, 6.0f);
            s.addEllipse (15.0f, 15.0f, 6.0f, 6.0f);
            s.startNewSubPath (7.8f, 15.8f); s.lineTo (18, 3);
            s.startNewSubPath (16.2f, 15.8f); s.lineTo (6, 3);
            break;
        case Icon::Draw:
            s.startNewSubPath (4, 20); s.lineTo (5, 15); s.lineTo (16, 4); s.lineTo (20, 8); s.lineTo (9, 19); s.closeSubPath();
            s.startNewSubPath (13.5f, 6.5f); s.lineTo (17.5f, 10.5f);
            break;
        case Icon::Magnet:
            s.startNewSubPath (6, 4); s.lineTo (6, 12);
            s.addCentredArc (12, 12, 6, 6, 0.0f, MathConstants<float>::halfPi * 3.0f, MathConstants<float>::halfPi, false);
            s.lineTo (18, 4);
            f.addRectangle (3.5f, 3.0f, 5.0f, 3.5f);
            f.addRectangle (15.5f, 3.0f, 5.0f, 3.5f);
            break;
        case Icon::Fit:
            s.startNewSubPath (4, 9); s.lineTo (4, 4); s.lineTo (9, 4);
            s.startNewSubPath (15, 4); s.lineTo (20, 4); s.lineTo (20, 9);
            s.startNewSubPath (20, 15); s.lineTo (20, 20); s.lineTo (15, 20);
            s.startNewSubPath (9, 20); s.lineTo (4, 20); s.lineTo (4, 15);
            break;
        case Icon::Follow:
            s.startNewSubPath (7, 3); s.lineTo (7, 21);
            s.startNewSubPath (11, 12); s.lineTo (20, 12);
            s.startNewSubPath (16.5f, 8.5f); s.lineTo (20, 12); s.lineTo (16.5f, 15.5f);
            break;
        case Icon::Audition:
            f.startNewSubPath (3, 9); f.lineTo (7, 9); f.lineTo (12, 4.5f); f.lineTo (12, 19.5f); f.lineTo (7, 15); f.lineTo (3, 15); f.closeSubPath();
            s.addCentredArc (12, 12, 4.5f, 4.5f, 0.0f, 0.75f, 2.4f, true);
            s.addCentredArc (12, 12, 8.5f, 8.5f, 0.0f, 0.75f, 2.4f, true);
            break;
        case Icon::Undo:
        case Icon::Redo:
            s.startNewSubPath (8.5f, 4.5f); s.lineTo (4, 9); s.lineTo (8.5f, 13.5f);
            s.startNewSubPath (4, 9); s.lineTo (14, 9);
            s.quadraticTo (20, 9, 20, 14.5f); s.quadraticTo (20, 20, 14, 20); s.lineTo (10, 20);
            if (icon == Icon::Redo)
                s.applyTransform (AffineTransform::scale (-1.0f, 1.0f).translated (24.0f, 0.0f));
            break;
        case Icon::Play:
            f.startNewSubPath (7, 4); f.lineTo (20, 12); f.lineTo (7, 20); f.closeSubPath();
            break;
        case Icon::Stop:
            f.addRoundedRectangle (6.0f, 6.0f, 12.0f, 12.0f, 1.5f);
            break;
        case Icon::Compare:
            s.addEllipse (4.0f, 4.0f, 16.0f, 16.0f);
            f.addPieSegment (4.0f, 4.0f, 16.0f, 16.0f, MathConstants<float>::pi, MathConstants<float>::twoPi, 0.0f);
            break;
        case Icon::Lock:
        case Icon::Unlock:
            f.addRoundedRectangle (5.0f, 11.0f, 14.0f, 10.0f, 2.0f);
            if (icon == Icon::Lock)
            {
                s.startNewSubPath (8, 11); s.lineTo (8, 8);
                s.addCentredArc (12, 8, 4, 4, 0.0f, -MathConstants<float>::halfPi, MathConstants<float>::halfPi, false);
                s.lineTo (16, 11);
            }
            else
            {
                s.startNewSubPath (8, 11); s.lineTo (8, 6);
                s.addCentredArc (12, 6, 4, 4, 0.0f, -MathConstants<float>::halfPi, MathConstants<float>::halfPi, false);
                s.lineTo (16, 7.5f);
            }
            break;
        case Icon::Restore: s = detail::arcArrow (true); break;
        case Icon::Reset:   s = detail::arcArrow (false); break;
        case Icon::Clear:
            s.startNewSubPath (4, 6.5f); s.lineTo (20, 6.5f);
            s.startNewSubPath (9, 6.5f); s.lineTo (9, 3.5f); s.lineTo (15, 3.5f); s.lineTo (15, 6.5f);
            s.startNewSubPath (6, 6.5f); s.lineTo (7.2f, 20.5f); s.lineTo (16.8f, 20.5f); s.lineTo (18, 6.5f);
            s.startNewSubPath (10, 10.5f); s.lineTo (10, 16.5f);
            s.startNewSubPath (14, 10.5f); s.lineTo (14, 16.5f);
            break;
        case Icon::Correct:
        {
            // a wobble that settles onto a straight line
            s.startNewSubPath (2.5f, 12.0f);
            for (int i = 1; i <= 24; ++i)
            {
                const float u = (float) i / 24.0f;
                s.lineTo (2.5f + u * 12.0f, 12.0f + 6.0f * (1.0f - u) * std::sin (u * 15.0f));
            }
            s.lineTo (21.5f, 12.0f);
            break;
        }
        case Icon::Merge:
            s.startNewSubPath (2.5f, 12); s.lineTo (9.5f, 12);
            s.startNewSubPath (6.5f, 8.5f); s.lineTo (10, 12); s.lineTo (6.5f, 15.5f);
            s.startNewSubPath (21.5f, 12); s.lineTo (14.5f, 12);
            s.startNewSubPath (17.5f, 8.5f); s.lineTo (14, 12); s.lineTo (17.5f, 15.5f);
            s.startNewSubPath (12, 4); s.lineTo (12, 20);
            break;
        case Icon::Return:
            s.startNewSubPath (19, 5); s.lineTo (19, 14); s.lineTo (6, 14);
            s.startNewSubPath (10, 10); s.lineTo (6, 14); s.lineTo (10, 18);
            break;
        case Icon::None:
            break;
    }

    g.setColour (colour);
    if (! s.isEmpty()) g.strokePath (s, stroke, t);
    if (! f.isEmpty()) g.fillPath (f, t);
}

// A TextButton that draws an icon to the left of its label, optionally with a small
// keyboard hint ("keycap") on the right. Leave the text empty for an icon-only button.
class IconButton : public juce::TextButton
{
public:
    IconButton (const juce::String& label, Icon i, Icon whenOn = Icon::None)
        : juce::TextButton (label), icon (i), onIcon (whenOn) {}

    void setIcon (Icon i, Icon whenOn = Icon::None) { icon = i; onIcon = whenOn; repaint(); }
    void setKeycap (Icon k) { keycap = k; repaint(); }
    void setOnColour (juce::Colour c) { onColour = c; repaint(); }

    // the width that shows the icon and label at full size
    int getIdealWidth (int height) const
    {
        const auto f = labelFont (height);
        const float iconW = iconSize (height);
        const float textW = getButtonText().isEmpty() ? 0.0f : juce::GlyphArrangement::getStringWidth (f, getButtonText()) + 6.0f;
        const float capW = keycap != Icon::None ? (float) height * 0.62f + 6.0f : 0.0f;
        return (int) std::ceil (iconW + textW + capW + (getButtonText().isEmpty() ? 12.0f : 22.0f));
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        using namespace juce;
        const bool on = getToggleState();
        const bool enabled = isEnabled();
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        const float corner = jmin (r.getHeight() * 0.5f, 8.0f);

        // background (same style as the look-and-feel, with a per-button "on" colour)
        Colour fill = on ? onColour : Colours::panelRaised;
        if (highlighted && enabled) fill = fill.brighter (0.08f);
        if (down)                   fill = fill.darker (0.12f);
        if (! enabled)              fill = fill.withMultipliedAlpha (0.55f);
        g.setColour (fill);
        g.fillRoundedRectangle (r, corner);
        g.setColour (on ? onColour.brighter (0.2f).withMultipliedAlpha (enabled ? 1.0f : 0.5f) : Colours::outline);
        g.drawRoundedRectangle (r, corner, 1.0f);

        Colour ink = on ? Colours::background : (highlighted ? Colours::text : Colours::text.interpolatedWith (Colours::textDim, 0.45f));
        if (! enabled) ink = Colours::textDim.withAlpha (0.45f);

        // lay out: [icon] [label] [keycap], centred, shrinking the label if needed
        const int h = getHeight();
        const float iw = iconSize (h);
        const float capW = keycap != Icon::None ? (float) h * 0.62f : 0.0f;
        auto f = labelFont (h);
        const String label = getButtonText();
        float tw = label.isEmpty() ? 0.0f : GlyphArrangement::getStringWidth (f, label);
        const float pad = label.isEmpty() ? 6.0f : 10.0f;
        const float room = r.getWidth() - 2.0f * pad - iw - (label.isEmpty() ? 0.0f : 6.0f) - (capW > 0.0f ? capW + 6.0f : 0.0f);
        if (tw > room && tw > 0.0f)
        {
            f = f.withHeight (jmax (10.0f, f.getHeight() * room / tw));
            tw = jmin (room, GlyphArrangement::getStringWidth (f, label));
        }
        const float total = iw + (label.isEmpty() ? 0.0f : 6.0f + tw) + (capW > 0.0f ? capW + 6.0f : 0.0f);
        float x = r.getCentreX() - total * 0.5f;
        const float cy = r.getCentreY();

        const Icon shown = (on && onIcon != Icon::None) ? onIcon : icon;
        drawIcon (g, shown, { x, cy - iw * 0.5f, iw, iw }, ink);
        x += iw;
        if (! label.isEmpty())
        {
            x += 6.0f;
            g.setColour (ink);
            g.setFont (f);
            g.drawText (label, Rectangle<float> (x, r.getY(), tw + 2.0f, r.getHeight()), Justification::centredLeft, false);
            x += tw;
        }
        if (capW > 0.0f)
        {
            x += 6.0f;
            const auto cap = Rectangle<float> (x, cy - capW * 0.5f, capW, capW);
            g.setColour (ink.withMultipliedAlpha (0.18f));
            g.fillRoundedRectangle (cap, 3.0f);
            drawIcon (g, keycap, cap.reduced (capW * 0.18f), ink.withMultipliedAlpha (0.85f));
        }
    }

private:
    static float iconSize (int h) noexcept { return juce::jlimit (12.0f, 18.0f, (float) h * 0.54f); }
    static juce::Font labelFont (int h) { return font (juce::jmin (13.5f, (float) h * 0.48f), true); }

    Icon icon, onIcon;
    Icon keycap = Icon::None;
    juce::Colour onColour { Colours::accent };
};

} // namespace claudetune::ui
