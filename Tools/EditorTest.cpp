// Drives the Edit tab's note editor with synthetic mouse and keyboard events,
// the way a user would: click, drag, marquee, double-click, zoom, keys, preview.
// Development tool only.

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "TestSignals.h"

using namespace claudetune;

static int failures = 0;
static void check (bool ok, const juce::String& what)
{
    std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (! ok) ++failures;
}

struct TestPlayHead : juce::AudioPlayHead
{
    juce::int64 time = 0;
    bool playing = false;
    double bpm = 0.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (playing);
        p.setTimeInSamples (time);
        if (bpm > 0.0)
        {
            p.setBpm (bpm);
            p.setPpqPosition ((double) time / 48000.0 * bpm / 60.0);
            p.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
        }
        return p;
    }
};

// ---- synthetic input ----------------------------------------------------------------
struct Mouse
{
    juce::Component& target;
    juce::Point<float> downPos;
    juce::Time downTime;

    juce::MouseEvent make (juce::Point<float> pos, juce::ModifierKeys mods, int clicks, bool dragged) const
    {
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, mods,
                                 juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f,
                                 &target, &target, juce::Time::getCurrentTime(), downPos, downTime, clicks, dragged);
    }

    void click (juce::Point<float> p, juce::ModifierKeys mods = {})
    {
        downPos = p;
        downTime = juce::Time::getCurrentTime();
        const auto m = mods.withFlags (juce::ModifierKeys::leftButtonModifier);
        target.mouseDown (make (p, m, 1, false));
        target.mouseUp (make (p, mods, 1, false));
    }

    void doubleClick (juce::Point<float> p, juce::ModifierKeys mods = {})
    {
        click (p, mods);
        target.mouseDoubleClick (make (p, mods, 2, false));
    }

    void drag (juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys mods = {}, int steps = 12,
               std::function<void()> whileHeld = {})
    {
        downPos = from;
        downTime = juce::Time::getCurrentTime();
        const auto m = mods.withFlags (juce::ModifierKeys::leftButtonModifier);
        target.mouseDown (make (from, m, 1, false));
        for (int i = 1; i <= steps; ++i)
        {
            const auto p = from + (to - from) * ((float) i / (float) steps);
            target.mouseDrag (make (p, m, 1, true));
        }
        if (whileHeld) whileHeld();
        target.mouseUp (make (to, mods, 1, true));
    }

    void wheel (juce::Point<float> p, float dy, juce::ModifierKeys mods = {})
    {
        juce::MouseWheelDetails w;
        w.deltaX = 0.0f;
        w.deltaY = dy;
        w.isReversed = false;
        w.isSmooth = false;
        w.isInertial = false;
        target.mouseWheelMove (make (p, mods, 0, false), w);
    }
};

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    const double sr = 48000.0;
    const auto ph = testsig::makePhrase (sr);

    ClaudeTuneProcessor proc;
    TestPlayHead head;
    head.bpm = 96.0;
    proc.setPlayHead (&head);
    proc.setPlayConfigDetails (1, 1, sr, 512);
    proc.prepareToPlay (sr, 512);
    proc.apvts.getParameter (ParamIDs::scale)->setValueNotifyingHost (proc.apvts.getParameter (ParamIDs::scale)->convertTo0to1 (1)); // C major
    proc.setMode (ClaudeTuneProcessor::Mode::Edit);

    // capture the phrase (starts 1 s into the song)
    std::vector<float> timeline ((size_t) sr, 0.0f);
    timeline.insert (timeline.end(), ph.audio.begin(), ph.audio.end());
    juce::AudioBuffer<float> buf (1, 512);
    juce::MidiBuffer midi;
    auto runBlocks = [&] (int blocks, bool playing, juce::int64 start, const std::vector<float>* src) {
        head.playing = playing;
        for (int b = 0; b < blocks; ++b)
        {
            const auto pos = start + (juce::int64) b * 512;
            for (int i = 0; i < 512; ++i)
                buf.setSample (0, i, src && pos + i < (juce::int64) src->size() ? (*src)[(size_t) (pos + i)] : 0.0f);
            head.time = pos;
            proc.processBlock (buf, midi);
        }
    };
    runBlocks ((int) (timeline.size() / 512), true, 0, &timeline);
    runBlocks (1, false, 0, nullptr);
    auto& session = proc.getSession();
    session.waitUntilIdle (20.0);

    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
    ed->setVisible (true);
    auto& page = dynamic_cast<ClaudeTuneEditor&> (*ed).getEditPage();
    auto& editor = page.getNoteEditor();
    auto pump = [&] (int ms = 40) {
        session.waitUntilIdle (10.0);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (ms);
        page.refresh();
    };
    pump (200);

    Mouse mouse { editor };
    const auto notes0 = session.snapshot()->notes;
    auto noteById = [&] (uint32_t id) {
        for (const auto& n : session.snapshot()->notes) if (n.id == id) return n;
        return edit::Note {};
    };

    // ---- 1. the view fits the capture ----------------------------------------------------
    {
        bool allVisible = ! notes0.empty();
        for (const auto& n : notes0)
            allVisible = allVisible && editor.getRollArea().contains (editor.getNoteBounds (n.id));
        check (allVisible && notes0.size() == ph.notes.size(),
               "all " + juce::String ((int) notes0.size()) + " notes fit in view after capture");
    }

    // ---- 2. click selects and auditions (host stopped) ---------------------------------------
    const auto n2 = notes0[2];
    {
        const auto c = editor.getNoteBounds (n2.id).getCentre();
        mouse.downPos = c;
        mouse.downTime = juce::Time::getCurrentTime();
        editor.mouseDown (mouse.make (c, juce::ModifierKeys::leftButtonModifier, 1, false));
        const bool selected = editor.getSelection() == std::vector<uint32_t> { n2.id };
        const bool auditioning = session.isPreviewing();
        runBlocks (8, false, 0, nullptr);
        const bool advancing = session.previewPosition() > (int64_t) (n2.outStart * sr);
        editor.mouseUp (mouse.make (c, {}, 1, false));
        check (selected && auditioning && advancing && ! session.isPreviewing(),
               "clicking a note selects it and loops it while held");
    }

    // ---- 3. drag up two rows: snaps to a semitone; one undo step ------------------------------
    {
        const auto c = editor.getNoteBounds (n2.id).getCentre();
        mouse.drag (c, c.translated (0.0f, -2.0f * editor.getRowHeight()));
        pump();
        const auto n = noteById (n2.id);
        const float expect = std::round (n2.detectedPitch + 2.0f);
        check (std::abs (n.targetPitch - expect) < 1e-4f,
               "drag up 2 rows: " + juce::String (n2.detectedPitch, 2) + " -> " + juce::String (n.targetPitch, 2)
                   + " (expected " + juce::String (expect, 0) + ")");
        session.undo();
        pump();
        check (std::abs (noteById (n2.id).targetPitch - n2.detectedPitch) < 1e-4f, "one undo reverts the whole drag");
    }

    // ---- 4. option-drag moves freely ---------------------------------------------------------
    {
        const auto c = editor.getNoteBounds (n2.id).getCentre();
        mouse.drag (c, c.translated (0.0f, -0.37f * editor.getRowHeight()), juce::ModifierKeys::altModifier);
        pump();
        const float moved = noteById (n2.id).targetPitch - n2.detectedPitch;
        check (std::abs (moved - 0.37f) < 0.03f, "option-drag moves freely (" + juce::String (moved * 100.0f, 0) + " cents)");
    }

    // ---- 5. double-click snaps to the nearest semitone ------------------------------------------
    {
        mouse.doubleClick (editor.getNoteBounds (n2.id).getCentre());
        pump();
        const float t = noteById (n2.id).targetPitch;
        check (std::abs (t - std::round (t)) < 1e-4f, "double-click snaps the note to " + juce::String (t, 2));
    }

    // ---- 6. scale snap: a C# drag lands on a C major note --------------------------------------
    {
        page.getNoteEditor().setSnap (ui::NoteEditor::Snap::Scale);
        const auto n1 = notes0[1]; // sung around C#4
        const auto c = editor.getNoteBounds (n1.id).getCentre();
        mouse.drag (c, c.translated (0.0f, -0.3f * editor.getRowHeight()));
        pump();
        const float t = noteById (n1.id).targetPitch;
        const int pc = ((int) std::lround (t)) % 12;
        const bool inC = pc == 0 || pc == 2 || pc == 4 || pc == 5 || pc == 7 || pc == 9 || pc == 11;
        check (inC && std::abs (t - std::round (t)) < 1e-4f,
               "scale snap: " + juce::String (n1.detectedPitch, 2) + " lands on " + juce::String (t, 0));
        page.getNoteEditor().setSnap (ui::NoteEditor::Snap::Chromatic);
    }

    // ---- 7. marquee selects several; dragging one moves them all ----------------------------------
    std::vector<uint32_t> group;
    {
        const auto a = editor.getNoteBounds (notes0[4].id), b = editor.getNoteBounds (notes0[6].id);
        const juce::Point<float> from (a.getX() - 4.0f, editor.getRollArea().getY() + 4.0f);
        const juce::Point<float> to (b.getRight() - 6.0f, editor.getRollArea().getBottom() - 4.0f); // note 7 starts where 6 ends
        mouse.drag (from, to);
        group = editor.getSelection();
        check (group.size() == 3, "marquee selects " + juce::String ((int) group.size()) + " notes (expected 3)");

        std::vector<float> before;
        for (auto id : group) before.push_back (noteById (id).targetPitch);
        const auto c = editor.getNoteBounds (group[1]).getCentre();
        mouse.drag (c, c.translated (0.0f, editor.getRowHeight()));
        pump();
        bool same = true;
        float delta0 = noteById (group[0]).targetPitch - before[0];
        for (size_t i = 0; i < group.size(); ++i)
            same = same && std::abs ((noteById (group[i]).targetPitch - before[i]) - delta0) < 1e-4f;
        check (same && delta0 < -0.4f, "dragging one of a group moves all of them by " + juce::String (delta0, 2) + " st");
    }

    // ---- 8. keyboard ------------------------------------------------------------------------------
    {
        editor.setSelection ({ notes0[0].id });
        const float t0 = noteById (notes0[0].id).targetPitch;
        editor.keyPressed (juce::KeyPress (juce::KeyPress::upKey));
        pump();
        const float up = noteById (notes0[0].id).targetPitch - t0;
        editor.keyPressed (juce::KeyPress (juce::KeyPress::rightKey));
        const bool next = editor.getSelection() == std::vector<uint32_t> { notes0[1].id };
        session.stopPreview();
        editor.setSelection ({ notes0[0].id });
        editor.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey));
        pump();
        const bool reset = ! noteById (notes0[0].id).hasAnyEdit();
        editor.keyPressed (juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 'a'));
        const bool all = editor.getSelection().size() == notes0.size();
        editor.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
        const bool none = editor.getSelection().empty();
        check (std::abs (up - 1.0f) < 1e-4f && next && reset && all && none,
               "keys: up +1 st, right = next note, delete = reset, cmd+A, Esc");
    }

    // ---- 9. Correct on a selection only touches those notes -------------------------------------
    {
        session.edit ([] (edit::EditDocument& d) { d.resetAll(); });
        pump();
        editor.setSelection ({ notes0[3].id, notes0[5].id });
        proc.correctAllNotes (editor.getSelection());
        pump();
        int edited = 0;
        bool right = true;
        for (const auto& n : session.snapshot()->notes)
        {
            if (n.hasAnyEdit()) ++edited;
            if (n.id == notes0[3].id || n.id == notes0[5].id) right = right && n.hasAnyEdit();
        }
        check (edited == 2 && right, "Correct with 2 notes selected edits just those 2");
    }

    // ---- 10. zoom and scroll --------------------------------------------------------------------
    {
        const auto roll = editor.getRollArea();
        const juce::Point<float> p (roll.getX() + roll.getWidth() * 0.6f, roll.getCentreY());
        const double tBefore = editor.getViewStart() + (p.x - roll.getX()) * editor.getSecondsPerPixel();
        const double sppBefore = editor.getSecondsPerPixel();
        mouse.wheel (p, 0.5f, juce::ModifierKeys::commandModifier);
        const double tAfter = editor.getViewStart() + (p.x - roll.getX()) * editor.getSecondsPerPixel();
        const bool zoomed = editor.getSecondsPerPixel() < sppBefore * 0.7;
        const bool anchored = std::abs (tAfter - tBefore) < 2.0 * editor.getSecondsPerPixel();

        const float rowBefore = editor.getRowHeight();
        mouse.wheel (p, 0.5f, juce::ModifierKeys::altModifier);
        const bool rows = editor.getRowHeight() > rowBefore * 1.2f;

        const float topBefore = editor.getTopPitch();
        mouse.wheel (p, -0.3f);
        const bool scrolled = editor.getTopPitch() < topBefore;

        editor.keyPressed (juce::KeyPress ('f', {}, 'f'));
        bool fit = true;
        for (const auto& n : session.snapshot()->notes)
            fit = fit && editor.getRollArea().contains (editor.getNoteBounds (n.id));
        check (zoomed && anchored && rows && scrolled && fit,
               "cmd+wheel zooms time around the mouse, option+wheel zooms pitch, wheel scrolls, F fits");
    }

    // ---- 11. ruler click moves the cursor; Return previews from it ---------------------------------
    {
        const auto ruler = editor.getRulerArea();
        const float x = ruler.getX() + ruler.getWidth() * 0.3f;
        mouse.click ({ x, ruler.getCentreY() });
        const double cursor = editor.getCursorSeconds();
        const double expected = editor.getViewStart() + (x - ruler.getX()) * editor.getSecondsPerPixel();
        editor.keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
        const bool started = session.isPreviewing();
        runBlocks (20, false, 0, nullptr);
        const auto pos = session.previewPosition();
        const bool fromCursor = pos > (int64_t) (cursor * sr) && pos < (int64_t) (cursor * sr) + 21 * 512;
        editor.keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
        runBlocks (4, false, 0, nullptr);
        check (std::abs (cursor - expected) < 0.01 && started && fromCursor && ! session.isPreviewing(),
               "ruler click sets the cursor; Return starts and stops the preview from it");
    }

    // ---- 12. host playback overrides the preview ------------------------------------------------------
    {
        page.refresh();
        editor.togglePreviewFromCursor();
        runBlocks (2, false, 0, nullptr);
        runBlocks (2, true, 48000, &timeline);
        check (! session.isPreviewing(), "pressing play in the host stops the preview");
        runBlocks (1, false, 0, nullptr);
    }

    // ---- 13. bars and beats from the host tempo -------------------------------------------------------
    {
        const auto tempo = session.tempo();
        check (tempo.isValid() && std::abs (tempo.bpm - 96.0) < 1e-9 && tempo.quartersPerBar() == 4.0,
               "ruler has the host tempo (96 bpm, 4/4)");
    }

    // ---- 14. Split tool: click a note to cut it where you clicked -------------------------------
    session.edit ([] (edit::EditDocument& d) { d.resetAll(); });
    pump();
    editor.keyPressed (juce::KeyPress ('f', {}, 'f'));
    uint32_t firstHalf = 0, secondHalf = 0;
    {
        const auto before = session.snapshot()->notes;
        const auto n = before[3];
        editor.keyPressed (juce::KeyPress ('2', {}, '2'));
        const bool tool = editor.getTool() == ui::NoteEditor::Tool::Split;
        const auto r = editor.getNoteBounds (n.id);
        const float x = r.getX() + 0.6f * r.getWidth();
        const double cutAt = editor.getViewStart() + (x - editor.getRollArea().getX()) * editor.getSecondsPerPixel();
        mouse.click ({ x, r.getCentreY() });
        pump();
        const auto after = session.snapshot()->notes;
        const edit::Note* a = nullptr; const edit::Note* b = nullptr;
        for (size_t i = 0; i + 1 < after.size(); ++i)
            if (after[i].id == n.id) { a = &after[i]; b = &after[i + 1]; }
        const bool split = after.size() == before.size() + 1 && a && b && std::abs (a->srcEnd - cutAt) < 0.01
                           && std::abs (b->srcStart - cutAt) < 0.01 && editor.getSelection() == std::vector<uint32_t> { b->id };
        if (a && b) { firstHalf = a->id; secondHalf = b->id; }
        check (tool && split, "Split tool cuts the note where you click (" + juce::String (cutAt, 3) + " s)");
    }

    // ---- 15. Merge joins them back (M) ---------------------------------------------------------
    {
        const auto before = session.snapshot()->notes.size();
        editor.setSelection ({ firstHalf, secondHalf });
        editor.keyPressed (juce::KeyPress ('m', {}, 'm'));
        pump();
        const auto after = session.snapshot()->notes;
        bool joined = false;
        for (const auto& n : after) if (n.id == firstHalf) joined = std::abs (n.srcEnd - notes0[3].srcEnd) < 1e-6;
        check (after.size() == before - 1 && joined && editor.getSelection() == std::vector<uint32_t> { firstHalf },
               "M merges the selected neighbours back into one note");
    }

    // ---- 16. Draw tool: a stroke sets the pitch; Option-stroke erases it; one undo per stroke --------
    {
        editor.keyPressed (juce::KeyPress ('3', {}, '3'));
        const auto n = noteById (notes0[3].id);
        const auto r = editor.getNoteBounds (n.id);
        const float target = n.detectedPitch + 1.5f;
        const float y = editor.yForPitch (target);
        mouse.drag ({ r.getX() + 0.2f * r.getWidth(), y }, { r.getX() + 0.8f * r.getWidth(), y });
        pump();
        auto snapNow = session.snapshot();
        const double frameSec = (double) snapNow->hop / sr;
        const int fa = (int) ((n.srcStart + 0.35 * (n.srcEnd - n.srcStart)) / frameSec);
        const int fb = (int) ((n.srcStart + 0.65 * (n.srcEnd - n.srcStart)) / frameSec);
        double worst = 0;
        for (int f = fa; f < fb; ++f)
            if (snapNow->pitch[(size_t) f] >= 0.0f)
                worst = std::max (worst, (double) std::abs (snapNow->outPitch[(size_t) f] - target));
        const bool drew = worst < 0.05;

        mouse.drag ({ r.getX() + 0.1f * r.getWidth(), y }, { r.getX() + 0.9f * r.getWidth(), y }, juce::ModifierKeys::altModifier);
        pump();
        snapNow = session.snapshot();
        double residual = 0;
        for (int f = fa; f < fb; ++f)
            if (snapNow->pitch[(size_t) f] >= 0.0f)
                residual = std::max (residual, (double) std::abs (snapNow->outPitch[(size_t) f] - snapNow->pitch[(size_t) f]));
        const bool erased = residual < 1e-4;

        session.undo(); // undo the erase -> drawing is back
        pump();
        snapNow = session.snapshot();
        const bool undoBack = std::abs (snapNow->outPitch[(size_t) ((fa + fb) / 2)] - target) < 0.05f;
        session.undo(); // undo the stroke
        pump();
        check (drew && erased && undoBack, "Draw tool: stroke sets the pitch (max error " + juce::String (worst, 3)
                                               + " st), Option erases, each stroke is one undo");
        editor.keyPressed (juce::KeyPress ('1', {}, '1'));
    }

    // ---- 17. inspector knobs shape every selected note; one undo per drag ---------------------------
    {
        const auto a = notes0[0].id, b = notes0[1].id;
        editor.setSelection ({ a, b });
        pump();
        auto& k = page.getNoteKnob (ui::EditPage::CenterKnob);
        const bool enabled = k.isEnabled();
        k.onDragStart();
        for (double v : { 20.0, 45.0, 80.0 })
            k.setValue (v, juce::sendNotificationSync);
        k.onDragEnd();
        pump();
        const bool both = std::abs (noteById (a).center - 0.8f) < 1e-4f && std::abs (noteById (b).center - 0.8f) < 1e-4f;
        session.undo();
        pump();
        const bool reverted = noteById (a).center == 0.0f && noteById (b).center == 0.0f;

        auto& gainKnob = page.getNoteKnob (ui::EditPage::GainKnob);
        gainKnob.setValue (-6.0, juce::sendNotificationSync); // like typing a value
        pump();
        const bool gain = std::abs (noteById (a).gainDb + 6.0f) < 1e-4f && std::abs (noteById (b).gainDb + 6.0f) < 1e-4f;

        auto& glide = page.getNoteKnob (ui::EditPage::GlideInKnob);
        glide.setValue (150.0, juce::sendNotificationSync);
        pump();
        const bool glideOk = std::abs (noteById (a).transitionInMs - 150.0f) < 1e-3f && std::abs (noteById (a).transitionOutMs - 30.0f) < 1e-3f;

        editor.setSelection ({});
        pump();
        check (enabled && both && reverted && gain && glideOk && ! k.isEnabled(),
               "inspector knobs edit all selected notes, a drag is one undo, knobs disable with no selection");
    }

    // ---- 18. timing: drag sideways moves the note in time (axis-locked) -----------------------------
    session.edit ([] (edit::EditDocument& d) { d.resetAll(); });
    pump();
    editor.keyPressed (juce::KeyPress ('f', {}, 'f'));
    editor.setTool (ui::NoteEditor::Tool::Select);
    auto playOut = [&] () {
        std::vector<float> outAudio;
        head.playing = true;
        for (juce::int64 pos = 0; pos + 512 <= (juce::int64) timeline.size(); pos += 512)
        {
            for (int i = 0; i < 512; ++i) buf.setSample (0, i, timeline[(size_t) (pos + i)]);
            head.time = pos;
            proc.processBlock (buf, midi);
            for (int i = 0; i < 512; ++i) outAudio.push_back (buf.getSample (0, i));
        }
        runBlocks (1, false, 0, nullptr);
        return outAudio;
    };
    {
        const auto n = noteById (notes0[4].id);
        const auto r = editor.getNoteBounds (n.id);
        const float dx = (float) (-0.04 / editor.getSecondsPerPixel());
        const auto c = r.getCentre();
        mouse.drag (c, c.translated (dx, 3.0f)); // a little vertical wobble: still a time move
        pump();
        const auto m = noteById (n.id);
        const double moved = m.outStart - n.outStart;
        const bool timeOnly = std::abs (moved + 0.04) < 2.0 * editor.getSecondsPerPixel()
                              && std::abs ((m.outEnd - m.outStart) - (n.outEnd - n.outStart)) < 1e-9
                              && m.srcStart == n.srcStart && m.targetPitch == n.targetPitch;

        // and you hear it: the note's onset in the output is 40 ms earlier
        const auto heard = playOut();
        const auto tr = edit::analyzePitch (heard.data(), (int64_t) heard.size(), sr);
        double best = 1e9;
        for (const auto& d : edit::segmentNotes (tr))
            best = std::min (best, std::abs (tr.frameToSeconds (d.startFrame) - m.outStart));
        check (timeOnly && best < 0.03, "dragging sideways moves the note " + juce::String (moved * 1000.0, 1)
                                            + " ms (pitch untouched); playback onset error " + juce::String (best * 1000.0, 1) + " ms");
        session.undo();
        pump();
        check (noteById (n.id).outStart == n.outStart, "one undo puts it back");
    }

    // ---- 19. it stops at the previous note -------------------------------------------------------
    {
        const auto n = noteById (notes0[4].id), prev = noteById (notes0[3].id);
        const auto c = editor.getNoteBounds (n.id).getCentre();
        mouse.drag (c, c.translated (-400.0f, 0.0f));
        pump();
        const auto m = noteById (n.id);
        check (m.outStart >= prev.outEnd - 1e-9 && m.outStart < n.outStart, "a dragged note stops at its neighbour");
        session.undo();
        pump();
    }

    // ---- 20. grab an edge to stretch -----------------------------------------------------------------
    {
        const auto n = noteById (notes0[7].id);
        const auto r = editor.getNoteBounds (n.id);
        const float dx = (float) (0.15 / editor.getSecondsPerPixel());
        mouse.drag ({ r.getRight() - 2.0f, r.getCentreY() }, { r.getRight() - 2.0f + dx, r.getCentreY() });
        pump();
        const auto m = noteById (n.id);
        check (std::abs ((m.outEnd - n.outEnd) - 0.15) < 2.0 * editor.getSecondsPerPixel() && m.outStart == n.outStart,
               "dragging the right edge stretches the note by " + juce::String ((m.outEnd - n.outEnd) * 1000.0, 0) + " ms");
        session.undo();
        pump();
    }

    // ---- 21. a selection moves together ---------------------------------------------------------------
    {
        const auto a = noteById (notes0[4].id), b = noteById (notes0[5].id);
        editor.setSelection ({ a.id, b.id });
        const auto c = editor.getNoteBounds (b.id).getCentre();
        mouse.drag (c, c.translated ((float) (0.02 / editor.getSecondsPerPixel()), 0.0f));
        pump();
        const double da = noteById (a.id).outStart - a.outStart, db = noteById (b.id).outStart - b.outStart;
        check (std::abs (da - db) < 1e-9 && da > 0.015, "two selected notes move together (" + juce::String (da * 1000.0, 1) + " ms)");
        session.undo();
        pump();
    }

    // ---- 22. Shift-drag snaps the start to the 1/16 grid ------------------------------------------------
    {
        const auto n = noteById (notes0[4].id);
        editor.setSelection ({ n.id });
        const auto c = editor.getNoteBounds (n.id).getCentre();
        mouse.drag (c, c.translated ((float) (-0.03 / editor.getSecondsPerPixel()), 0.0f), juce::ModifierKeys::shiftModifier);
        pump();
        const auto m = noteById (n.id);
        const double sixteenth = 60.0 / 96.0 / 4.0;
        const double off = std::fmod (m.outStart, sixteenth);
        const double err = std::min (off, sixteenth - off);
        check (err < 0.001 && m.outStart != n.outStart,
               "Shift-drag lands the note on the 1/16 grid (" + juce::String (m.outStart, 4) + " s)");
        session.undo();
        pump();
    }

    // ---- 23. Option + arrow keys nudge in time ---------------------------------------------------------
    {
        const auto n = noteById (notes0[4].id);
        editor.setSelection ({ n.id });
        editor.keyPressed (juce::KeyPress (juce::KeyPress::rightKey, juce::ModifierKeys::altModifier, 0));
        pump();
        const double one = noteById (n.id).outStart - n.outStart;
        editor.keyPressed (juce::KeyPress (juce::KeyPress::leftKey, juce::ModifierKeys (juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier), 0));
        pump();
        const double two = noteById (n.id).outStart - n.outStart;
        check (std::abs (one - 0.01) < 1e-9 && std::abs (two + 0.04) < 1e-9 && editor.getSelection() == std::vector<uint32_t> { n.id },
               "Option+right nudges 10 ms, Option+Shift+left 50 ms");
        session.undo(); session.undo();
        pump();
    }

    // ---- 24. Reset puts timing back ----------------------------------------------------------------------
    {
        const auto n = noteById (notes0[4].id);
        editor.setSelection ({ n.id });
        editor.keyPressed (juce::KeyPress (juce::KeyPress::rightKey, juce::ModifierKeys::altModifier, 0));
        pump();
        editor.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey));
        pump();
        check (! noteById (n.id).hasTimingEdit(), "Delete (reset) restores the note's timing too");
    }

    // render a typical edit for the screenshot: everything corrected, two notes moved later in time,
    // one note stretched, the moved pair selected
    const auto allowed = proc.getAllowedNotes();
    const auto n7 = noteById (notes0[7].id);
    session.edit ([&] (edit::EditDocument& d) {
        d.correctAll (allowed, 0.5f, 1.0f);
        d.moveNotesTime ({ notes0[4].id, notes0[5].id }, 0.06);
        d.setNoteOutEdges (n7.id, n7.outStart, n7.outEnd + 0.12);
    });
    pump();
    editor.setSelection ({ notes0[4].id, notes0[5].id });
    pump (100);
    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
    juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile ("editor_test.png");
    out.deleteFile();
    {
        juce::FileOutputStream os (out);
        juce::PNGImageFormat().writeImageToStream (img, os);
    }

    ed.reset();
    proc.setPlayHead (nullptr);
    std::printf ("\n%s (%d failures)\n", failures == 0 ? "EDITOR TEST PASSED" : "EDITOR TEST FAILED", failures);
    return failures == 0 ? 0 : 1;
}
