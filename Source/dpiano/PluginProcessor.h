#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "common/plugin/PresetHost.h"
#include "dsp/Piano.h"

class DpianoAudioProcessor final : public juce::AudioProcessor,
                                   public dustify::PresetHost
{
public:
    DpianoAudioProcessor();

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
    double getTailLengthSeconds() const override { return 3.0; }

    // Programs = factory presets
    int getNumPrograms() override { return juce::jmax (1, (int) factoryPresets.size()); }
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // PresetHost
    const std::vector<dustify::FactoryPreset>& getFactoryPresets() const override { return factoryPresets; }
    void loadFactoryPreset (int index) override { setCurrentProgram (index); }
    void loadPresetText (const std::string& text) override;
    std::string savePresetText (const juce::String& name) const override;
    juce::String getCurrentPresetName() const override
    {
        const juce::ScopedLock lock (presetNameLock);
        return currentPresetName;
    }
    void setCurrentPresetName (const juce::String& name) override
    {
        const juce::ScopedLock lock (presetNameLock);
        currentPresetName = name;
    }
    juce::File userPresetDirectory() const override;
    juce::String presetFileExtension() const override { return "dpiano"; }

    juce::AudioProcessorValueTreeState apvts;
    juce::MidiKeyboardState keyboardState;
    float uiScale = 1.0f; // editor zoom, saved with the session
    int editorTab = 0;    // remembered while the plugin is loaded

private:
    void handleMidi (const juce::MidiMessage&);
    dpiano::Patch currentPatch() const;
    float onSaveGrid (const dpiano::ParamInfo&, float value) const;

    dpiano::Piano piano;
    juce::CriticalSection presetNameLock;
    juce::String currentPresetName;
    std::array<std::atomic<float>*, dpiano::numParams> rawParams {};
    std::vector<dustify::FactoryPreset> factoryPresets;
    std::vector<float> scratchRight; // right channel when the host gives us a mono bus
    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DpianoAudioProcessor)
};
