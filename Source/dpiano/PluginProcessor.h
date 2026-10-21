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
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;

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

    // Keyboard velocity calibration (see dsp/VelocityMap.h). Not a preset parameter: a machine-wide
    // settings file holds it for every instance, and the session keeps a copy (which wins when a
    // session has one). Message thread.
    dpiano::VelocityMap getVelocityMap() const;
    // Applies a map to this instance; with saveMachineWide also writes it to the settings file
    void setVelocityMap (const dpiano::VelocityMap&, bool saveMachineWide);
    // <user app data>/dpiano/dpiano.settings, or dpiano.settings in $DUSTIFY_SETTINGS_DIR (tests and
    // tools set that so they never touch the user's file)
    static juce::File settingsFile();
    static dpiano::VelocityMap loadMachineVelocityMap();

    // Note-on velocities as played (before the map), recorded by the audio thread for the editor:
    // the latest one, a count of notes, and a FIFO of recent velocities (single reader)
    float lastPlayedVelocity() const { return lastVelocity.load (std::memory_order_relaxed); }
    int playedNoteCount() const { return noteCount.load (std::memory_order_acquire); }
    bool popPlayedVelocity (float& velocity);

    juce::AudioProcessorValueTreeState apvts;
    juce::MidiKeyboardState keyboardState;
    float uiScale = 1.0f; // editor zoom, saved with the session
    int editorTab = 0;    // remembered while the plugin is loaded

private:
    void handleMidi (const juce::MidiMessage&);
    dpiano::Patch currentPatch() const;
    float onSaveGrid (const dpiano::ParamInfo&, float value) const;

    dpiano::Piano piano;
    std::atomic<float> velocityMin { 0.0f }, velocityMax { 1.0f }, velocitySensitivity { 0.0f };
    std::atomic<float> lastVelocity { 0.0f };
    std::atomic<int> noteCount { 0 };
    static constexpr int velocityFifoSize = 128;
    juce::AbstractFifo velocityFifo { velocityFifoSize };
    std::array<float, velocityFifoSize> velocityFifoData {};
    juce::CriticalSection presetNameLock;
    juce::String currentPresetName;
    std::array<std::atomic<float>*, dpiano::numParams> rawParams {};
    std::vector<dustify::FactoryPreset> factoryPresets;
    std::vector<float> scratchRight; // right channel when the host gives us a mono bus
    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DpianoAudioProcessor)
};
