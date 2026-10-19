#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "common/plugin/PresetHost.h"
#include "dsp/Cabinet.h"
#include "dsp/Rig.h"

#include "common/dsp/Convolver.h"

#include <mutex>
#include <optional>

class DampAudioProcessor final : public juce::AudioProcessor,
                                 public dustify::PresetHost,
                                 private juce::Timer
{
public:
    DampAudioProcessor();
    ~DampAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override { rig.looper().audioStopped(); }
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
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
    juce::String presetFileExtension() const override { return "damp"; }

    // Cabinet impulse responses (message thread). A loaded IR is kept in the session state.
    bool loadUserImpulse (const juce::File&, juce::String& error);
    void setUserImpulse (const juce::String& name, std::vector<float> samples, double rate);
    juce::String getUserImpulseName() const;
    std::vector<float> getUserImpulse (double& rate) const;

    // Tuner: while active, the audio thread feeds the input (decimated) to a FIFO the editor reads
    void setTunerActive (bool active);
    bool isTunerActive() const { return tunerActive.load(); }
    double tunerSampleRate() const { return tunerRate.load(); }
    int readTunerSamples (float* dest, int maxSamples);
    std::atomic<bool> tunerMute { true };        // silence the output while tuning (saved)
    std::atomic<float> tunerReference { 440.0f }; // A4 in Hz (saved)

    // Pedal order (message thread); saved with the session and in presets
    damp::BoardOrder getBoard() const;
    void setBoard (const damp::BoardOrder&);

    // The looper (see damp::Looper for its message-thread API). Its pool is allocated when looper_on
    // first goes on, or when a session with loops loads; the loops are saved with the session,
    // never in presets.
    damp::Looper& getLooper() { return rig.looper(); }
    // WAV files (message thread). Export: the recording as it is, or (asPlayed) one cycle as heard,
    // 32-bit float stereo at the host rate. Import: resampled to the host rate, cut to what fits.
    bool exportTrack (int track, const juce::File&, bool asPlayed, juce::String& error);
    bool importTrack (int track, const juce::File&, juce::String& error);

    // Peak levels since the last call (message thread)
    float takeInputPeak() { return inputPeak.exchange (0.0f); }
    float takeOutputPeak() { return outputPeak.exchange (0.0f); }

    juce::AudioProcessorValueTreeState apvts;
    float uiScale = 1.0f; // editor zoom, saved with the session

private:
    void timerCallback() override;
    std::unique_ptr<dustify::ConvolutionKernel> buildCabKernel (const damp::CabSettings&) const;
    void rebuildCab (bool force);

    damp::Patch currentPatch() const;
    float onSaveGrid (const damp::ParamInfo&, float value) const;
    void delayDry (const juce::AudioBuffer<float>&, int numInputs);
    void ensureLooperPool();

    damp::BoardOrder readBoard() const; // audio thread

    damp::Rig rig;
    juce::CriticalSection presetNameLock;
    juce::String currentPresetName;
    std::array<std::atomic<float>*, damp::numParams> rawParams {};
    std::vector<dustify::FactoryPreset> factoryPresets;
    int currentProgram = 0;

    // Cabinet: kernels are built on the message thread and handed to the audio thread through
    // `pendingKernel`; it reports the one it took in `takenKernel`. `kernels` (guarded by
    // kernelLock) owns them in the order they were handed over, and keeps the last two taken (one
    // may still be fading out) plus anything not yet taken.
    mutable std::mutex kernelLock;
    std::vector<std::unique_ptr<dustify::ConvolutionKernel>> kernels;
    std::atomic<dustify::ConvolutionKernel*> pendingKernel { nullptr };
    std::atomic<const dustify::ConvolutionKernel*> takenKernel { nullptr };
    damp::CabSettings builtSettings;
    int builtImpulseVersion = -1;
    double cabRate = 0.0;
    juce::String impulseName;
    std::vector<float> impulseSamples;
    double impulseRate = 48000.0;
    int impulseVersion = 0;

    // Looper: loops from a session loaded before the host rate was known wait in pendingLoops
    std::mutex looperLock;
    std::optional<damp::looper::LoopSession> pendingLoops;
    std::atomic<double> looperRate { 0.0 };

    // Board order handed to the audio thread through a sequence lock: the writer makes the count
    // odd while it changes the slots; a reader retries if the count was odd or moved
    std::atomic<unsigned> boardSequence { 0 };
    std::array<std::atomic<int>, damp::BoardOrder::size> boardSlots;
    damp::BoardOrder boardOnMessageThread = damp::BoardOrder::defaults();

    // Tuner feed: a 4th-order low-pass, then every `decimation`th sample
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        float process (float x)
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    void feedTuner (const juce::AudioBuffer<float>&, int numInputs);
    std::atomic<bool> tunerActive { false };
    std::atomic<double> tunerRate { 12000.0 };
    juce::AbstractFifo tunerFifo { 16384 };
    std::vector<float> tunerBuffer = std::vector<float> (16384, 0.0f);
    std::array<Biquad, 2> tunerFilter;
    int decimation = 4, decimationPhase = 0;
    std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };

    // The input delayed by the latency, for bypass (kept running so switching is seamless)
    juce::AudioBuffer<float> dryDelay { 2, damp::Rig::latencySamples };
    juce::AudioBuffer<float> dryOut;
    int dryPos = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DampAudioProcessor)
};
