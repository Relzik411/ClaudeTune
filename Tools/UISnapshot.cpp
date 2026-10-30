// Renders the ClaudeTune editor to a PNG. Development tool only.
//   ClaudeTuneSnapshot out.png            -> Live tab with a slightly sharp test voice
//   ClaudeTuneSnapshot out.png edit       -> Edit tab after capturing a phrase + Correct All
//   ClaudeTuneSnapshot out.png edit-empty -> Edit tab before anything is captured

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "TestSignals.h"

#include <thread>

struct SnapshotPlayHead : juce::AudioPlayHead
{
    std::atomic<juce::int64> time { 0 };
    std::atomic<bool> playing { false };
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (playing.load());
        p.setTimeInSamples (time.load());
        return p;
    }
};

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File out (argc > 1 ? juce::String (argv[1]) : juce::String ("claudetune_ui.png"));
    const juce::String page (argc > 2 ? argv[2] : "live");
    const double sr = 48000.0;

    ClaudeTuneProcessor proc;
    SnapshotPlayHead head;
    proc.setPlayHead (&head);
    proc.setPlayConfigDetails (1, 1, sr, 512);
    proc.prepareToPlay (sr, 512);

    // a little customisation so the grid shows every note state
    proc.apvts.getParameter (ParamIDs::scale)->setValueNotifyingHost (proc.apvts.getParameter (ParamIDs::scale)->convertTo0to1 (1)); // Major
    if (page == "live")
    {
        proc.cycleNoteState (66, false); // F# -> illegal up (from illegal)
        proc.cycleNoteState (70, false);
        proc.cycleNoteState (70, false); // A# -> illegal down
        for (int i = 0; i < 4; ++i) proc.cycleNoteState (64, false); // E -> bypass
        proc.apvts.getParameter (ParamIDs::vibratoOn)->setValueNotifyingHost (1.0f);
    }

    juce::MidiBuffer midi;
    if (page.startsWith ("edit"))
    {
        proc.setMode (ClaudeTuneProcessor::Mode::Edit);
        if (page == "edit")
        {
            // play a phrase that starts 1 s into the song, then correct it
            const auto ph = testsig::makePhrase (sr);
            std::vector<float> timeline ((size_t) sr, 0.0f);
            timeline.insert (timeline.end(), ph.audio.begin(), ph.audio.end());
            juce::AudioBuffer<float> buf (1, 512);
            head.playing = true;
            for (size_t pos = 0; pos + 512 <= timeline.size(); pos += 512)
            {
                for (int i = 0; i < 512; ++i) buf.setSample (0, i, timeline[pos + (size_t) i]);
                head.time = (juce::int64) pos;
                proc.processBlock (buf, midi);
            }
            head.playing = false;
            buf.clear();
            proc.processBlock (buf, midi);
            proc.getSession().waitUntilIdle (20.0);
            proc.correctAllNotes();
            proc.getSession().waitUntilIdle (10.0);
            head.time = (juce::int64) (3.3 * sr); // park the playhead mid-phrase
            proc.processBlock (buf, midi);
        }
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setVisible (true);

    std::atomic<bool> running { page == "live" };
    std::thread audio ([&] {
        juce::AudioBuffer<float> buf (1, 512);
        double phase[40] = {};
        for (int block = 0; running; ++block)
        {
            for (int i = 0; i < 512; ++i)
            {
                const double t = (block * 512 + i) / sr;
                const double f0 = 440.0 * std::pow (2.0, (0.34 + 0.08 * std::sin (2 * 3.14159 * 5 * t)) / 12.0);
                double v = 0;
                for (int h = 1; h < 30; ++h)
                {
                    phase[h] += 2 * 3.14159265 * f0 * h / sr;
                    v += std::sin (phase[h]) / h;
                }
                buf.setSample (0, i, (float) (0.25 * v));
            }
            juce::MidiBuffer m;
            proc.processBlock (buf, m);
            std::this_thread::sleep_for (std::chrono::microseconds (2000));
        }
    });

    juce::MessageManager::getInstance()->runDispatchLoopUntil (1500);
    running = false;
    audio.join();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (60);

    auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
    out.deleteFile();
    juce::FileOutputStream os (out);
    juce::PNGImageFormat png;
    const bool ok = png.writeImageToStream (img, os);
    std::printf ("%s %s (%dx%d)\n", ok ? "wrote" : "FAILED", out.getFullPathName().toRawUTF8(), img.getWidth(), img.getHeight());

    editor.reset();
    proc.setPlayHead (nullptr);
    return ok ? 0 : 1;
}
