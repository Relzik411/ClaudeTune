// Loads the built ClaudeTune.vst3 through JUCE's VST3 host, then:
//  - processes audio at several sample rates / block sizes (incl. odd sizes)
//  - checks output is finite and that a sharp tone gets corrected
//  - automates every parameter while processing
//  - saves and restores state (including the note grid)
//  - opens and closes the editor
// Development tool only.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>

static int failures = 0;
static void check (bool ok, const juce::String& what)
{
    std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (! ok) ++failures;
}

static double measureMidi (const juce::AudioBuffer<float>& b, int start, int len, double sr)
{
    const float* x = b.getReadPointer (0) + start;
    const int lo = (int) (sr / 1000), hi = (int) (sr / 80);
    double best = -1e9; int bestLag = lo;
    std::vector<double> ac ((size_t) hi + 2, 0.0);
    for (int lag = lo; lag <= hi + 1; ++lag)
    {
        double s = 0, e1 = 0, e2 = 0;
        for (int i = 0; i + lag < len; ++i) { s += x[i] * x[i + lag]; e1 += x[i] * x[i]; e2 += x[i + lag] * x[i + lag]; }
        ac[(size_t) lag] = s / std::sqrt (e1 * e2 + 1e-12);
    }
    for (int lag = lo; lag <= hi; ++lag)
        if (ac[(size_t) lag] > best) { best = ac[(size_t) lag]; bestLag = lag; }
    // prefer the first strong peak (avoid octave-down errors)
    for (int lag = lo; lag <= hi; ++lag)
        if (ac[(size_t) lag] > 0.9 * best && ac[(size_t) lag] >= ac[(size_t) lag - 1] && ac[(size_t) lag] >= ac[(size_t) lag + 1]) { bestLag = lag; break; }
    const double a = ac[(size_t) bestLag - 1], b0 = ac[(size_t) bestLag], c = ac[(size_t) bestLag + 1];
    const double lag = bestLag + 0.5 * (a - c) / (a - 2 * b0 + c);
    return 69.0 + 12.0 * std::log2 (sr / lag / 440.0);
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc < 2) { std::printf ("usage: HostTest path/to/ClaudeTune.vst3\n"); return 2; }

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descs;
    format.findAllTypesForFile (descs, argv[1]);
    check (descs.size() == 1, "found plug-in description");
    if (descs.isEmpty()) return 1;

    const auto& d = *descs[0];
    std::printf ("  name=%s manufacturer=%s category=%s acceptsMidi=%d\n", d.name.toRawUTF8(),
                 d.manufacturerName.toRawUTF8(), d.category.toRawUTF8(), (int) d.isInstrument);

    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        juce::String err;
        auto inst = format.createInstanceFromDescription (d, sr, 1024, err);
        check (inst != nullptr, "instantiate @" + juce::String (sr) + " " + err);
        if (! inst) continue;

        inst->enableAllBuses();
        inst->setPlayConfigDetails (2, 2, sr, 1024);
        inst->prepareToPlay (sr, 1024);
        std::printf ("  latency @%g = %d samples\n", sr, inst->getLatencySamples());
        check (inst->getLatencySamples() > 0, "reports latency");

        // 2.5 s of a sharp harmonic tone (A4 + 35 cents) in odd block sizes
        const int total = (int) (sr * 2.5);
        juce::AudioBuffer<float> all (2, total);
        double ph[30] = {};
        for (int i = 0; i < total; ++i)
        {
            double v = 0;
            for (int h = 1; h < 30; ++h) { ph[h] += 2 * juce::MathConstants<double>::pi * 440.0 * std::pow (2.0, 0.35 / 12.0) * h / sr; v += std::sin (ph[h]) / h; }
            all.setSample (0, i, (float) (0.2 * v));
            all.setSample (1, i, (float) (0.2 * v));
        }

        juce::MidiBuffer midi;
        const int sizes[] = { 512, 1, 37, 256, 1024, 64, 500 };
        bool finite = true;
        for (int pos = 0, k = 0; pos < total; ++k)
        {
            const int n = juce::jmin (sizes[k % 7], total - pos);
            juce::AudioBuffer<float> blk (all.getArrayOfWritePointers(), 2, pos, n);
            inst->processBlock (blk, midi);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i)
                    if (! std::isfinite (blk.getSample (c, i))) finite = false;
            pos += n;
        }
        check (finite, "finite output with odd block sizes @" + juce::String (sr));
        const double m = measureMidi (all, (int) (sr * 1.5), (int) (sr * 0.8), sr);
        check (std::abs (m - 69.0) < 0.06, "A4+35c corrected to " + juce::String ((m - 69.0) * 100.0, 1) + " c @" + juce::String (sr));

        // parameter fuzz while processing
        juce::Random rng (42);
        auto params = inst->getParameters();
        juce::AudioBuffer<float> buf (2, 256);
        for (int it = 0; it < 400; ++it)
        {
            for (auto* p : params)
                if (rng.nextInt (6) == 0)
                    p->setValue (rng.nextFloat());
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 256; ++i)
                    buf.setSample (c, i, rng.nextFloat() * 0.6f - 0.3f);
            if (it % 50 == 0) { midi.clear(); midi.addEvent (juce::MidiMessage::noteOn (1, 60 + it % 12, 0.8f), 0); }
            inst->processBlock (buf, midi);
            midi.clear();
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 256; ++i)
                    if (! std::isfinite (buf.getSample (c, i))) finite = false;
        }
        check (finite, "parameter fuzz keeps output finite @" + juce::String (sr) + " (" + juce::String (params.size()) + " params)");

        // state round trip: A -> state -> fresh instance B -> state must match
        juce::MemoryBlock stateA, stateB;
        inst->getStateInformation (stateA);
        {
            juce::String err2;
            auto other = format.createInstanceFromDescription (d, sr, 1024, err2);
            other->setStateInformation (stateA.getData(), (int) stateA.getSize());
            other->getStateInformation (stateB);
        }
        const bool same = stateA == stateB;
        const bool hasGrid = stateA.toString().contains ("noteGrid") || juce::String::fromUTF8 ((const char*) stateA.getData(), (int) stateA.getSize()).contains ("noteGrid");
        if (! same)
            std::printf ("  A=%d bytes B=%d bytes\n", (int) stateA.getSize(), (int) stateB.getSize());
        check (same, "state round trip into a fresh instance (" + juce::String ((int) stateA.getSize()) + " bytes)");
        std::printf ("  state mentions note grid: %s\n", hasGrid ? "yes" : "no (encoded)");

        if (sr == 48000.0)
        {
            std::unique_ptr<juce::AudioProcessorEditor> ed (inst->createEditorIfNeeded());
            check (ed != nullptr && ed->getWidth() > 900, "editor opens (" + (ed ? juce::String (ed->getWidth()) + "x" + juce::String (ed->getHeight()) : juce::String ("null")) + ")");
            if (ed) inst->editorBeingDeleted (ed.get());
        }

        inst->releaseResources();
    }

    // ---- Edit mode through the VST3 interface -----------------------------------
    {
        struct HostPlayHead : juce::AudioPlayHead
        {
            juce::int64 time = 0;
            bool playing = false;
            juce::Optional<PositionInfo> getPosition() const override
            {
                PositionInfo p;
                p.setIsPlaying (playing);
                p.setTimeInSamples (time);
                return p;
            }
        };

        const double sr = 48000.0;
        const int block = 512;
        const int total = (int) (sr * 3.0);
        juce::AudioBuffer<float> vocal (2, total);
        double phase = 0.0;
        for (int i = 0; i < total; ++i)
        {
            const double t = i / sr;
            const double f0 = 220.0 * std::pow (2.0, (t < 1.5 ? 0.3 : 3.2) / 12.0);
            phase += 2 * juce::MathConstants<double>::pi * f0 / sr;
            double v = 0;
            for (int h = 1; h < 20; ++h) v += std::sin (h * phase) / h;
            const double env = (t > 0.3 && t < 2.8) ? 0.25 : 0.0;
            vocal.setSample (0, i, (float) (env * v));
            vocal.setSample (1, i, (float) (env * v));
        }

        auto playThrough = [&] (juce::AudioPluginInstance& p, HostPlayHead& head) {
            juce::AudioBuffer<float> out (vocal);
            juce::MidiBuffer midi;
            head.playing = true;
            for (int pos = 0; pos < total; pos += block)
            {
                const int n = juce::jmin (block, total - pos);
                juce::AudioBuffer<float> blk (out.getArrayOfWritePointers(), 2, pos, n);
                head.time = pos;
                p.processBlock (blk, midi);
            }
            head.playing = false;
            return out;
        };
        auto setMode = [] (juce::AudioPluginInstance& p, float v) {
            for (auto* prm : p.getParameters())
                if (prm->getName (32) == "Mode") { prm->setValueNotifyingHost (v); return true; }
            return false;
        };
        auto pump = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };

        juce::String err;
        auto inst = format.createInstanceFromDescription (d, sr, block, err);
        HostPlayHead head;
        inst->setPlayHead (&head);
        inst->enableAllBuses();
        inst->setPlayConfigDetails (2, 2, sr, block);
        inst->prepareToPlay (sr, block);
        const bool found = setMode (*inst, 1.0f);
        { juce::AudioBuffer<float> b (2, block); b.clear(); juce::MidiBuffer m; inst->processBlock (b, m); }
        pump (200);
        check (found && inst->getLatencySamples() == 0, "Edit mode via VST3: latency " + juce::String (inst->getLatencySamples()));

        const auto first = playThrough (*inst, head);
        float diff = 0.0f;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < total; ++i)
                diff = std::max (diff, std::abs (first.getSample (c, i) - vocal.getSample (c, i)));
        pump (1500); // background analysis
        juce::MemoryBlock state;
        inst->getStateInformation (state);
        check (diff == 0.0f && state.getSize() > 50000,
               "VST3 capture pass is untouched and saved with the project (" + juce::String ((int) state.getSize() / 1024) + " KB)");

        auto other = format.createInstanceFromDescription (d, sr, block, err);
        HostPlayHead head2;
        other->setPlayHead (&head2);
        other->setStateInformation (state.getData(), (int) state.getSize());
        other->enableAllBuses();
        other->setPlayConfigDetails (2, 2, sr, block);
        other->prepareToPlay (sr, block);
        pump (1500);
        juce::MemoryBlock state2;
        other->getStateInformation (state2);
        const auto replay = playThrough (*other, head2);
        float diff2 = 0.0f;
        for (int i = 0; i < total; ++i)
            diff2 = std::max (diff2, std::abs (replay.getSample (0, i) - vocal.getSample (0, i)));
        check (state2 == state && diff2 == 0.0f, "VST3 project reload restores the capture exactly");

        other->releaseResources();
        inst->releaseResources();
        other->setPlayHead (nullptr);
        inst->setPlayHead (nullptr);
    }

    std::printf ("\n%s (%d failures)\n", failures == 0 ? "HOST TEST PASSED" : "HOST TEST FAILED", failures);
    return failures == 0 ? 0 : 1;
}
