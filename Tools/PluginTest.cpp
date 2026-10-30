// End-to-end test of the ClaudeTune processor in Edit mode, driven like a DAW:
// playhead, capture, Correct All, project save (FLAC) and reload, v1 projects.
// Development tool only.

#include "PluginProcessor.h"
#include "TestSignals.h"

#include <thread>

static int failures = 0;
static void check (bool ok, const juce::String& what)
{
    std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (! ok) ++failures;
}

struct TestPlayHead : juce::AudioPlayHead
{
    juce::int64 time = 0;
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (playing);
        p.setTimeInSamples (time);
        return p;
    }
};

static std::vector<float> play (ClaudeTuneProcessor& proc, TestPlayHead& ph, const std::vector<float>& timeline, int block = 512)
{
    std::vector<float> out (timeline.size());
    juce::AudioBuffer<float> buf (1, block);
    juce::MidiBuffer midi;
    ph.playing = true;
    for (size_t pos = 0; pos < timeline.size(); pos += (size_t) block)
    {
        const int n = (int) std::min<size_t> ((size_t) block, timeline.size() - pos);
        buf.setSize (1, n, false, false, true);
        for (int i = 0; i < n; ++i) buf.setSample (0, i, timeline[pos + (size_t) i]);
        ph.time = (juce::int64) pos;
        proc.processBlock (buf, midi);
        for (int i = 0; i < n; ++i) out[pos + (size_t) i] = buf.getSample (0, i);
    }
    ph.playing = false;
    buf.setSize (1, 64);
    buf.clear();
    proc.processBlock (buf, midi);
    return out;
}

static int notesOnGrid (const std::vector<float>& audio, double sr, const std::vector<claudetune::edit::Note>& notes)
{
    const auto tr = claudetune::edit::analyzePitch (audio.data(), (int64_t) audio.size(), sr);
    int good = 0;
    for (const auto& n : notes)
    {
        std::vector<float> v;
        const double len = n.outEnd - n.outStart;
        for (int f = (int) tr.secondsToFrame (n.outStart + 0.15 * len); f < (int) tr.secondsToFrame (n.outEnd - 0.15 * len); ++f)
            if (tr.voiced (f)) v.push_back (tr.frames[(size_t) f].midi);
        if (v.empty()) continue;
        std::sort (v.begin(), v.end());
        const double m = v[v.size() / 2];
        const int pc = ((int) std::lround (m)) % 12;
        const bool inC = pc == 0 || pc == 2 || pc == 4 || pc == 5 || pc == 7 || pc == 9 || pc == 11;
        if (inC && std::abs (m - std::round (m)) < 0.08) ++good;
    }
    return good;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    const double sr = 48000.0;
    const auto ph = testsig::makePhrase (sr);
    std::vector<float> timeline ((size_t) sr, 0.0f); // vocal starts at 1 s
    timeline.insert (timeline.end(), ph.audio.begin(), ph.audio.end());

    auto runMessages = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };

    juce::MemoryBlock savedState;
    std::vector<float> edited;
    size_t notesBefore = 0;

    {
        ClaudeTuneProcessor proc;
        TestPlayHead head;
        proc.setPlayHead (&head);
        proc.setPlayConfigDetails (1, 1, sr, 512);
        proc.prepareToPlay (sr, 512);
        const int liveLatency = proc.getLatencySamples();

        // set the key the way a user would on the Live tab: C major
        proc.apvts.getParameter (ParamIDs::scale)->setValueNotifyingHost (proc.apvts.getParameter (ParamIDs::scale)->convertTo0to1 (1));
        proc.setMode (ClaudeTuneProcessor::Mode::Edit);
        play (proc, head, std::vector<float> (512, 0.0f)); // one block to apply the mode
        runMessages (50);
        check (liveLatency > 0 && proc.getLatencySamples() == 0,
               "Edit mode reports zero latency (Live: " + juce::String (liveLatency) + " samples)");

        const auto pass1 = play (proc, head, timeline);
        proc.getSession().waitUntilIdle (20.0);
        bool same = true;
        for (size_t i = 0; i < timeline.size(); ++i) if (pass1[i] != timeline[i]) { same = false; break; }
        notesBefore = proc.getSession().snapshot()->notes.size();
        check (same && notesBefore == ph.notes.size(), "capture pass: audio untouched, " + juce::String ((int) notesBefore) + " notes");

        proc.correctAllNotes();
        proc.getSession().waitUntilIdle (10.0);
        edited = play (proc, head, timeline);
        const auto notes = proc.getSession().snapshot()->notes;
        const int good = notesOnGrid (edited, sr, notes);
        check (good == (int) notes.size(), "Correct All (C major from the Live tab): " + juce::String (good) + "/" + juce::String ((int) notes.size()) + " notes on pitch");

        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        proc.getStateInformation (savedState);
        const auto t1 = juce::Time::getMillisecondCounterHiRes();
        juce::MemoryBlock again;
        proc.getStateInformation (again);
        const auto t2 = juce::Time::getMillisecondCounterHiRes();
        const double rawBytes = (double) timeline.size() * 4.0;
        check (savedState.getSize() > 1000 && savedState == again,
               "project state: " + juce::String (savedState.getSize() / 1024) + " KB (raw audio "
                   + juce::String ((int) (rawBytes / 1024)) + " KB), first save " + juce::String (t1 - t0, 0)
                   + " ms, cached save " + juce::String (t2 - t1, 0) + " ms");
        proc.setPlayHead (nullptr);
    }

    // ---- reopen the project in a new instance ----------------------------------
    {
        ClaudeTuneProcessor proc;
        TestPlayHead head;
        proc.setPlayHead (&head);
        proc.setStateInformation (savedState.getData(), (int) savedState.getSize()); // hosts restore before prepare
        proc.setPlayConfigDetails (1, 1, sr, 512);
        proc.prepareToPlay (sr, 512);
        const bool idle = proc.getSession().waitUntilIdle (20.0);
        check (idle && proc.getMode() == ClaudeTuneProcessor::Mode::Edit && proc.getLatencySamples() == 0,
               "project reopens in Edit mode");

        const auto replay = play (proc, head, timeline);
        double maxErr = 0.0;
        for (size_t i = 0; i < replay.size(); ++i) maxErr = std::max (maxErr, (double) std::abs (replay[i] - edited[i]));
        const auto snap = proc.getSession().snapshot();
        check (snap->notes.size() == notesBefore && maxErr < 1e-4,
               "reopened project plays the same edits (max diff " + juce::String (maxErr, 7) + ", 24-bit FLAC)");
        proc.getSession().waitUntilIdle (5.0);
        juce::MemoryBlock resaved;
        proc.getStateInformation (resaved);
        check (std::abs ((double) resaved.getSize() - (double) savedState.getSize()) < 0.02 * (double) savedState.getSize(),
               "re-saving a reopened project keeps it intact (" + juce::String (resaved.getSize() / 1024) + " KB)");
        proc.setPlayHead (nullptr);
    }

    // ---- a ClaudeTune 1.0 project still loads ------------------------------------
    {
        ClaudeTuneProcessor v1;
        v1.apvts.getParameter (ParamIDs::speed)->setValueNotifyingHost (v1.apvts.getParameter (ParamIDs::speed)->convertTo0to1 (42.0f));
        auto state = v1.apvts.copyState();
        juce::MemoryBlock v1State;
        if (auto xml = state.createXml())
            juce::AudioProcessor::copyXmlToBinary (*xml, v1State); // exactly what 1.0 wrote (plus grid)

        ClaudeTuneProcessor proc;
        proc.setStateInformation (v1State.getData(), (int) v1State.getSize());
        const float speed = proc.apvts.getRawParameterValue (ParamIDs::speed)->load();
        check (std::abs (speed - 42.0f) < 0.2f && proc.getMode() == ClaudeTuneProcessor::Mode::Live,
               "v1 project loads (speed " + juce::String (speed, 1) + " ms, Live mode)");
    }

    std::printf ("\n%s (%d failures)\n", failures == 0 ? "PLUGIN TEST PASSED" : "PLUGIN TEST FAILED", failures);
    return failures == 0 ? 0 : 1;
}
