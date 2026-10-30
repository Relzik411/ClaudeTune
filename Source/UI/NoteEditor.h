#pragma once
// ClaudeTune - piano-roll note editor for the Edit tab.
//
//  * notes are drawn where they will play, with the corrected pitch curve inside;
//    an outline shows where they were sung when they've been moved
//  * click to select (shift/cmd add), drag on empty space to select several,
//    drag a note up or down to change its pitch (snaps to semitones or the scale;
//    hold Option for free movement), double-click to snap it
//  * scroll with the wheel / trackpad, zoom with cmd+wheel (time), option+wheel
//    (pitch) or pinch; the strip at the bottom shows the whole capture
//  * with the host stopped, clicking a note loops it so you hear edits as you
//    make them, and the cursor can play the edited vocal from any point

#include "Theme.h"
#include "../PluginProcessor.h"

#include <functional>

namespace claudetune::ui
{

class NoteEditor : public juce::Component
{
public:
    enum class Snap { Chromatic = 0, Scale = 1, Free = 2 };
    enum class Tool { Select = 0, Split = 1, Draw = 2 };

    explicit NoteEditor (ClaudeTuneProcessor& processor);

    // called ~30 times a second by the editor
    void refresh();

    void setSnap (Snap s) noexcept { snapMode = s; }
    Snap getSnap() const noexcept { return snapMode; }
    void setFollowPlayhead (bool f) noexcept { follow = f; }
    void setAuditionOnClick (bool a) noexcept { auditionOnClick = a; }
    void zoomToFit();
    void zoomTime (double factor, float aroundX);
    void zoomPitch (float factor, float aroundY);
    void togglePreviewFromCursor();

    void setTool (Tool t);
    Tool getTool() const noexcept { return tool; }
    std::function<void()> onToolChanged;

    // joins selected notes that sit next to each other; returns true if anything merged
    bool mergeSelection();

    const std::vector<uint32_t>& getSelection() const noexcept { return selection; }
    void setSelection (std::vector<uint32_t> ids);
    std::function<void()> onSelectionChanged;

    // ---- geometry (also used by the tests) --------------------------------------
    juce::Rectangle<float> getNoteBounds (uint32_t id) const;
    juce::Rectangle<float> getRollArea() const noexcept { return rollArea; }
    juce::Rectangle<float> getRulerArea() const noexcept { return rulerArea; }
    double getCursorSeconds() const noexcept { return cursorSec; }
    double getViewStart() const noexcept { return viewStart; }
    double getSecondsPerPixel() const noexcept { return spp; }
    float getRowHeight() const noexcept { return rowH; }
    float getTopPitch() const noexcept { return topPitch; }
    float yForPitch (float midi) const noexcept { return rollArea.getY() + (topPitch - midi) * rowH; }
    float xForSeconds (double t) const noexcept { return rollArea.getX() + (float) ((t - viewStart) / spp); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scaleFactor) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    enum class Drag { None, PitchPending, Pitch, Time, StretchStart, StretchEnd, Marquee, Navigator, Ruler, Draw };

    double secondsForX (float x) const noexcept { return viewStart + (double) (x - rollArea.getX()) * spp; }
    float pitchForY (float y) const noexcept { return topPitch - (y - rollArea.getY()) / rowH; }
    double outSecondsForFrame (int frame) const noexcept;
    const edit::Note* noteAt (juce::Point<float> p) const;
    const edit::Note* findNote (uint32_t id) const;
    bool isSelected (uint32_t id) const;
    void selectionChanged();
    void clampView();
    void captureRange (double& t0, double& t1) const;
    float snapPitch (float raw, bool free) const;
    void applyPitchDelta (float delta);
    void transposeSelection (float semitones);
    void snapSelection();
    void selectNeighbour (int direction);
    void auditionNote (const edit::Note& n);
    double sourceSecondsForX (float x) const noexcept;
    void splitNoteAt (const edit::Note& n, float x);
    void drawStroke (juce::Point<float> from, juce::Point<float> to, bool erase);
    int edgeAt (const edit::Note& n, juce::Point<float> p) const;   // -1 left edge, +1 right edge, 0 body
    double snapToGrid (double seconds) const;                        // nearest 1/16 note (needs host tempo)
    void applyTimeDelta (double totalDelta);
    void nudgeSelection (double seconds);

    void drawRuler (juce::Graphics&);
    void drawKeys (juce::Graphics&);
    void drawRoll (juce::Graphics&);
    void drawNavigator (juce::Graphics&);
    void drawEmpty (juce::Graphics&);

    ClaudeTuneProcessor& proc;
    std::shared_ptr<const edit::SessionSnapshot> snap;
    uint64_t snapRevision = 0;
    std::array<bool, 128> allowed {};
    float refOffset = 0.0f;
    bool flats = false;

    // view
    juce::Rectangle<float> rulerArea, keysArea, rollArea, navArea;
    double viewStart = 0.0, spp = 0.01; // seconds at the left edge, seconds per pixel
    float topPitch = 76.0f, rowH = 14.0f;
    bool viewInitialised = false;

    // interaction
    Snap snapMode = Snap::Chromatic;
    Tool tool = Tool::Select;
    juce::Point<float> lastDrawPoint, hoverPos;
    bool drawErasing = false;
    bool follow = true, auditionOnClick = true;
    std::vector<uint32_t> selection;
    uint32_t hoverId = 0;
    Drag drag = Drag::None;
    juce::Point<float> dragStart;
    juce::Rectangle<float> marquee;
    std::vector<uint32_t> marqueeBase;
    std::vector<std::pair<uint32_t, float>> dragOrigins; // id, target pitch at drag start
    float dragPrimaryOrigin = 0.0f;
    float appliedDelta = 0.0f;
    double appliedTime = 0.0;
    uint32_t primaryId = 0;
    double primaryOutStart = 0.0, primaryOutEnd = 0.0;
    bool dragEdited = false;
    bool auditioningNote = false;
    double cursorSec = 0.0;
    int64_t lastPlayhead = -1, lastPreviewPos = -1;
    bool lastPlaying = false, lastPreviewing = false;
};

} // namespace claudetune::ui
