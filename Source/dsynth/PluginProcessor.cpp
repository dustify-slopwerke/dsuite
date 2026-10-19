#include "PluginProcessor.h"

#include "PluginEditor.h"

#include "BinaryData.h"
#include "dsp/Preset.h"

#include "common/plugin/ParamLayout.h"

using dustify::toJuce;

DsynthAudioProcessor::DsynthAudioProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    for (const auto& info : dsynth::allParams())
        rawParams[(size_t) info.pid] = apvts.getRawParameterValue (toJuce (info.id));

    loadFactoryPresets();
}

juce::AudioProcessorValueTreeState::ParameterLayout DsynthAudioProcessor::createParameterLayout()
{
    return dustify::createParameterLayout (dsynth::allParams());
}

void DsynthAudioProcessor::loadFactoryPresets()
{
    std::vector<std::pair<juce::String, int>> files; // original filename -> resource index
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
        files.emplace_back (BinaryData::originalFilenames[i], i);
    std::sort (files.begin(), files.end());

    for (const auto& [filename, index] : files)
    {
        int size = 0;
        const auto* data = BinaryData::getNamedResource (BinaryData::namedResourceList[index], size);
        std::string text (data, (size_t) size);
        const auto parsed = dsynth::parsePreset (text);
        jassert (parsed.warnings.empty());
        factoryPresets.push_back ({ parsed.name.empty() ? filename : juce::String (parsed.name), juce::String (parsed.category), std::move (text) });
    }
}

void DsynthAudioProcessor::setCurrentProgram (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) factoryPresets.size()))
        return;

    currentProgram = index;
    loadPresetText (factoryPresets[(size_t) index].text);
}

const juce::String DsynthAudioProcessor::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, (int) factoryPresets.size()) ? factoryPresets[(size_t) index].name
                                                                         : juce::String ("Init");
}

void DsynthAudioProcessor::applyToParameters (const dsynth::Patch& patch, bool includeLayerSetup)
{
    for (const auto& info : dsynth::allParams())
        if (includeLayerSetup || ! info.global)
            if (auto* param = apvts.getParameter (toJuce (info.id)))
                param->setValueNotifyingHost (param->convertTo0to1 (patch[info.pid]));
}

void DsynthAudioProcessor::notifyHostOfNewValues()
{
    // Tells hosts that (potentially) every parameter value changed at once, so they re-read them
    // all: a CLAP params rescan, a VST3 parameter-values restart.
    updateHostDisplay (ChangeDetails().withProgramChanged (true));
}

void DsynthAudioProcessor::loadPresetText (const std::string& text)
{
    const auto parsed = dsynth::parsePreset (text);
    setCurrentPresetName (juce::String (parsed.name));

    {
        const juce::SpinLock::ScopedLockType lock (layerLock);
        layerPatches = { parsed.patch, parsed.hasLayerB ? parsed.layerB : dsynth::Patch::defaults() };
        swapping = true;
    }
    applyToParameters (parsed.patch, true);
    {
        const juce::SpinLock::ScopedLockType lock (layerLock);
        editLayer = 0;
        swapping = false;
    }
    notifyHostOfNewValues();
}

std::array<dsynth::Patch, 2> DsynthAudioProcessor::bothLayers() const
{
    const auto live = currentPatch();
    const juce::SpinLock::ScopedLockType lock (layerLock);
    auto layers = layerPatches;
    layers[(size_t) editLayer.load()] = live;
    for (const auto& info : dsynth::allParams())
        if (info.global)
            layers[0][info.pid] = live[info.pid]; // layer setup lives in layer A
    return layers;
}

std::string DsynthAudioProcessor::savePresetText (const juce::String& name) const
{
    const auto layers = bothLayers();
    const bool layered = layers[0].choice<dsynth::LayerMode> (dsynth::PID::LayerMode) != dsynth::LayerMode::Single;
    return dsynth::serialisePreset (layers[0], name.toStdString(), layered ? &layers[1] : nullptr);
}

void DsynthAudioProcessor::setEditLayer (int layer)
{
    layer = juce::jlimit (0, 1, layer);
    if (layer == editLayer.load())
        return;

    {
        const juce::SpinLock::ScopedLockType lock (layerLock);
        layerPatches[(size_t) editLayer.load()] = currentPatch();
        swapping = true;
    }
    applyToParameters (layerPatches[(size_t) layer], false);
    {
        const juce::SpinLock::ScopedLockType lock (layerLock);
        editLayer = layer;
        swapping = false;
    }
    notifyHostOfNewValues();
}

void DsynthAudioProcessor::copyEditLayerToOther()
{
    {
        const auto live = currentPatch();
        const juce::SpinLock::ScopedLockType lock (layerLock);
        layerPatches[(size_t) (1 - editLayer.load())] = live;
    }
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true)); // the session changed
}

juce::File DsynthAudioProcessor::userPresetDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("dsynth/presets");
}

dsynth::Patch DsynthAudioProcessor::currentPatch() const
{
    dsynth::Patch patch;
    for (size_t i = 0; i < patch.v.size(); ++i)
        patch.v[i] = rawParams[i]->load (std::memory_order_relaxed);
    return patch;
}

void DsynthAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    scratchRight.assign ((size_t) juce::jmax (1, samplesPerBlock), 0.0f);
    keyboardState.reset();
    synth.setPatches (currentPatch(), dsynth::Patch::defaults());
    synth.prepare (sampleRate);
}

bool DsynthAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void DsynthAudioProcessor::handleMidi (const juce::MidiMessage& m)
{
    const int channel = m.getChannel() - 1; // 0..15; channel 0 is the MPE master channel

    if (m.isNoteOn())
        synth.noteOn (m.getNoteNumber(), m.getFloatVelocity(), channel);
    else if (m.isNoteOff())
        synth.noteOff (m.getNoteNumber(), channel);
    else if (m.isPitchWheel())
        synth.pitchBend ((float) (m.getPitchWheelValue() - 8192) / 8192.0f, channel);
    else if (m.isChannelPressure())
        synth.aftertouch ((float) m.getChannelPressureValue() / 127.0f, channel);
    else if (m.isAftertouch())
        synth.polyPressure (m.getNoteNumber(), (float) m.getAfterTouchValue() / 127.0f);
    else if (m.isController() && m.getControllerNumber() == 74)
        synth.timbre ((float) m.getControllerValue() / 127.0f, channel);
    else if (m.isController() && m.getControllerNumber() == 1)
        synth.modWheel ((float) m.getControllerValue() / 127.0f);
    else if (m.isSustainPedalOn())
        synth.sustainPedal (true);
    else if (m.isSustainPedalOff())
        synth.sustainPedal (false);
    else if (m.isAllNotesOff() || m.isAllSoundOff())
        synth.allNotesOff();
}

void DsynthAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    keyboardState.processNextMidiBuffer (midi, 0, numSamples, true);
    // The edit layer plays the live parameters; the other layer plays its stored patch
    {
        const juce::SpinLock::ScopedTryLockType lock (layerLock);
        if (lock.isLocked())
            audioLayerCache = layerPatches;
    }
    const auto live = currentPatch();
    auto layers = audioLayerCache;
    if (! swapping.load())
        layers[(size_t) editLayer.load()] = live;
    for (const auto& info : dsynth::allParams())
        if (info.global)
            layers[0][info.pid] = live[info.pid];
    synth.setPatches (layers[0], layers[1]);

    double bpm = 0.0, ppq = -1.0;
    bool playing = false;
    if (auto* hostPlayHead = getPlayHead())
        if (const auto position = hostPlayHead->getPosition())
        {
            bpm = position->getBpm().orFallback (0.0);
            ppq = position->getPpqPosition().orFallback (-1.0);
            playing = position->getIsPlaying();
        }
    synth.setTransport (bpm, playing, ppq);

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
            synth.process (left + position, right + position, eventPos - position);
            position = eventPos;
        }
        handleMidi (meta.getMessage());
    }
    if (position < numSamples)
        synth.process (left + position, right + position, numSamples - position);

    if (buffer.getNumChannels() == 1)
        for (int i = 0; i < numSamples; ++i)
            left[i] = 0.5f * (left[i] + right[i]);
}

juce::AudioProcessorEditor* DsynthAudioProcessor::createEditor()
{
    return new DsynthAudioProcessorEditor (*this);
}

float DsynthAudioProcessor::onSaveGrid (dsynth::PID pid, float value) const
{
    auto* param = apvts.getParameter (toJuce (dsynth::paramInfo (pid).id));
    return param == nullptr ? value : dustify::snapToSaveGrid (*param, value);
}

// Session state: the parameters (edit layer + layer setup) plus both layers as preset text.
// Byte-for-byte reproducible: loading a state and saving it again gives the same data.
void DsynthAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    for (auto child : state)
        if (child.hasType ("PARAM"))
            if (const auto* info = dsynth::findParam (child["id"].toString().toStdString()))
                child.setProperty ("value", onSaveGrid (info->pid, (float) child["value"]), nullptr);

    auto layers = bothLayers();
    for (auto& patch : layers)
        for (const auto& info : dsynth::allParams())
            patch[info.pid] = onSaveGrid (info.pid, patch[info.pid]);
    const bool layered = layers[0].choice<dsynth::LayerMode> (dsynth::PID::LayerMode) != dsynth::LayerMode::Single;

    if (auto xml = state.createXml())
    {
        auto* layersXml = xml->createNewChildElement ("DSYNTH_LAYERS");
        layersXml->setAttribute ("edit", editLayer.load());
        layersXml->setAttribute ("uiScale", juce::String (uiScale, 2));
        layersXml->setAttribute ("text", juce::String (dsynth::serialisePreset (layers[0], "session", layered ? &layers[1] : nullptr)));
        copyXmlToBinary (*xml, destData);
    }
}

void DsynthAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    std::string layerText;
    int edit = 0;
    if (auto* layers = xml->getChildByName ("DSYNTH_LAYERS"))
    {
        layerText = layers->getStringAttribute ("text").toStdString();
        edit = juce::jlimit (0, 1, layers->getIntAttribute ("edit"));
        uiScale = juce::jlimit (0.6f, 2.0f, (float) layers->getDoubleAttribute ("uiScale", 1.0));
        xml->removeChildElement (layers, true);
    }

    {
        const juce::SpinLock::ScopedLockType lock (layerLock);
        if (! layerText.empty())
        {
            const auto parsed = dsynth::parsePreset (layerText);
            layerPatches = { parsed.patch, parsed.hasLayerB ? parsed.layerB : dsynth::Patch::defaults() };
        }
        swapping = true;
    }
    apvts.replaceState (juce::ValueTree::fromXml (*xml));

    // replaceState() skips parameters whose restored value rounds to their current one, which
    // leaves a bool holding e.g. 0.8 instead of exactly 1 as far as the host can see. Push those
    // back. Only those: re-setting a value that's already right can nudge its last float digit
    // through a skewed range, and the saved state would no longer be byte-for-byte stable.
    for (const auto& info : dsynth::allParams())
        if (auto* param = apvts.getParameter (toJuce (info.id)))
            if (const float target = param->convertTo0to1 (rawParams[(size_t) info.pid]->load());
                std::abs (param->getValue() - target) > 1.0e-6f)
                param->setValueNotifyingHost (target);
    {
        const juce::SpinLock::ScopedLockType lock (layerLock);
        editLayer = edit;
        swapping = false;
    }
    notifyHostOfNewValues();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DsynthAudioProcessor();
}
