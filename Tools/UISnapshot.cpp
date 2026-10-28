// Renders the ClaudeTune editor to a PNG while feeding the processor a
// slightly sharp synthetic voice, so the meters and tuner show live values.
// Development tool only (Linux CI / headless checks).

#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <thread>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File out (argc > 1 ? juce::String (argv[1]) : juce::String ("claudetune_ui.png"));

    ClaudeTuneProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    // a little customisation so the grid shows every note state
    proc.apvts.getParameter (ParamIDs::scale)->setValueNotifyingHost (proc.apvts.getParameter (ParamIDs::scale)->convertTo0to1 (1)); // Major
    proc.cycleNoteState (66, false); // F# -> illegal up (from illegal)
    proc.cycleNoteState (70, false);
    proc.cycleNoteState (70, false); // A# -> illegal down
    for (int i = 0; i < 4; ++i) proc.cycleNoteState (64, false); // E -> bypass
    proc.apvts.getParameter (ParamIDs::vibratoOn)->setValueNotifyingHost (1.0f);

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setVisible (true);

    std::atomic<bool> running { true };
    std::thread audio ([&] {
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        double phase[40] = {};
        const double sr = 48000.0;
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
                buf.setSample (1, i, (float) (0.25 * v));
            }
            proc.processBlock (buf, midi);
            std::this_thread::sleep_for (std::chrono::microseconds (2000));
        }
    });

    juce::MessageManager::getInstance()->runDispatchLoopUntil (1500);
    running = false;
    audio.join();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (40);

    auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
    out.deleteFile();
    juce::FileOutputStream os (out);
    juce::PNGImageFormat png;
    const bool ok = png.writeImageToStream (img, os);
    std::printf ("%s %s (%dx%d)\n", ok ? "wrote" : "FAILED", out.getFullPathName().toRawUTF8(), img.getWidth(), img.getHeight());

    editor.reset();
    return ok ? 0 : 1;
}
