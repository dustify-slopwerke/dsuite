#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include "dsp/LayeredSynth.h"

class DsynthAudioProcessor final : public juce::AudioProcessor
{
public:
    DsynthAudioProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    // Programs = factory presets
    int getNumPrograms() override { return juce::jmax (1, (int) factoryPresets.size()); }
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Presets (call from the message thread)
    struct FactoryPreset
    {
        juce::String name;
        juce::String category;
        std::string text;
    };
    const std::vector<FactoryPreset>& getFactoryPresets() const { return factoryPresets; }
    void loadPresetText (const std::string& text);
    std::string savePresetText (const juce::String& name) const;
    static juce::File userPresetDirectory();

    // Layers: the parameters (and the UI) always show the layer being edited, like the panel
    // select on a Jupiter-8; the other layer's patch is kept here. Message thread only.
    int getEditLayer() const { return editLayer.load(); }

    // Name of the last preset loaded or saved. Locked: hosts may change programs from any thread.
    juce::String getCurrentPresetName() const
    {
        const juce::ScopedLock lock (presetNameLock);
        return currentPresetName;
    }
    dsynth::Patch livePatch() const { return currentPatch(); }

    void setCurrentPresetName (const juce::String& name)
    {
        const juce::ScopedLock lock (presetNameLock);
        currentPresetName = name;
    }
    void setEditLayer (int layer);
    void copyEditLayerToOther();

    juce::AudioProcessorValueTreeState apvts;
    juce::MidiKeyboardState keyboardState;
    int editorTab = 0;       // remembered while the plugin is loaded
    float uiScale = 1.0f;    // editor zoom, saved with the session

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void loadFactoryPresets();
    void handleMidi (const juce::MidiMessage&);
    dsynth::Patch currentPatch() const;
    void applyToParameters (const dsynth::Patch&, bool includeLayerSetup);
    void notifyHostOfNewValues();
    float onSaveGrid (dsynth::PID, float value) const;
    std::array<dsynth::Patch, 2> bothLayers() const; // message thread

    dsynth::LayeredSynth synth;

    // Stored layer patches. The edit layer's entry is only refreshed when switching layers or
    // saving; while `swapping`, the audio thread plays both stored patches instead of the
    // half-updated parameters.
    std::array<dsynth::Patch, 2> layerPatches { dsynth::Patch::defaults(), dsynth::Patch::defaults() };
    std::array<dsynth::Patch, 2> audioLayerCache { dsynth::Patch::defaults(), dsynth::Patch::defaults() };
    std::atomic<int> editLayer { 0 };
    std::atomic<bool> swapping { false };
    juce::SpinLock layerLock;
    juce::CriticalSection presetNameLock;
    juce::String currentPresetName;
    std::array<std::atomic<float>*, dsynth::numParams> rawParams {};
    std::vector<FactoryPreset> factoryPresets;
    std::vector<float> scratchRight; // right channel when the host gives us a mono bus
    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DsynthAudioProcessor)
};
