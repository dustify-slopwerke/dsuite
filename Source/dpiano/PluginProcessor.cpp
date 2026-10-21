#include "PluginProcessor.h"
#include "PluginEditor.h"

#include "BinaryData.h"
#include "dsp/Preset.h"

#include "common/plugin/ParamLayout.h"

using dustify::toJuce;

DpianoAudioProcessor::DpianoAudioProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", dustify::createParameterLayout (dpiano::allParams()))
{
    for (const auto& info : dpiano::allParams())
        rawParams[(size_t) info.pid] = apvts.getRawParameterValue (toJuce (info.id));

    factoryPresets = dustify::loadFactoryPresets (BinaryData::namedResourceListSize, BinaryData::namedResourceList,
                                                  BinaryData::originalFilenames, BinaryData::getNamedResource,
                                                  [] (const std::string& text)
                                                  {
                                                      const auto parsed = dpiano::parsePreset (text);
                                                      jassert (parsed.warnings.empty());
                                                      return std::pair { parsed.name, parsed.category };
                                                  });

    setVelocityMap (loadMachineVelocityMap(), false);
}

void DpianoAudioProcessor::setCurrentProgram (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) factoryPresets.size()))
        return;
    currentProgram = index;
    loadPresetText (factoryPresets[(size_t) index].text);
}

const juce::String DpianoAudioProcessor::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, (int) factoryPresets.size()) ? factoryPresets[(size_t) index].name
                                                                         : juce::String ("Init");
}

void DpianoAudioProcessor::loadPresetText (const std::string& text)
{
    const auto parsed = dpiano::parsePreset (text);
    setCurrentPresetName (juce::String (parsed.name));
    for (const auto& info : dpiano::allParams())
        if (auto* param = apvts.getParameter (toJuce (info.id)))
            param->setValueNotifyingHost (param->convertTo0to1 (parsed.patch[info.pid]));
    // Every value may have changed at once: hosts re-read them all
    updateHostDisplay (ChangeDetails().withProgramChanged (true));
}

std::string DpianoAudioProcessor::savePresetText (const juce::String& name) const
{
    return dpiano::serialisePreset (currentPatch(), name.toStdString());
}

juce::File DpianoAudioProcessor::userPresetDirectory() const
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("dpiano/presets");
}

juce::File DpianoAudioProcessor::settingsFile()
{
    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("DUSTIFY_SETTINGS_DIR", {}); dir.isNotEmpty())
        return juce::File (dir).getChildFile ("dpiano.settings");
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("dpiano/dpiano.settings");
}

namespace
{
juce::PropertiesFile::Options settingsOptions()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "dpiano";
    options.filenameSuffix = "settings";
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    options.millisecondsBeforeSaving = -1; // saved explicitly
    return options;
}

// Velocities are written with 4 decimals (MIDI steps are 1/127), so saving is reproducible
juce::String velocityText (float v) { return juce::String (v, 4); }

constexpr auto keyboardTag = "KEYBOARD";
} // namespace

dpiano::VelocityMap DpianoAudioProcessor::loadMachineVelocityMap()
{
    const auto file = settingsFile();
    if (! file.existsAsFile())
        return {};
    juce::PropertiesFile settings (file, settingsOptions());
    dpiano::VelocityMap map;
    map.inMin = (float) settings.getDoubleValue ("velocityMin", 0.0);
    map.inMax = (float) settings.getDoubleValue ("velocityMax", 1.0);
    map.sensitivity = (float) settings.getDoubleValue ("velocitySensitivity", 0.0);
    return map.sanitised();
}

dpiano::VelocityMap DpianoAudioProcessor::getVelocityMap() const
{
    dpiano::VelocityMap map;
    map.inMin = velocityMin.load();
    map.inMax = velocityMax.load();
    map.sensitivity = velocitySensitivity.load();
    return map;
}

void DpianoAudioProcessor::setVelocityMap (const dpiano::VelocityMap& newMap, bool saveMachineWide)
{
    const auto map = newMap.sanitised();
    velocityMin = map.inMin;
    velocityMax = map.inMax;
    velocitySensitivity = map.sensitivity;
    if (! saveMachineWide)
        return;

    const auto file = settingsFile();
    file.getParentDirectory().createDirectory();
    juce::PropertiesFile settings (file, settingsOptions());
    settings.setValue ("velocityMin", velocityText (map.inMin));
    settings.setValue ("velocityMax", velocityText (map.inMax));
    settings.setValue ("velocitySensitivity", velocityText (map.sensitivity));
    if (! settings.saveIfNeeded())
    {
        DBG ("dpiano: couldn't write " << file.getFullPathName());
    }
}

bool DpianoAudioProcessor::popPlayedVelocity (float& velocity)
{
    const auto scope = velocityFifo.read (1);
    if (scope.blockSize1 < 1)
        return false;
    velocity = velocityFifoData[(size_t) scope.startIndex1];
    return true;
}

dpiano::Patch DpianoAudioProcessor::currentPatch() const
{
    dpiano::Patch patch;
    for (size_t i = 0; i < patch.v.size(); ++i)
        patch.v[i] = rawParams[i]->load (std::memory_order_relaxed);
    return patch;
}

void DpianoAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    scratchRight.assign ((size_t) juce::jmax (1, samplesPerBlock), 0.0f);
    keyboardState.reset();
    piano.setPatch (currentPatch());
    piano.prepare (sampleRate);
    setLatencySamples (piano.latencySamples()); // the output limiter's look-ahead
}

bool DpianoAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void DpianoAudioProcessor::handleMidi (const juce::MidiMessage& m)
{
    if (m.isNoteOn())
    {
        const float velocity = m.getFloatVelocity();
        lastVelocity.store (velocity, std::memory_order_relaxed);
        if (const auto scope = velocityFifo.write (1); scope.blockSize1 > 0) // full (no editor reading): dropped
            velocityFifoData[(size_t) scope.startIndex1] = velocity;
        noteCount.fetch_add (1, std::memory_order_release);
        piano.noteOn (m.getNoteNumber(), velocity);
    }
    else if (m.isNoteOff())
        piano.noteOff (m.getNoteNumber());
    else if (m.isController())
    {
        const int value = m.getControllerValue();
        switch (m.getControllerNumber())
        {
            case 64: piano.sustainPedalLevel ((float) value / 127.0f); break; // continuous: half-pedalling
            case 66: piano.sostenutoPedal (value >= 64); break;
            case 67: piano.softPedal (value >= 64); break;
            default: break;
        }
        if (m.isAllNotesOff() || m.isAllSoundOff())
            piano.allNotesOff();
    }
}

void DpianoAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    keyboardState.processNextMidiBuffer (midi, 0, numSamples, true);
    piano.setPatch (currentPatch());
    // Sanitised here too: the three values are separate atomics, so a block could see half an update
    piano.setVelocityMap (getVelocityMap().sanitised());

    if ((int) scratchRight.size() < numSamples)
        scratchRight.resize ((size_t) numSamples); // only if the host exceeds its promised block size

    auto* left = buffer.getWritePointer (0);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : scratchRight.data();

    // Render between MIDI events so timing is sample-accurate
    int position = 0;
    for (const auto meta : midi)
    {
        const int eventPos = juce::jlimit (0, numSamples, meta.samplePosition);
        if (eventPos > position)
        {
            piano.process (left + position, right + position, eventPos - position);
            position = eventPos;
        }
        handleMidi (meta.getMessage());
    }
    if (position < numSamples)
        piano.process (left + position, right + position, numSamples - position);

    if (buffer.getNumChannels() == 1)
        for (int i = 0; i < numSamples; ++i)
            left[i] = 0.5f * (left[i] + right[i]);
}

// Bypassed, an instrument's dry signal is silence: in time with the reported latency whatever it is
void DpianoAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    buffer.clear();
}

juce::AudioProcessorEditor* DpianoAudioProcessor::createEditor()
{
    return new DpianoAudioProcessorEditor (*this);
}

float DpianoAudioProcessor::onSaveGrid (const dpiano::ParamInfo& info, float value) const
{
    auto* param = apvts.getParameter (toJuce (info.id));
    return param == nullptr ? value : dustify::snapToSaveGrid (*param, value);
}

// Session state: the parameters, byte-for-byte reproducible (loading a state and saving it again
// gives the same data)
void DpianoAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    for (auto child : state)
        if (child.hasType ("PARAM"))
            if (const auto* info = dpiano::findParam (child["id"].toString().toStdString()))
                child.setProperty ("value", onSaveGrid (*info, (float) child["value"]), nullptr);

    if (auto xml = state.createXml())
    {
        xml->setAttribute ("uiScale", juce::String (uiScale, 2));
        // The keyboard calibration in use, so the project keeps its feel on another machine
        const auto map = getVelocityMap();
        auto* keyboard = xml->createNewChildElement (keyboardTag);
        keyboard->setAttribute ("velocityMin", velocityText (map.inMin));
        keyboard->setAttribute ("velocityMax", velocityText (map.inMax));
        keyboard->setAttribute ("sensitivity", velocityText (map.sensitivity));
        copyXmlToBinary (*xml, destData);
    }
}

void DpianoAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    uiScale = juce::jlimit (0.6f, 2.0f, (float) xml->getDoubleAttribute ("uiScale", 1.0));
    xml->removeAttribute ("uiScale");

    // A session's calibration wins; one saved before calibration existed uses this machine's
    if (auto* keyboard = xml->getChildByName (keyboardTag))
    {
        dpiano::VelocityMap map;
        map.inMin = (float) keyboard->getDoubleAttribute ("velocityMin", 0.0);
        map.inMax = (float) keyboard->getDoubleAttribute ("velocityMax", 1.0);
        map.sensitivity = (float) keyboard->getDoubleAttribute ("sensitivity", 0.0);
        setVelocityMap (map, false);
        xml->removeChildElement (keyboard, true);
    }
    else
    {
        setVelocityMap (loadMachineVelocityMap(), false);
    }
    apvts.replaceState (juce::ValueTree::fromXml (*xml));

    // replaceState() skips parameters whose restored value rounds to their current one, which
    // leaves a bool holding e.g. 0.8 instead of exactly 1 as far as the host can see. Push those
    // back (only those: see the dsynth processor)
    for (const auto& info : dpiano::allParams())
        if (auto* param = apvts.getParameter (toJuce (info.id)))
            if (const float target = param->convertTo0to1 (rawParams[(size_t) info.pid]->load());
                std::abs (param->getValue() - target) > 1.0e-6f)
                param->setValueNotifyingHost (target);
    updateHostDisplay (ChangeDetails().withProgramChanged (true));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DpianoAudioProcessor();
}
