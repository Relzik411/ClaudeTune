#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace claudetune;

namespace
{
    // Logarithmic 0.1 - 800 ms range with 0.1 ms steps
    juce::NormalisableRange<float> logMsRange (float lo, float hi)
    {
        return { lo, hi,
                 [] (float s, float e, float t) { return s * std::pow (e / s, t); },
                 [] (float s, float e, float v) { return std::log (v / s) / std::log (e / s); },
                 [] (float s, float e, float v) { return juce::jlimit (s, e, std::round (v * 10.0f) / 10.0f); } };
    }

    juce::String msText (float v, int)
    {
        return v < 10.0f ? juce::String (v, 1) + " ms" : juce::String (juce::roundToInt (v)) + " ms";
    }

    float numberFromText (const juce::String& t)
    {
        return t.retainCharacters ("0123456789.-").getFloatValue();
    }

    juce::StringArray rootNames()
    {
        juce::StringArray s;
        for (int i = 0; i < 12; ++i)
            s.add (noteName (i, false));
        return s;
    }

    juce::StringArray scaleNames()
    {
        juce::StringArray s;
        for (int i = 0; i < kNumScales; ++i)
            s.add (kScales[i].name);
        return s;
    }

    juce::String midiNoteText (int note)
    {
        return juce::String (noteName (note, false)) + juce::String (note / 12 - 1);
    }
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout ClaudeTuneProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    auto msAttr = AudioParameterFloatAttributes()
                      .withStringFromValueFunction (msText)
                      .withValueFromStringFunction (numberFromText);

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::speed, 1 }, "Speed",
                                                        logMsRange (0.1f, 800.0f), 15.0f, msAttr));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::transition, 1 }, "Note Transition",
                                                        logMsRange (0.1f, 800.0f), 120.0f, msAttr));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::link, 1 }, "Link", false));

    p.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::tolCents, 1 }, "Tolerance Cents", NormalisableRange<float> (0.0f, 40.0f, 1.0f), 0.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int) { return v < 0.5f ? String ("Off") : String (roundToInt (v)) + " c"; })
            .withValueFromStringFunction ([] (const String& t) { return t.startsWithIgnoreCase ("off") ? 0.0f : numberFromText (t); })));

    p.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::tolTime, 1 }, "Tolerance Time", NormalisableRange<float> (0.0f, 300.0f, 1.0f), 0.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int) { return v < 0.5f ? String ("Off") : String (roundToInt (v)) + " ms"; })
            .withValueFromStringFunction ([] (const String& t) { return t.startsWithIgnoreCase ("off") ? 0.0f : numberFromText (t); })));

    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::vibratoOn, 1 }, "Vibrato", false));
    p.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::vibratoDepth, 1 }, "Vibrato Depth", NormalisableRange<float> (0.0f, 200.0f, 1.0f), 100.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + "%"; })
            .withValueFromStringFunction (numberFromText)));

    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::correctionOn, 1 }, "Correction", true));
    p.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::correction, 1 }, "Correction Amount", NormalisableRange<float> (0.1f, 100.0f, 0.1f), 100.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int) { return v >= 10.0f ? String (roundToInt (v)) + "%" : String (v, 1) + "%"; })
            .withValueFromStringFunction (numberFromText)));

    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::formant, 1 }, "Formant Correction", true));

    p.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::refHz, 1 }, "Reference", NormalisableRange<float> (415.30f, 466.16f, 0.01f), 440.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int) { return String (v, 2) + " Hz"; })
            .withValueFromStringFunction (numberFromText)));

    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::root, 1 }, "Key", rootNames(), 0));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::scale, 1 }, "Scale", scaleNames(), 0));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::useFlats, 1 }, "Show Flats", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::groupOctaves, 1 }, "Group Octaves", true));

    auto noteAttr = AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return midiNoteText (v); });
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { ParamIDs::rangeLow, 1 }, "Range Low",
                                                      kKeyboardLow, kKeyboardHigh - 1, 36, noteAttr));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { ParamIDs::rangeHigh, 1 }, "Range High",
                                                      kKeyboardLow + 1, kKeyboardHigh, 84, noteAttr));

    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::refTone, 1 }, "Reference Tone", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::targetPitch, 1 }, "Target Pitch", false));
    p.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::refToneLevel, 1 }, "Reference Tone Level", NormalisableRange<float> (-18.0f, 18.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int) { return (v > 0.04f ? "+" : "") + String (v, 1) + " dB"; })
            .withValueFromStringFunction (numberFromText)));

    return { p.begin(), p.end() };
}

//==============================================================================
ClaudeTuneProcessor::ClaudeTuneProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "ClaudeTune", createLayout())
{
    raw.speed        = apvts.getRawParameterValue (ParamIDs::speed);
    raw.transition   = apvts.getRawParameterValue (ParamIDs::transition);
    raw.tolCents     = apvts.getRawParameterValue (ParamIDs::tolCents);
    raw.tolTime      = apvts.getRawParameterValue (ParamIDs::tolTime);
    raw.vibratoOn    = apvts.getRawParameterValue (ParamIDs::vibratoOn);
    raw.vibratoDepth = apvts.getRawParameterValue (ParamIDs::vibratoDepth);
    raw.correctionOn = apvts.getRawParameterValue (ParamIDs::correctionOn);
    raw.correction   = apvts.getRawParameterValue (ParamIDs::correction);
    raw.formant      = apvts.getRawParameterValue (ParamIDs::formant);
    raw.refHz        = apvts.getRawParameterValue (ParamIDs::refHz);
    raw.rangeLow     = apvts.getRawParameterValue (ParamIDs::rangeLow);
    raw.rangeHigh    = apvts.getRawParameterValue (ParamIDs::rangeHigh);
    raw.refTone      = apvts.getRawParameterValue (ParamIDs::refTone);
    raw.targetPitch  = apvts.getRawParameterValue (ParamIDs::targetPitch);
    raw.refToneLevel = apvts.getRawParameterValue (ParamIDs::refToneLevel);

    rebuildScale();

    apvts.addParameterListener (ParamIDs::root, this);
    apvts.addParameterListener (ParamIDs::scale, this);
}

ClaudeTuneProcessor::~ClaudeTuneProcessor()
{
    cancelPendingUpdate();
    apvts.removeParameterListener (ParamIDs::root, this);
    apvts.removeParameterListener (ParamIDs::scale, this);
}

//==============================================================================
bool ClaudeTuneProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void ClaudeTuneProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const int channels = juce::jmax (1, getTotalNumOutputChannels());
    engine.prepare (sampleRate, samplesPerBlock, channels);
    engine.setParams (readParams());
    lastLatency = engine.getLatencySamples();
    setLatencySamples (lastLatency);
}

EngineParams ClaudeTuneProcessor::readParams() const
{
    EngineParams p;
    p.speedMs          = raw.speed->load();
    p.transitionMs     = raw.transition->load();
    p.toleranceCents   = raw.tolCents->load();
    p.toleranceTimeMs  = raw.tolTime->load();
    p.vibratoOn        = raw.vibratoOn->load() > 0.5f;
    p.vibratoDepth     = raw.vibratoDepth->load() / 100.0f;
    p.correctionOn     = raw.correctionOn->load() > 0.5f;
    p.correctionAmount = raw.correction->load() / 100.0f;
    p.formantCorrected = raw.formant->load() > 0.5f;
    p.referenceHz      = raw.refHz->load();
    p.rangeLow         = (int) raw.rangeLow->load();
    p.rangeHigh        = juce::jmax (p.rangeLow + 1, (int) raw.rangeHigh->load());
    p.referenceToneOn  = raw.refTone->load() > 0.5f;
    p.targetPitchOn    = raw.targetPitch->load() > 0.5f;
    p.referenceToneDb  = raw.refToneLevel->load();
    return p;
}

void ClaudeTuneProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (m.isNoteOn())
            engine.noteOn (m.getNoteNumber());
        else if (m.isNoteOff())
            engine.noteOff (m.getNoteNumber());
        else if (m.isAllNotesOff() || m.isAllSoundOff())
            engine.allNotesOff();
    }

    NoteStateArray states;
    for (size_t i = 0; i < 128; ++i)
        states[i] = (NoteState) noteStates[i].load (std::memory_order_relaxed);
    engine.setNoteStates (states);
    engine.setParams (readParams());

    if (engine.getLatencySamples() != lastLatency)
    {
        lastLatency = engine.getLatencySamples();
        triggerAsyncUpdate(); // tell the host on the message thread
    }

    const int channels = juce::jmin (buffer.getNumChannels(), getTotalNumOutputChannels());
    for (int c = getTotalNumInputChannels(); c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());

    engine.process (buffer.getArrayOfWritePointers(), channels, buffer.getNumSamples());
}

void ClaudeTuneProcessor::handleAsyncUpdate()
{
    setLatencySamples (engine.telemetry.latency.load());
}

//==============================================================================
NoteState ClaudeTuneProcessor::getNoteState (int note) const noexcept
{
    return (NoteState) noteStates[(size_t) juce::jlimit (0, 127, note)].load();
}

void ClaudeTuneProcessor::cycleNoteState (int note, bool backwards)
{
    note = juce::jlimit (0, 127, note);
    auto s = getNoteState (note);
    NoteState next = s;
    if (backwards)
        next = (NoteState) (((int) s + kNumNoteStates - 1) % kNumNoteStates);
    else
        next = nextNoteState (s);

    const bool group = apvts.getRawParameterValue (ParamIDs::groupOctaves)->load() > 0.5f;
    for (int n = 0; n < 128; ++n)
        if (n == note || (group && n % 12 == note % 12))
            noteStates[(size_t) n].store ((uint8_t) next);

    scaleModified = true;
    ++noteStateVersion;
}

void ClaudeTuneProcessor::rebuildScale()
{
    const int root = (int) apvts.getRawParameterValue (ParamIDs::root)->load();
    const int scale = (int) apvts.getRawParameterValue (ParamIDs::scale)->load();
    const auto states = buildScale (scale, root);
    for (size_t i = 0; i < 128; ++i)
        noteStates[i].store ((uint8_t) states[i]);
    scaleModified = false;
    ++noteStateVersion;
}

void ClaudeTuneProcessor::resetScale() { rebuildScale(); }

void ClaudeTuneProcessor::parameterChanged (const juce::String&, float)
{
    rebuildScale();
}

//==============================================================================
void ClaudeTuneProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    juce::String grid;
    for (size_t i = 0; i < 128; ++i)
        grid << juce::String ((int) noteStates[i].load());
    state.setProperty ("noteGrid", grid, nullptr);
    state.setProperty ("scaleModified", scaleModified.load(), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void ClaudeTuneProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto state = juce::ValueTree::fromXml (*xml);
    apvts.replaceState (state); // may rebuild the scale via the listener...

    // ...so restore the custom note grid afterwards
    const auto grid = state.getProperty ("noteGrid").toString();
    if (grid.length() == 128)
    {
        for (int i = 0; i < 128; ++i)
        {
            const int v = grid[i] - '0';
            if (v >= 0 && v < kNumNoteStates)
                noteStates[(size_t) i].store ((uint8_t) v);
        }
        scaleModified = (bool) state.getProperty ("scaleModified", false);
        ++noteStateVersion;
    }
}

//==============================================================================
juce::AudioProcessorEditor* ClaudeTuneProcessor::createEditor()
{
    return new ClaudeTuneEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ClaudeTuneProcessor();
}
