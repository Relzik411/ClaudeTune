#pragma once
// ClaudeTune - colours and look-and-feel.

#include <juce_gui_basics/juce_gui_basics.h>

namespace claudetune::ui
{

namespace Colours
{
    inline const juce::Colour background   { 0xff111214 };
    inline const juce::Colour panel        { 0xff1a1c20 };
    inline const juce::Colour panelRaised  { 0xff22252b };
    inline const juce::Colour outline      { 0xff2e3138 };
    inline const juce::Colour text         { 0xffece8e1 };
    inline const juce::Colour textDim      { 0xff8b9099 };
    inline const juce::Colour accent       { 0xffd97757 }; // terracotta
    inline const juce::Colour accentSoft   { 0xff8a4a36 };
    inline const juce::Colour sand         { 0xffe8c07d }; // sung pitch
    inline const juce::Colour teal         { 0xff5fb3a9 }; // bypass
    inline const juce::Colour danger       { 0xffe05d5d };
}

inline juce::Font font (float height, bool bold = false)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        using namespace juce;
        setColour (Slider::textBoxTextColourId, Colours::text);
        setColour (Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (Slider::textBoxHighlightColourId, Colours::accentSoft);
        setColour (Label::textColourId, Colours::text);
        setColour (TextButton::buttonColourId, Colours::panelRaised);
        setColour (TextButton::buttonOnColourId, Colours::accent);
        setColour (TextButton::textColourOffId, Colours::textDim);
        setColour (TextButton::textColourOnId, Colours::background);
        setColour (ComboBox::backgroundColourId, Colours::background);
        setColour (ComboBox::outlineColourId, Colours::outline);
        setColour (ComboBox::textColourId, Colours::text);
        setColour (ComboBox::arrowColourId, Colours::accent);
        setColour (PopupMenu::backgroundColourId, Colours::panelRaised);
        setColour (PopupMenu::textColourId, Colours::text);
        setColour (PopupMenu::highlightedBackgroundColourId, Colours::accent);
        setColour (PopupMenu::highlightedTextColourId, Colours::background);
        setColour (TextEditor::backgroundColourId, Colours::background);
        setColour (TextEditor::textColourId, Colours::text);
        setColour (TextEditor::highlightColourId, Colours::accentSoft);
        setColour (CaretComponent::caretColourId, Colours::accent);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
                           float startAngle, float endAngle, juce::Slider& slider) override
    {
        using namespace juce;
        const auto bounds = Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (4.0f);
        const float radius = jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float track = jmax (3.0f, radius * 0.12f);
        const float angle = startAngle + pos * (endAngle - startAngle);
        const bool enabled = slider.isEnabled();

        // track
        Path bg;
        bg.addCentredArc (centre.x, centre.y, radius - track, radius - track, 0.0f, startAngle, endAngle, true);
        g.setColour (Colours::outline);
        g.strokePath (bg, PathStrokeType (track, PathStrokeType::curved, PathStrokeType::rounded));

        // value arc
        Path arc;
        arc.addCentredArc (centre.x, centre.y, radius - track, radius - track, 0.0f, startAngle, angle, true);
        g.setColour (enabled ? Colours::accent : Colours::textDim);
        g.strokePath (arc, PathStrokeType (track, PathStrokeType::curved, PathStrokeType::rounded));

        // knob body
        const float bodyR = radius - track * 2.6f;
        g.setGradientFill (ColourGradient (Colours::panelRaised.brighter (0.15f), centre.x, centre.y - bodyR,
                                           Colours::panel, centre.x, centre.y + bodyR, false));
        g.fillEllipse (Rectangle<float> (bodyR * 2, bodyR * 2).withCentre (centre));
        g.setColour (Colours::outline);
        g.drawEllipse (Rectangle<float> (bodyR * 2, bodyR * 2).withCentre (centre), 1.0f);

        // pointer
        const auto tip = centre.getPointOnCircumference (bodyR * 0.8f, angle);
        const auto base = centre.getPointOnCircumference (bodyR * 0.25f, angle);
        g.setColour (enabled ? Colours::text : Colours::textDim);
        g.drawLine ({ base, tip }, jmax (2.0f, bodyR * 0.09f));
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                               bool highlighted, bool down) override
    {
        using namespace juce;
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        const float corner = jmin (r.getHeight() * 0.5f, 8.0f);
        const bool on = b.getToggleState();

        Colour fill = on ? Colours::accent : Colours::panelRaised;
        if (highlighted) fill = fill.brighter (0.08f);
        if (down)        fill = fill.darker (0.12f);

        g.setColour (fill);
        g.fillRoundedRectangle (r, corner);
        g.setColour (on ? Colours::accent.brighter (0.2f) : Colours::outline);
        g.drawRoundedRectangle (r, corner, 1.0f);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return font (juce::jmin (13.5f, (float) buttonHeight * 0.48f), true);
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override { return font (14.0f, true); }
    juce::Font getPopupMenuFont() override { return font (14.0f); }

    juce::Label* createSliderTextBox (juce::Slider& s) override
    {
        auto* l = LookAndFeel_V4::createSliderTextBox (s);
        l->setFont (font (13.0f, true));
        l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        return l;
    }

    void drawLabel (juce::Graphics& g, juce::Label& label) override
    {
        using namespace juce;
        g.fillAll (label.findColour (Label::backgroundColourId));
        if (! label.isBeingEdited())
        {
            const float alpha = label.isEnabled() ? 1.0f : 0.5f;
            g.setColour (label.findColour (Label::textColourId).withMultipliedAlpha (alpha));
            g.setFont (getLabelFont (label));
            g.drawFittedText (label.getText(), label.getBorderSize().subtractedFrom (label.getLocalBounds()),
                              label.getJustificationType(), 1, label.getMinimumHorizontalScale());
        }
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox&) override
    {
        using namespace juce;
        auto r = Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f);
        g.setColour (Colours::background);
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (Colours::outline);
        g.drawRoundedRectangle (r, 6.0f, 1.0f);

        Path arrow;
        const float ax = (float) width - 16.0f, ay = (float) height * 0.5f;
        arrow.addTriangle (ax - 5, ay - 3, ax + 5, ay - 3, ax, ay + 3);
        g.setColour (Colours::accent);
        g.fillPath (arrow);
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (8, 1, box.getWidth() - 30, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }
};

} // namespace claudetune::ui
