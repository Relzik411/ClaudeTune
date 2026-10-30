#include "NoteEditor.h"

#include <algorithm>
#include <cmath>

namespace claudetune::ui
{

namespace
{
    constexpr float kRulerH = 26.0f, kKeysW = 46.0f, kNavH = 30.0f;
    constexpr float kMinRowH = 5.0f, kMaxRowH = 40.0f;
    constexpr double kMinSpp = 1.0 / 4000.0;   // 4000 px per second
    constexpr float kLowestPitch = 24.0f, kHighestPitch = 108.0f;

    bool isBlackKey (int n) noexcept
    {
        const int pc = ((n % 12) + 12) % 12;
        return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
    }

    juce::String pitchName (float midi, bool flats)
    {
        const int n = (int) std::lround (midi);
        return juce::String (noteName (n, flats)) + juce::String (n / 12 - 1);
    }

    juce::String centsText (float cents)
    {
        const int c = (int) std::lround (cents);
        return (c > 0 ? "+" : "") + juce::String (c) + "c";
    }

    juce::String clockText (double sec, bool tenths)
    {
        if (sec < 0.0) sec = 0.0;
        const int m = (int) (sec / 60.0);
        const double s = sec - m * 60.0;
        return juce::String (m) + ":" + (tenths ? juce::String (s, 1).paddedLeft ('0', 4) : juce::String ((int) s).paddedLeft ('0', 2));
    }
}

//==============================================================================
NoteEditor::NoteEditor (ClaudeTuneProcessor& p) : proc (p)
{
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (true);
    setOpaque (false);
}

void NoteEditor::resized()
{
    auto r = getLocalBounds().toFloat();
    navArea = r.removeFromBottom (kNavH);
    r.removeFromBottom (4.0f);
    rulerArea = r.removeFromTop (kRulerH).withTrimmedLeft (kKeysW);
    keysArea = r.removeFromLeft (kKeysW);
    rollArea = r;
    clampView();
}

//==============================================================================
void NoteEditor::refresh()
{
    auto& session = proc.getSession();
    auto s = session.snapshot();
    bool needsRepaint = false;

    if (s && s->revision != snapRevision)
    {
        snap = s;
        snapRevision = s->revision;
        // forget selected notes that no longer exist (re-takes replace notes)
        const auto before = selection.size();
        selection.erase (std::remove_if (selection.begin(), selection.end(),
                                         [this] (uint32_t id) { return findNote (id) == nullptr; }),
                         selection.end());
        if (selection.size() != before)
            selectionChanged();
        if (! viewInitialised && ! snap->notes.empty())
            zoomToFit();
        needsRepaint = true;
    }

    const auto a = proc.getAllowedNotes();
    const float ref = proc.getReferenceOffset();
    const bool fl = proc.usesFlats();
    if (a != allowed || std::abs (ref - refOffset) > 1e-6f || fl != flats)
    {
        allowed = a;
        refOffset = ref;
        flats = fl;
        needsRepaint = true;
    }

    const auto ph = session.playheadSamples();
    const bool playing = session.isPlaying();
    const bool previewing = session.isPreviewing();
    const auto pv = session.previewPosition();
    if (ph != lastPlayhead || playing != lastPlaying || previewing != lastPreviewing || (previewing && pv != lastPreviewPos))
    {
        lastPlayhead = ph;
        lastPlaying = playing;
        lastPreviewing = previewing;
        lastPreviewPos = pv;
        needsRepaint = true;

        // keep the moving position in view
        if (follow && snap && snap->sampleRate > 0.0 && drag == Drag::None && (playing || previewing))
        {
            const double t = (double) (previewing ? pv : ph) / snap->sampleRate;
            const double width = rollArea.getWidth() * spp;
            if (t < viewStart || t > viewStart + 0.92 * width)
            {
                viewStart = t - 0.08 * width;
                clampView();
            }
        }
    }

    if (auditioningNote && ! previewing)
        auditioningNote = false;

    if (needsRepaint)
        repaint();
}

//==============================================================================
void NoteEditor::captureRange (double& t0, double& t1) const
{
    t0 = 0.0;
    t1 = 10.0;
    if (! snap || snap->coverage.empty() || snap->sampleRate <= 0.0)
        return;
    t0 = (double) snap->coverage.front().first / snap->sampleRate;
    t1 = (double) snap->coverage.back().second / snap->sampleRate;
    for (const auto& n : snap->notes)
    {
        t0 = std::min (t0, n.outStart);
        t1 = std::max (t1, n.outEnd);
    }
}

void NoteEditor::zoomToFit()
{
    if (rollArea.getWidth() <= 0.0f)
        return;

    double t0, t1;
    captureRange (t0, t1);
    if (snap && ! snap->notes.empty())
    {
        t0 = snap->notes.front().outStart;
        t1 = snap->notes.back().outEnd;
        for (const auto& n : snap->notes) { t0 = std::min (t0, n.outStart); t1 = std::max (t1, n.outEnd); }
    }
    const double pad = std::max (0.25, 0.04 * (t1 - t0));
    spp = std::max (kMinSpp, (t1 - t0 + 2.0 * pad) / rollArea.getWidth());
    viewStart = t0 - pad;

    float lo = 60.0f, hi = 72.0f;
    if (snap && ! snap->notes.empty())
    {
        lo = 200.0f; hi = -200.0f;
        for (const auto& n : snap->notes)
        {
            lo = std::min ({ lo, n.detectedPitch, n.targetPitch });
            hi = std::max ({ hi, n.detectedPitch, n.targetPitch });
        }
    }
    const float span = std::max (12.0f, hi - lo + 6.0f);
    rowH = std::clamp (rollArea.getHeight() / span, 8.0f, 22.0f);
    topPitch = 0.5f * (lo + hi) + 0.5f * rollArea.getHeight() / rowH;
    viewInitialised = true;
    clampView();
    repaint();
}

void NoteEditor::clampView()
{
    if (rollArea.getWidth() <= 0.0f)
        return;
    double t0, t1;
    captureRange (t0, t1);
    const double maxSpp = std::max (kMinSpp * 2.0, (std::max (t1, 10.0) + 2.0) / rollArea.getWidth());
    spp = std::clamp (spp, kMinSpp, maxSpp);
    const double width = rollArea.getWidth() * spp;
    viewStart = std::clamp (viewStart, std::min (0.0, t0 - 1.0), std::max (0.0, t1 + 1.0 - width * 0.5));
    viewStart = std::max (-0.5, viewStart);

    rowH = std::clamp (rowH, kMinRowH, kMaxRowH);
    const float visible = rollArea.getHeight() / rowH;
    topPitch = std::clamp (topPitch, kLowestPitch + visible, std::max (kLowestPitch + visible, kHighestPitch));
}

void NoteEditor::zoomTime (double factor, float aroundX)
{
    const double anchor = secondsForX (aroundX);
    spp /= factor;
    clampView();
    viewStart = anchor - (double) (aroundX - rollArea.getX()) * spp;
    clampView();
    repaint();
}

void NoteEditor::zoomPitch (float factor, float aroundY)
{
    const float anchor = pitchForY (aroundY);
    rowH *= factor;
    clampView();
    topPitch = anchor + (aroundY - rollArea.getY()) / rowH;
    clampView();
    repaint();
}

//==============================================================================
double NoteEditor::outSecondsForFrame (int frame) const noexcept
{
    const double srcSample = (double) frame * snap->hop;
    return snap->map.srcToOut (srcSample) / snap->sampleRate;
}

const edit::Note* NoteEditor::findNote (uint32_t id) const
{
    if (! snap) return nullptr;
    for (const auto& n : snap->notes)
        if (n.id == id) return &n;
    return nullptr;
}

bool NoteEditor::isSelected (uint32_t id) const
{
    return std::find (selection.begin(), selection.end(), id) != selection.end();
}

juce::Rectangle<float> NoteEditor::getNoteBounds (uint32_t id) const
{
    const auto* n = findNote (id);
    if (n == nullptr) return {};
    const float h = std::max (6.0f, rowH * 0.86f);
    const float x0 = xForSeconds (n->outStart), x1 = xForSeconds (n->outEnd);
    return { x0, yForPitch (n->targetPitch) - h * 0.5f, std::max (4.0f, x1 - x0), h };
}

const edit::Note* NoteEditor::noteAt (juce::Point<float> p) const
{
    if (! snap || ! rollArea.contains (p)) return nullptr;
    const edit::Note* best = nullptr;
    float bestDist = 1e9f;
    for (const auto& n : snap->notes)
    {
        const auto r = getNoteBounds (n.id).expanded (2.0f, 3.0f);
        if (r.contains (p))
        {
            const float d = std::abs (r.getCentreY() - p.y);
            if (d < bestDist) { bestDist = d; best = &n; }
        }
    }
    return best;
}

void NoteEditor::setSelection (std::vector<uint32_t> ids)
{
    selection = std::move (ids);
    selectionChanged();
}

void NoteEditor::selectionChanged()
{
    repaint();
    if (onSelectionChanged) onSelectionChanged();
}

float NoteEditor::snapPitch (float raw, bool free) const
{
    if (free || snapMode == Snap::Free)
        return raw;
    if (snapMode == Snap::Scale)
    {
        const float s = edit::EditDocument::nearestAllowed (raw, allowed, refOffset);
        if (s >= 0.0f) return s;
    }
    return refOffset + std::round (raw - refOffset);
}

//==============================================================================
void NoteEditor::applyPitchDelta (float delta)
{
    if (std::abs (delta - appliedDelta) < 1e-5f)
        return;
    appliedDelta = delta;
    const auto origins = dragOrigins;
    proc.getSession().edit ([&origins, delta] (edit::EditDocument& d) {
        for (const auto& [id, origin] : origins)
            d.setTargetPitch (id, origin + delta);
    }, ! dragEdited);
    dragEdited = true;
}

void NoteEditor::transposeSelection (float semitones)
{
    if (selection.empty()) return;
    const auto ids = selection;
    proc.getSession().edit ([&ids, semitones] (edit::EditDocument& d) {
        for (const auto id : ids)
            if (const auto* n = d.findNote (id))
                d.setTargetPitch (id, n->targetPitch + semitones);
    });
}

void NoteEditor::snapSelection()
{
    if (selection.empty()) return;
    const auto ids = selection;
    const auto mode = snapMode;
    const auto al = allowed;
    const float ref = refOffset;
    proc.getSession().edit ([&] (edit::EditDocument& d) {
        for (const auto id : ids)
            if (const auto* n = d.findNote (id))
            {
                float t = ref + std::round (n->targetPitch - ref);
                if (mode == Snap::Scale)
                {
                    const float s = edit::EditDocument::nearestAllowed (n->targetPitch, al, ref);
                    if (s >= 0.0f) t = s;
                }
                d.setTargetPitch (id, t);
            }
    });
}

void NoteEditor::selectNeighbour (int direction)
{
    if (! snap || snap->notes.empty()) return;
    const auto& notes = snap->notes;
    int idx = -1;
    if (! selection.empty())
        for (size_t i = 0; i < notes.size(); ++i)
            if (notes[i].id == selection.back()) idx = (int) i;
    if (idx < 0)
    {
        // nearest note after the cursor
        idx = 0;
        for (size_t i = 0; i < notes.size(); ++i)
            if (notes[i].outStart >= cursorSec) { idx = (int) i - (direction > 0 ? 1 : 0); break; }
    }
    idx = std::clamp (idx + direction, 0, (int) notes.size() - 1);
    setSelection ({ notes[(size_t) idx].id });

    // bring it into view
    const auto& n = notes[(size_t) idx];
    const double width = rollArea.getWidth() * spp;
    if (n.outStart < viewStart || n.outEnd > viewStart + width)
        viewStart = n.outStart - 0.2 * width;
    const float y = yForPitch (n.targetPitch);
    if (y < rollArea.getY() + rowH || y > rollArea.getBottom() - rowH)
        topPitch = n.targetPitch + 0.5f * rollArea.getHeight() / rowH;
    clampView();
    if (auditionOnClick && ! proc.getSession().isPlaying())
        auditionNote (n);
}

void NoteEditor::auditionNote (const edit::Note& n)
{
    if (! snap || snap->sampleRate <= 0.0) return;
    proc.getSession().startPreview ((int64_t) (n.outStart * snap->sampleRate),
                                    (int64_t) (n.outEnd * snap->sampleRate), true);
    auditioningNote = true;
}

void NoteEditor::togglePreviewFromCursor()
{
    auto& s = proc.getSession();
    if (s.isPreviewing())
    {
        s.stopPreview();
        return;
    }
    if (! snap || snap->sampleRate <= 0.0 || snap->coverage.empty()) return;
    const auto from = (int64_t) (cursorSec * snap->sampleRate);
    const auto to = snap->coverage.back().second;
    if (to > from)
    {
        s.startPreview (from, to, false);
        auditioningNote = false;
    }
}

void NoteEditor::setTool (Tool t)
{
    if (t == tool) return;
    tool = t;
    drag = Drag::None;
    setMouseCursor (juce::MouseCursor::NormalCursor);
    repaint();
    if (onToolChanged) onToolChanged();
}

double NoteEditor::sourceSecondsForX (float x) const noexcept
{
    if (! snap || snap->sampleRate <= 0.0) return secondsForX (x);
    return snap->map.outToSrc (secondsForX (x) * snap->sampleRate) / snap->sampleRate;
}

void NoteEditor::splitNoteAt (const edit::Note& n, float x)
{
    const double t = sourceSecondsForX (x);
    const uint32_t id = n.id;
    uint32_t created = 0;
    proc.getSession().edit ([&] (edit::EditDocument& d) { created = d.splitNote (id, t); });
    if (created != 0)
        setSelection ({ created });
}

bool NoteEditor::mergeSelection()
{
    if (! snap || selection.size() < 2) return false;

    // anything to merge? (two selected notes next to each other)
    bool any = false;
    for (size_t i = 1; i < snap->notes.size() && ! any; ++i)
        any = isSelected (snap->notes[i - 1].id) && isSelected (snap->notes[i].id);
    if (! any) return false;

    const auto ids = selection;
    std::vector<uint32_t> survivors;
    proc.getSession().edit ([&] (edit::EditDocument& d) {
        std::vector<uint32_t> ordered;
        for (const auto& n : d.notes())
            if (std::find (ids.begin(), ids.end(), n.id) != ids.end())
                ordered.push_back (n.id);
        uint32_t current = 0;
        int currentIdx = -10;
        for (const auto id : ordered)
        {
            const int idx = d.indexOf (id);
            if (current != 0 && idx == currentIdx + 1)
            {
                current = d.mergeNotes (current, id);
                currentIdx = d.indexOf (current);
            }
            else
            {
                current = id;
                currentIdx = idx;
                survivors.push_back (id);
            }
        }
    });
    setSelection (std::move (survivors));
    return true;
}

void NoteEditor::drawStroke (juce::Point<float> from, juce::Point<float> to, bool erase)
{
    if (! snap || snap->sampleRate <= 0.0 || snap->pitch.empty()) return;
    const int nf = (int) snap->pitch.size();
    auto frameAt = [this] (float x) {
        return (int) std::lround (sourceSecondsForX (x) * snap->sampleRate / snap->hop);
    };
    int f0 = frameAt (from.x), f1 = frameAt (to.x);
    float p0 = pitchForY (from.y), p1 = pitchForY (to.y);
    if (f1 < f0) { std::swap (f0, f1); std::swap (p0, p1); }
    const int a = std::max (0, f0), b = std::min (nf - 1, f1);
    if (b < a) return;

    if (erase)
    {
        proc.getSession().edit ([a, b] (edit::EditDocument& d) { d.clearDrawnPitch (a, b + 1); }, ! dragEdited);
    }
    else
    {
        std::vector<float> values;
        for (int f = a; f <= b; ++f)
        {
            const float t = f1 == f0 ? 0.0f : (float) (f - f0) / (float) (f1 - f0);
            values.push_back (p0 + t * (p1 - p0));
        }
        proc.getSession().edit ([a, &values] (edit::EditDocument& d) { d.setDrawnPitch (a, values); }, ! dragEdited);
    }
    dragEdited = true;
}

int NoteEditor::edgeAt (const edit::Note& n, juce::Point<float> p) const
{
    const auto r = getNoteBounds (n.id);
    if (r.getWidth() < 18.0f) return 0;
    const float grab = std::min (7.0f, r.getWidth() * 0.2f);
    if (std::abs (p.x - r.getX()) <= grab) return -1;
    if (std::abs (p.x - r.getRight()) <= grab) return 1;
    return 0;
}

double NoteEditor::snapToGrid (double seconds) const
{
    const auto tempo = proc.getSession().tempo();
    if (! tempo.isValid() || ! snap || snap->sampleRate <= 0.0) return seconds;
    const double q = tempo.quartersAt (seconds, snap->sampleRate);
    return tempo.secondsAtQuarters (std::round (q / 0.25) * 0.25, snap->sampleRate);
}

void NoteEditor::applyTimeDelta (double totalDelta)
{
    const double want = totalDelta - appliedTime;
    if (std::abs (want) < 1e-6) return;
    const auto ids = selection;
    double got = 0.0;
    proc.getSession().edit ([&] (edit::EditDocument& d) { got = d.moveNotesTime (ids, want); }, ! dragEdited);
    dragEdited = true;
    appliedTime += got;
}

void NoteEditor::nudgeSelection (double seconds)
{
    if (selection.empty()) return;
    const auto ids = selection;
    proc.getSession().edit ([&] (edit::EditDocument& d) { d.moveNotesTime (ids, seconds); });
}

//==============================================================================
// Mouse

void NoteEditor::mouseMove (const juce::MouseEvent& e)
{
    hoverPos = e.position;
    if (tool == Tool::Split) repaint();
    const auto* n = noteAt (e.position);
    const uint32_t id = n ? n->id : 0;
    if (id != hoverId)
    {
        hoverId = id;
        repaint();
    }
    if (rulerArea.contains (e.position) || navArea.contains (e.position))
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    else if (tool == Tool::Draw && rollArea.contains (e.position))
        setMouseCursor (juce::MouseCursor::CrosshairCursor);
    else if (tool == Tool::Split)
        setMouseCursor (n ? juce::MouseCursor::IBeamCursor : juce::MouseCursor::NormalCursor);
    else if (n != nullptr && edgeAt (*n, e.position) != 0)
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    else
        setMouseCursor (n ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
}

void NoteEditor::mouseExit (const juce::MouseEvent&)
{
    if (hoverId != 0) { hoverId = 0; repaint(); }
}

void NoteEditor::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    dragStart = e.position;
    drag = Drag::None;
    dragEdited = false;
    appliedDelta = 0.0f;

    if (navArea.contains (e.position))
    {
        drag = Drag::Navigator;
        mouseDrag (e);
        return;
    }
    if (rulerArea.contains (e.position))
    {
        drag = Drag::Ruler;
        cursorSec = std::max (0.0, secondsForX (e.position.x));
        repaint();
        return;
    }
    if (! rollArea.contains (e.position))
        return;

    if (tool == Tool::Draw)
    {
        drag = Drag::Draw;
        drawErasing = e.mods.isAltDown();
        lastDrawPoint = e.position;
        drawStroke (e.position, e.position, drawErasing);
        return;
    }
    if (tool == Tool::Split)
    {
        if (const auto* n = noteAt (e.position))
            splitNoteAt (*n, e.position.x);
        return;
    }

    const bool add = e.mods.isShiftDown() || e.mods.isCommandDown();
    if (const auto* n = noteAt (e.position))
    {
        if (e.mods.isCommandDown() && isSelected (n->id))
        {
            selection.erase (std::remove (selection.begin(), selection.end(), n->id), selection.end());
            selectionChanged();
            return;
        }
        if (! isSelected (n->id))
        {
            if (! add) selection.clear();
            selection.push_back (n->id);
            selectionChanged();
        }
        else if (! add && selection.size() > 1)
        {
            // keep the group selected so it can be dragged together
        }

        dragOrigins.clear();
        for (const auto id : selection)
            if (const auto* s = findNote (id))
                dragOrigins.push_back ({ id, s->targetPitch });
        dragPrimaryOrigin = n->targetPitch;
        primaryId = n->id;
        primaryOutStart = n->outStart;
        primaryOutEnd = n->outEnd;
        appliedTime = 0.0;

        // grabbing an edge stretches just that note
        if (const int edge = edgeAt (*n, e.position); edge != 0)
        {
            if (selection != std::vector<uint32_t> { n->id })
                setSelection ({ n->id });
            drag = edge < 0 ? Drag::StretchStart : Drag::StretchEnd;
        }
        else
            drag = Drag::PitchPending;

        if (auditionOnClick && ! proc.getSession().isPlaying())
            auditionNote (*n);
        return;
    }

    // empty space: move the cursor, start a selection rectangle
    cursorSec = std::max (0.0, secondsForX (e.position.x));
    marqueeBase = add ? selection : std::vector<uint32_t> {};
    if (! add && ! selection.empty())
    {
        selection.clear();
        selectionChanged();
    }
    drag = Drag::Marquee;
    marquee = {};
    repaint();
}

void NoteEditor::mouseDrag (const juce::MouseEvent& e)
{
    switch (drag)
    {
        case Drag::Navigator:
        {
            double t0, t1;
            captureRange (t0, t1);
            const double total = std::max (1.0, t1 - t0 + 2.0);
            const double t = (t0 - 1.0) + (double) ((e.position.x - navArea.getX()) / navArea.getWidth()) * total;
            viewStart = t - 0.5 * rollArea.getWidth() * spp;
            clampView();
            repaint();
            break;
        }
        case Drag::Ruler:
            cursorSec = std::max (0.0, secondsForX (e.position.x));
            repaint();
            break;

        case Drag::PitchPending:
        {
            // the first few pixels decide: up/down changes pitch, sideways moves in time
            const float dx = std::abs (e.position.x - dragStart.x), dy = std::abs (e.position.y - dragStart.y);
            if (std::max (dx, dy) < 4.0f)
                break;
            drag = dx > dy * 1.2f ? Drag::Time : Drag::Pitch;
            mouseDrag (e);
            break;
        }
        case Drag::Time:
        {
            double delta = (double) (e.position.x - dragStart.x) * spp;
            if (e.mods.isShiftDown())
                delta = snapToGrid (primaryOutStart + delta) - primaryOutStart;
            applyTimeDelta (delta);
            repaint();
            break;
        }
        case Drag::StretchStart:
        case Drag::StretchEnd:
        {
            const bool start = drag == Drag::StretchStart;
            double edge = (start ? primaryOutStart : primaryOutEnd) + (double) (e.position.x - dragStart.x) * spp;
            if (e.mods.isShiftDown())
                edge = snapToGrid (edge);
            const auto id = primaryId;
            const double s0 = start ? edge : primaryOutStart, e0 = start ? primaryOutEnd : edge;
            proc.getSession().edit ([id, s0, e0] (edit::EditDocument& d) { d.setNoteOutEdges (id, s0, e0); }, ! dragEdited);
            dragEdited = true;
            repaint();
            break;
        }
        case Drag::Pitch:
        {
            const float raw = dragPrimaryOrigin + (dragStart.y - e.position.y) / rowH;
            const float target = snapPitch (raw, e.mods.isAltDown());
            applyPitchDelta (target - dragPrimaryOrigin);
            repaint();
            break;
        }
        case Drag::Marquee:
        {
            marquee = juce::Rectangle<float> (dragStart, e.position).getIntersection (rollArea);
            std::vector<uint32_t> sel = marqueeBase;
            if (snap)
                for (const auto& n : snap->notes)
                    if (marquee.intersects (getNoteBounds (n.id)) && std::find (sel.begin(), sel.end(), n.id) == sel.end())
                        sel.push_back (n.id);
            if (sel != selection)
            {
                selection = std::move (sel);
                selectionChanged();
            }
            repaint();
            break;
        }
        case Drag::Draw:
            drawStroke (lastDrawPoint, e.position, drawErasing);
            lastDrawPoint = e.position;
            repaint();
            break;
        case Drag::None:
            break;
    }
}

void NoteEditor::mouseUp (const juce::MouseEvent&)
{
    if (auditioningNote)
    {
        proc.getSession().stopPreview();
        auditioningNote = false;
    }
    drag = Drag::None;
    marquee = {};
    repaint();
}

void NoteEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (rulerArea.contains (e.position))
    {
        cursorSec = std::max (0.0, secondsForX (e.position.x));
        proc.getSession().stopPreview();
        togglePreviewFromCursor();
        return;
    }
    if (noteAt (e.position) != nullptr)
        snapSelection();
}

void NoteEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        zoomTime (std::pow (2.0, (double) w.deltaY * 1.5), e.position.x);
        return;
    }
    if (e.mods.isAltDown())
    {
        zoomPitch (std::pow (2.0f, w.deltaY * 1.5f), e.position.y);
        return;
    }

    const float dx = e.mods.isShiftDown() ? w.deltaY : w.deltaX;
    const float dy = e.mods.isShiftDown() ? 0.0f : w.deltaY;
    viewStart -= (double) dx * rollArea.getWidth() * spp * 0.6;
    topPitch += dy * (rollArea.getHeight() / rowH) * 0.4f;
    clampView();
    repaint();
}

void NoteEditor::mouseMagnify (const juce::MouseEvent& e, float scale)
{
    zoomTime (scale, e.position.x);
}

bool NoteEditor::keyPressed (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    const int code = key.getKeyCode();

    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
    {
        const float step = mods.isShiftDown() ? 12.0f : (mods.isAltDown() ? 0.1f : 1.0f);
        transposeSelection (code == juce::KeyPress::upKey ? step : -step);
        return true;
    }
    if ((code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) && mods.isAltDown())
    {
        const double step = mods.isShiftDown() ? 0.05 : 0.01;
        nudgeSelection (code == juce::KeyPress::rightKey ? step : -step);
        return true;
    }
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
    {
        selectNeighbour (code == juce::KeyPress::rightKey ? 1 : -1);
        return true;
    }
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
    {
        const auto ids = selection;
        proc.getSession().edit ([&ids] (edit::EditDocument& d) { for (auto id : ids) d.resetNote (id); });
        return true;
    }
    if (code == juce::KeyPress::escapeKey)
    {
        proc.getSession().stopPreview();
        setSelection ({});
        return true;
    }
    if (code == juce::KeyPress::returnKey)
    {
        togglePreviewFromCursor();
        return true;
    }
    if (! mods.isCommandDown() && (code == '1' || code == '2' || code == '3'))
    {
        setTool ((Tool) (code - '1'));
        return true;
    }
    if (! mods.isCommandDown() && (code == 'M' || code == 'm'))
    {
        mergeSelection();
        return true;
    }
    if (mods.isCommandDown() && (code == 'A' || code == 'a'))
    {
        std::vector<uint32_t> all;
        if (snap) for (const auto& n : snap->notes) all.push_back (n.id);
        setSelection (std::move (all));
        return true;
    }
    if (mods.isCommandDown() && (code == 'Z' || code == 'z'))
    {
        if (mods.isShiftDown()) proc.getSession().redo();
        else proc.getSession().undo();
        return true;
    }
    if (! mods.isCommandDown() && (code == '=' || code == '+'))
    {
        zoomTime (1.5, rollArea.getCentreX());
        return true;
    }
    if (! mods.isCommandDown() && code == '-')
    {
        zoomTime (1.0 / 1.5, rollArea.getCentreX());
        return true;
    }
    if (! mods.isCommandDown() && (code == 'F' || code == 'f'))
    {
        zoomToFit();
        return true;
    }
    return false;
}

//==============================================================================
// Painting

void NoteEditor::paint (juce::Graphics& g)
{
    using namespace juce;
    g.setColour (Colours::background);
    g.fillRoundedRectangle (getLocalBounds().toFloat().withTrimmedBottom (kNavH + 4.0f), 8.0f);

    if (! snap || snap->coverage.empty() || snap->sampleRate <= 0.0)
    {
        drawEmpty (g);
        return;
    }

    drawRoll (g);
    drawKeys (g);
    drawRuler (g);
    drawNavigator (g);
}

void NoteEditor::drawEmpty (juce::Graphics& g)
{
    using namespace juce;
    auto bounds = getLocalBounds().toFloat().withTrimmedBottom (kNavH + 4.0f);
    const auto c = bounds.getCentre().translated (0.0f, -26.0f);
    Path wave;
    wave.startNewSubPath (c.x - 120.0f, c.y);
    for (int i = 1; i <= 60; ++i)
    {
        const float t = (float) i / 60.0f;
        const float amp = 14.0f * std::sin (t * 3.14159f) * (1.0f - t);
        wave.lineTo (c.x - 120.0f + 240.0f * t, c.y + amp * std::sin (t * 26.0f));
    }
    g.setColour (Colours::accent.withAlpha (0.8f));
    g.strokePath (wave, PathStrokeType (2.2f, PathStrokeType::curved, PathStrokeType::rounded));

    g.setColour (Colours::text);
    g.setFont (font (16.0f, true));
    g.drawText ("Press play in Logic", bounds.withTrimmedTop (bounds.getHeight() * 0.5f).withHeight (24.0f), Justification::centred);
    g.setColour (Colours::textDim);
    g.setFont (font (12.5f));
    g.drawFittedText ("ClaudeTune records the vocal as Logic plays it, finds the notes, and plays your edits back\n"
                      "in place. Put it first on the vocal track so it hears the raw recording.",
                      bounds.withTrimmedTop (bounds.getHeight() * 0.5f + 30.0f).withHeight (40.0f).toNearestInt(),
                      Justification::centredTop, 2);
}

void NoteEditor::drawRoll (juce::Graphics& g)
{
    using namespace juce;
    Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (rollArea.toNearestInt());

    const double sr = snap->sampleRate;
    const float bottomPitch = pitchForY (rollArea.getBottom());

    // ---- rows: black keys darker, notes outside the scale dimmer ----------------------
    for (int n = (int) std::floor (bottomPitch) - 1; n <= (int) std::ceil (topPitch) + 1; ++n)
    {
        const float y = yForPitch ((float) n + 0.5f + refOffset);
        const int idx = std::clamp (n, 0, 127);
        Colour c = isBlackKey (n) ? Colours::panel.withAlpha (0.55f) : Colours::panel.withAlpha (0.28f);
        if (! allowed[(size_t) idx])
            c = c.interpolatedWith (Colours::background, 0.55f);
        g.setColour (c);
        g.fillRect (rollArea.getX(), y, rollArea.getWidth(), rowH - 1.0f);
        if (((n % 12) + 12) % 12 == 0)
        {
            g.setColour (Colours::outline.withAlpha (0.9f));
            g.drawHorizontalLine ((int) (y + rowH - 1.0f), rollArea.getX(), rollArea.getRight());
        }
    }

    // ---- time grid ------------------------------------------------------------
    const auto tempo = proc.getSession().tempo();
    const double tA = viewStart, tB = secondsForX (rollArea.getRight());
    if (tempo.isValid())
    {
        const double qpb = tempo.quartersPerBar();
        const double beatQ = 4.0 / tempo.denominator;
        const float pxPerBeat = (float) (beatQ * 60.0 / tempo.bpm / spp);
        const double q0 = std::floor (tempo.quartersAt (tA, sr) / beatQ) * beatQ;
        for (double q = q0; ; q += beatQ)
        {
            const double t = tempo.secondsAtQuarters (q, sr);
            if (t > tB) break;
            const bool bar = std::abs (std::fmod (q, qpb)) < 1e-6 || std::abs (std::fmod (q, qpb) - qpb) < 1e-6;
            if (! bar && pxPerBeat < 10.0f) continue;
            g.setColour (Colours::outline.withAlpha (bar ? 0.85f : 0.35f));
            g.drawVerticalLine ((int) xForSeconds (t), rollArea.getY(), rollArea.getBottom());
        }
    }
    else
    {
        const double span = tB - tA;
        const double step = span > 120 ? 10.0 : span > 40 ? 5.0 : span > 12 ? 1.0 : span > 4 ? 0.5 : 0.1;
        for (double t = std::ceil (tA / step) * step; t < tB; t += step)
        {
            g.setColour (Colours::outline.withAlpha (0.5f));
            g.drawVerticalLine ((int) xForSeconds (t), rollArea.getY(), rollArea.getBottom());
        }
    }

    // ---- areas that haven't been captured are shaded ---------------------------------
    {
        double prevEnd = std::min (0.0, tA);
        auto shade = [&] (double a, double b) {
            if (b <= a) return;
            g.setColour (Colours::background.withAlpha (0.55f));
            g.fillRect (Rectangle<float>::leftTopRightBottom (xForSeconds (a), rollArea.getY(), xForSeconds (b), rollArea.getBottom()));
        };
        for (const auto& r : snap->coverage)
        {
            shade (prevEnd, (double) r.first / sr);
            prevEnd = (double) r.second / sr;
        }
        shade (prevEnd, std::max (prevEnd, tB));
    }
    if (snap->hasBackup)
    {
        g.setColour (Colours::sand.withAlpha (0.07f));
        g.fillRect (Rectangle<float>::leftTopRightBottom (xForSeconds ((double) snap->backupRange.first / sr), rollArea.getY(),
                                                         xForSeconds ((double) snap->backupRange.second / sr), rollArea.getBottom()));
    }

    // ---- the pitch that was sung (faint) ------------------------------------------------
    const double frameSec = (double) snap->hop / sr;
    const int nf = (int) snap->pitch.size();
    const int f0 = std::clamp ((int) (tA / frameSec) - 2, 0, nf), f1 = std::clamp ((int) (tB / frameSec) + 2, 0, nf);
    const int stride = std::max (1, (int) (frameSec > 0.0 ? 0.6 * spp / frameSec : 1));
    {
        Path p;
        bool open = false;
        for (int f = f0; f < f1; f += stride)
        {
            const float m = snap->pitch[(size_t) f];
            if (m < 0.0f) { open = false; continue; }
            const float x = xForSeconds (f * frameSec), y = yForPitch (m);
            if (! open) { p.startNewSubPath (x, y); open = true; } else p.lineTo (x, y);
        }
        g.setColour (Colours::sand.withAlpha (0.28f));
        g.strokePath (p, PathStrokeType (1.2f));
    }

    // ---- notes --------------------------------------------------------------------
    const float h = std::max (6.0f, rowH * 0.86f);
    const float corner = std::min (5.0f, h * 0.45f);
    for (const auto& n : snap->notes)
    {
        if (n.outEnd < tA - 0.1 && n.srcEnd < tA - 0.1) continue;
        if (n.outStart > tB + 0.1 && n.srcStart > tB + 0.1) continue;

        const bool sel = isSelected (n.id);
        const bool hover = n.id == hoverId;
        const bool moved = std::abs (n.targetPitch - n.detectedPitch) > 0.005f || n.hasTimingEdit();
        const auto r = getNoteBounds (n.id);

        if (moved)
        {
            // where it was sung
            const float gx0 = xForSeconds (n.srcStart), gx1 = xForSeconds (n.srcEnd);
            const auto ghost = Rectangle<float> (gx0, yForPitch (n.detectedPitch) - h * 0.5f, std::max (4.0f, gx1 - gx0), h);
            g.setColour (Colours::textDim.withAlpha (sel ? 0.7f : 0.4f));
            g.drawRoundedRectangle (ghost.reduced (0.5f), corner, 1.0f);
        }

        Colour fill = n.hasAnyEdit() ? Colours::accent : Colours::accent.interpolatedWith (Colours::panelRaised, 0.45f);
        if (hover) fill = fill.brighter (0.12f);
        if (sel) fill = fill.brighter (0.22f);
        g.setColour (fill.withAlpha (0.92f));
        g.fillRoundedRectangle (r, corner);
        if (sel)
        {
            g.setColour (Colours::text);
            g.drawRoundedRectangle (r.reduced (0.75f), corner, 1.5f);
        }

        // the pitch it will play, inside the note
        const int a = std::clamp ((int) std::lround (n.srcStart / frameSec), 0, nf);
        const int b = std::clamp ((int) std::lround (n.srcEnd / frameSec), a, nf);
        Path curve;
        bool open = false;
        const int step = std::max (1, stride);
        for (int f = a; f < b; f += step)
        {
            const float m = (size_t) f < snap->outPitch.size() ? snap->outPitch[(size_t) f] : -1.0f;
            if (m < 0.0f) { open = false; continue; }
            const float x = xForSeconds (outSecondsForFrame (f));
            const float y = yForPitch (m);
            if (! open) { curve.startNewSubPath (x, y); open = true; } else curve.lineTo (x, y);
        }
        g.setColour (Colours::text.withAlpha (sel ? 0.9f : 0.62f));
        g.strokePath (curve, PathStrokeType (1.4f, PathStrokeType::curved, PathStrokeType::rounded));

        // label just above the note: the pitch it plays, and how far off the sung pitch is
        if (r.getWidth() > 30.0f && rowH >= 9.0f)
        {
            juce::String label = pitchName (n.targetPitch - refOffset, flats);
            if (! moved)
                label << "  " << centsText ((n.detectedPitch - refOffset - std::round (n.detectedPitch - refOffset)) * 100.0f);
            else if (std::abs (n.targetPitch - std::round (n.targetPitch - refOffset) - refOffset) > 0.005f)
                label << "  " << centsText ((n.targetPitch - refOffset - std::round (n.targetPitch - refOffset)) * 100.0f);
            g.setColour (sel ? Colours::text : Colours::textDim);
            g.setFont (font (10.5f, true));
            g.drawText (label, Rectangle<float> (r.getX() + 1.0f, r.getY() - 14.0f, std::max (60.0f, r.getWidth()), 13.0f),
                        Justification::bottomLeft, false);
        }
    }

    // ---- where the Split tool would cut ---------------------------------------------------
    if (tool == Tool::Split && hoverId != 0)
    {
        const auto r = getNoteBounds (hoverId).expanded (0.0f, 4.0f);
        g.setColour (Colours::text);
        const float dash[] = { 3.0f, 3.0f };
        g.drawDashedLine (Line<float> (hoverPos.x, r.getY(), hoverPos.x, r.getBottom()), dash, 2, 1.5f);
    }

    // ---- marquee, cursor, playhead -------------------------------------------------------
    if (! marquee.isEmpty())
    {
        g.setColour (Colours::text.withAlpha (0.08f));
        g.fillRect (marquee);
        g.setColour (Colours::text.withAlpha (0.5f));
        g.drawRect (marquee, 1.0f);
    }

    g.setColour (Colours::teal.withAlpha (0.8f));
    g.drawVerticalLine ((int) xForSeconds (cursorSec), rollArea.getY(), rollArea.getBottom());

    auto& session = proc.getSession();
    if (session.isPreviewing())
    {
        g.setColour (Colours::teal);
        g.fillRect (xForSeconds ((double) session.previewPosition() / sr) - 0.75f, rollArea.getY(), 1.5f, rollArea.getHeight());
    }
    else
    {
        g.setColour (session.isPlaying() ? Colours::text : Colours::textDim.withAlpha (0.6f));
        g.fillRect (xForSeconds ((double) session.playheadSamples() / sr) - 0.6f, rollArea.getY(), 1.2f, rollArea.getHeight());
    }
}

void NoteEditor::drawKeys (juce::Graphics& g)
{
    using namespace juce;
    Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (keysArea.toNearestInt());
    g.setColour (Colours::panel);
    g.fillRect (keysArea);

    const float bottomPitch = pitchForY (keysArea.getBottom());
    const float hoverPitch = hoverId != 0 && findNote (hoverId) ? findNote (hoverId)->targetPitch - refOffset : -100.0f;
    for (int n = (int) std::floor (bottomPitch) - 1; n <= (int) std::ceil (topPitch) + 1; ++n)
    {
        const float y = yForPitch ((float) n + 0.5f + refOffset);
        auto key = Rectangle<float> (keysArea.getX(), y, keysArea.getWidth() - 2.0f, rowH - 1.0f);
        const bool black = isBlackKey (n);
        g.setColour (black ? Colour (0xff2a2d33) : Colour (0xffd9d5ce));
        g.fillRect (black ? key.withTrimmedRight (14.0f) : key);
        if (std::lround (hoverPitch) == n)
        {
            g.setColour (Colours::accent.withAlpha (0.7f));
            g.fillRect (key);
        }
        if (allowed[(size_t) std::clamp (n, 0, 127)])
        {
            g.setColour (Colours::accent);
            g.fillEllipse (key.getRight() - 9.0f, key.getCentreY() - 2.5f, 5.0f, 5.0f);
        }
        if (((n % 12) + 12) % 12 == 0 && rowH >= 7.0f)
        {
            g.setColour (Colour (0xff4a4e57));
            g.setFont (font (std::min (11.0f, rowH), true));
            g.drawText ("C" + String (n / 12 - 1), key.withTrimmedLeft (4.0f), Justification::centredLeft);
        }
    }
}

void NoteEditor::drawRuler (juce::Graphics& g)
{
    using namespace juce;
    Graphics::ScopedSaveState save (g);
    g.setColour (Colours::panel);
    g.fillRect (rulerArea.withX (keysArea.getX()).withWidth (rulerArea.getRight() - keysArea.getX()));
    g.reduceClipRegion (rulerArea.toNearestInt());

    const double sr = snap->sampleRate;
    const double tA = viewStart, tB = secondsForX (rulerArea.getRight());
    g.setFont (font (11.0f, true));
    const auto tempo = proc.getSession().tempo();

    if (tempo.isValid())
    {
        const double qpb = tempo.quartersPerBar();
        const float pxPerBar = (float) (qpb * 60.0 / tempo.bpm / spp);
        int every = 1;
        while (pxPerBar * (float) every < 44.0f) every *= 2;
        const double beatQ = 4.0 / tempo.denominator;
        const float pxPerBeat = (float) (beatQ * 60.0 / tempo.bpm / spp);
        const double qStart = std::floor (tempo.quartersAt (tA, sr) / beatQ) * beatQ;
        for (double q = qStart; ; q += beatQ)
        {
            const double t = tempo.secondsAtQuarters (q, sr);
            if (t > tB) break;
            const double barF = q / qpb;
            const bool isBar = std::abs (barF - std::round (barF)) < 1e-6;
            const float x = xForSeconds (t);
            if (isBar)
            {
                const int bar = (int) std::lround (barF) + 1;
                if (((bar - 1) % every) == 0)
                {
                    g.setColour (Colours::textDim);
                    g.drawVerticalLine ((int) x, rulerArea.getY() + 10.0f, rulerArea.getBottom());
                    g.setColour (Colours::text.withAlpha (0.85f));
                    g.drawText (String (bar), Rectangle<float> (x + 3.0f, rulerArea.getY() + 1.0f, 40.0f, 14.0f), Justification::centredLeft);
                }
            }
            else if (pxPerBeat >= 10.0f)
            {
                g.setColour (Colours::outline.brighter (0.3f));
                g.drawVerticalLine ((int) x, rulerArea.getBottom() - 6.0f, rulerArea.getBottom());
            }
        }
    }
    else
    {
        const double span = tB - tA;
        const double step = span > 120 ? 10.0 : span > 40 ? 5.0 : span > 12 ? 1.0 : span > 4 ? 0.5 : 0.1;
        for (double t = std::ceil (tA / step) * step; t < tB; t += step)
        {
            const float x = xForSeconds (t);
            g.setColour (Colours::textDim);
            g.drawVerticalLine ((int) x, rulerArea.getY() + 10.0f, rulerArea.getBottom());
            g.setColour (Colours::text.withAlpha (0.85f));
            g.drawText (clockText (t, step < 1.0), Rectangle<float> (x + 3.0f, rulerArea.getY() + 1.0f, 50.0f, 14.0f), Justification::centredLeft);
        }
    }

    // cursor marker
    const float cx = xForSeconds (cursorSec);
    Path tri;
    tri.addTriangle (cx - 6.0f, rulerArea.getBottom() - 9.0f, cx + 6.0f, rulerArea.getBottom() - 9.0f, cx, rulerArea.getBottom());
    g.setColour (Colours::teal);
    g.fillPath (tri);
}

void NoteEditor::drawNavigator (juce::Graphics& g)
{
    using namespace juce;
    g.setColour (Colours::background);
    g.fillRoundedRectangle (navArea, 6.0f);

    double t0, t1;
    captureRange (t0, t1);
    t0 -= 1.0;
    t1 += 1.0;
    const double total = std::max (1.0, t1 - t0);
    auto xOf = [&] (double t) { return navArea.getX() + (float) ((t - t0) / total) * navArea.getWidth(); };

    float lo = 200.0f, hi = -200.0f;
    for (const auto& n : snap->notes) { lo = std::min (lo, n.targetPitch); hi = std::max (hi, n.targetPitch); }
    if (hi < lo) { lo = 60.0f; hi = 72.0f; }
    const float span = std::max (6.0f, hi - lo);
    auto yOf = [&] (float m) { return navArea.getBottom() - 5.0f - (m - lo) / span * (navArea.getHeight() - 10.0f); };

    for (const auto& r : snap->coverage)
    {
        g.setColour (Colours::teal.withAlpha (0.35f));
        g.fillRect (Rectangle<float>::leftTopRightBottom (xOf ((double) r.first / snap->sampleRate), navArea.getBottom() - 3.0f,
                                                         xOf ((double) r.second / snap->sampleRate), navArea.getBottom() - 1.0f));
    }
    for (const auto& n : snap->notes)
    {
        g.setColour ((isSelected (n.id) ? Colours::text : Colours::accent).withAlpha (0.8f));
        g.fillRect (Rectangle<float>::leftTopRightBottom (xOf (n.outStart), yOf (n.targetPitch) - 1.0f,
                                                         std::max (xOf (n.outStart) + 1.0f, xOf (n.outEnd)), yOf (n.targetPitch) + 1.0f));
    }

    const auto view = Rectangle<float>::leftTopRightBottom (xOf (viewStart), navArea.getY() + 1.0f,
                                                           xOf (secondsForX (rollArea.getRight())), navArea.getBottom() - 1.0f);
    g.setColour (Colours::text.withAlpha (0.08f));
    g.fillRoundedRectangle (view, 4.0f);
    g.setColour (Colours::text.withAlpha (0.45f));
    g.drawRoundedRectangle (view, 4.0f, 1.0f);

    const double ph = (double) (proc.getSession().isPreviewing() ? proc.getSession().previewPosition()
                                                                  : proc.getSession().playheadSamples()) / snap->sampleRate;
    g.setColour (Colours::text.withAlpha (0.7f));
    g.drawVerticalLine ((int) xOf (ph), navArea.getY() + 2.0f, navArea.getBottom() - 2.0f);
}

} // namespace claudetune::ui
