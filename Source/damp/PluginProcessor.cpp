#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "LoopFlac.h"

#include "BinaryData.h"
#include "dsp/Preset.h"

#include "common/plugin/ParamLayout.h"

#include <map>

using dustify::toJuce;

DampAudioProcessor::DampAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", dustify::createParameterLayout (damp::allParams()))
{
    for (const auto& info : damp::allParams())
        rawParams[(size_t) info.pid] = apvts.getRawParameterValue (toJuce (info.id));

    factoryPresets = dustify::loadFactoryPresets (BinaryData::namedResourceListSize, BinaryData::namedResourceList,
                                                  BinaryData::originalFilenames, BinaryData::getNamedResource,
                                                  [] (const std::string& text)
                                                  {
                                                      const auto parsed = damp::parsePreset (text);
                                                      jassert (parsed.warnings.empty());
                                                      return std::pair { parsed.name, parsed.category };
                                                  });

    // The same at every oversampling factor, so it never changes while the plugin runs
    setBoard (damp::BoardOrder::defaults());
    setLatencySamples (damp::Rig::latencySamples);
    startTimerHz (20);
}

DampAudioProcessor::~DampAudioProcessor()
{
    stopTimer();
}

// ---- Cabinet ----

std::unique_ptr<dustify::ConvolutionKernel> DampAudioProcessor::buildCabKernel (const damp::CabSettings& settings) const
{
    std::vector<float> impulse;
    if (settings.cab == damp::CabType::User)
    {
        const std::lock_guard lock (kernelLock);
        impulse = impulseSamples.empty() ? std::vector<float> { 1.0f }
                                         : damp::prepareUserImpulse (impulseSamples, impulseRate, cabRate, damp::maxImpulseSeconds);
    }
    else
    {
        impulse = damp::modelledCabinet (settings, cabRate);
    }
    return std::make_unique<dustify::ConvolutionKernel> (impulse.data(), (int) impulse.size());
}

// Message thread: builds a kernel for changed cab settings (or a new user IR) and hands it over
void DampAudioProcessor::rebuildCab (bool force)
{
    if (cabRate <= 0.0)
        return; // not prepared yet: prepareToPlay builds it
    const auto settings = damp::cabSettings (currentPatch());
    int version = 0;
    {
        const std::lock_guard lock (kernelLock);
        version = impulseVersion;
    }
    const bool userChanged = settings.cab == damp::CabType::User && version != builtImpulseVersion;
    if (! force && settings == builtSettings && ! userChanged)
        return;
    builtSettings = settings;
    builtImpulseVersion = version;

    auto kernel = buildCabKernel (settings);
    const std::lock_guard lock (kernelLock);
    auto* fresh = kernel.get();
    kernels.push_back (std::move (kernel));
    if (auto* untaken = pendingKernel.exchange (fresh))
        std::erase_if (kernels, [untaken] (const auto& k) { return k.get() == untaken; });

    // Free what the audio thread has finished with: everything before the kernel it took last
    // but one (it takes a kernel only once the cab has finished switching to the one before:
    // see processBlock)
    const auto* taken = takenKernel.load();
    const auto it = std::find_if (kernels.begin(), kernels.end(), [taken] (const auto& k) { return k.get() == taken; });
    if (it != kernels.end() && it != kernels.begin())
        kernels.erase (kernels.begin(), it - 1);
}

void DampAudioProcessor::timerCallback()
{
    rebuildCab (false);
    if (rawParams[(size_t) damp::PID::LooperOn]->load (std::memory_order_relaxed) >= 0.5f)
        ensureLooperPool();
}

// ---- Looper ----

// Message thread: the pool, on first use, at the host rate
void DampAudioProcessor::ensureLooperPool()
{
    const double rate = looperRate.load();
    auto& looper = rig.looper();
    if (rate > 0.0 && ! juce::approximatelyEqual (looper.poolRate(), rate))
        looper.createPool (rate);
}

bool DampAudioProcessor::exportTrack (int track, const juce::File& file, bool asPlayed, juce::String& error)
{
    auto& looper = rig.looper();
    std::vector<float> left, right;
    if (! (asPlayed ? looper.renderLoop (track, left, right) : looper.copyTrackAudio (track, left, right)))
    {
        error = "Track " + juce::String (track + 1) + " is empty";
        return false;
    }
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    if (! static_cast<juce::FileOutputStream&> (*stream).openedOk())
    {
        error = "Couldn't write " + file.getFullPathName();
        return false;
    }
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                   .withSampleRate (looper.poolRate())
                                                   .withNumChannels (2)
                                                   .withBitsPerSample (32)
                                                   .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    const float* channels[] { left.data(), right.data() };
    if (writer == nullptr || ! writer->writeFromFloatArrays (channels, 2, (int) left.size()))
    {
        error = "Couldn't write " + file.getFullPathName();
        return false;
    }
    return true;
}

bool DampAudioProcessor::importTrack (int track, const juce::File& file, juce::String& error)
{
    ensureLooperPool();
    auto& looper = rig.looper();
    const double rate = looper.poolRate();
    if (rate <= 0.0)
    {
        error = "The looper isn't running yet";
        return false;
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
    {
        error = "Couldn't read " + file.getFileName() + " as audio";
        return false;
    }
    // No more than the pool holds (before resampling)
    const auto status = looper.status();
    const auto most = (juce::int64) (status.poolTotalSeconds * reader->sampleRate) + 1;
    const auto length = (int) juce::jmin<juce::int64> (reader->lengthInSamples, most);
    juce::AudioBuffer<float> buffer (2, length);
    reader->read (&buffer, 0, length, 0, true, true);
    if (reader->numChannels < 2)
        buffer.copyFrom (1, 0, buffer, 0, 0, length);
    const auto left = damp::looper::resample (std::vector<float> (buffer.getReadPointer (0), buffer.getReadPointer (0) + length),
                                              reader->sampleRate, rate);
    const auto right = damp::looper::resample (std::vector<float> (buffer.getReadPointer (1), buffer.getReadPointer (1) + length),
                                               reader->sampleRate, rate);
    if (looper.setTrackAudio (track, left.data(), right.data(), (int64_t) left.size()) <= 0)
    {
        error = "The looper's memory is full";
        return false;
    }
    return true;
}

bool DampAudioProcessor::loadUserImpulse (const juce::File& file, juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
    {
        error = "Couldn't read " + file.getFileName() + " as audio";
        return false;
    }

    // First channel only; anything past the longest IR we use isn't needed
    const auto length = (int) juce::jmin<juce::int64> (reader->lengthInSamples, (juce::int64) (damp::maxImpulseSeconds * reader->sampleRate) + 1);
    juce::AudioBuffer<float> buffer (1, length);
    reader->read (&buffer, 0, length, 0, true, false);
    setUserImpulse (file.getFileNameWithoutExtension(), std::vector<float> (buffer.getReadPointer (0), buffer.getReadPointer (0) + length),
                    reader->sampleRate);

    if (auto* param = apvts.getParameter (toJuce (damp::paramInfo (damp::PID::Cab).id)))
        param->setValueNotifyingHost (param->convertTo0to1 ((float) damp::CabType::User));
    return true;
}

void DampAudioProcessor::setUserImpulse (const juce::String& name, std::vector<float> samples, double rate)
{
    {
        const std::lock_guard lock (kernelLock);
        impulseName = name;
        impulseSamples = std::move (samples);
        impulseRate = rate;
        ++impulseVersion;
    }
    if (juce::MessageManager::existsAndIsCurrentThread())
        rebuildCab (false);
}

juce::String DampAudioProcessor::getUserImpulseName() const
{
    const std::lock_guard lock (kernelLock);
    return impulseName;
}

std::vector<float> DampAudioProcessor::getUserImpulse (double& rate) const
{
    const std::lock_guard lock (kernelLock);
    rate = impulseRate;
    return impulseSamples;
}

void DampAudioProcessor::setCurrentProgram (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) factoryPresets.size()))
        return;
    currentProgram = index;
    loadPresetText (factoryPresets[(size_t) index].text);
}

const juce::String DampAudioProcessor::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, (int) factoryPresets.size()) ? factoryPresets[(size_t) index].name
                                                                         : juce::String ("Init");
}

void DampAudioProcessor::loadPresetText (const std::string& text)
{
    const auto parsed = damp::parsePreset (text);
    setCurrentPresetName (juce::String (parsed.name));
    setBoard (parsed.board);
    for (const auto& info : damp::allParams())
        if (damp::inPresets (info.pid)) // a preset never touches the looper
            if (auto* param = apvts.getParameter (toJuce (info.id)))
                param->setValueNotifyingHost (param->convertTo0to1 (parsed.patch[info.pid]));
    // Every value may have changed at once: hosts re-read them all
    updateHostDisplay (ChangeDetails().withProgramChanged (true));
}

std::string DampAudioProcessor::savePresetText (const juce::String& name) const
{
    return damp::serialisePreset (currentPatch(), name.toStdString(), getBoard());
}

juce::File DampAudioProcessor::userPresetDirectory() const
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("damp/presets");
}

damp::Patch DampAudioProcessor::currentPatch() const
{
    damp::Patch patch;
    for (size_t i = 0; i < patch.v.size(); ++i)
        patch.v[i] = rawParams[i]->load (std::memory_order_relaxed);
    return patch;
}

// ---- Board order ----

damp::BoardOrder DampAudioProcessor::getBoard() const
{
    return boardOnMessageThread;
}

void DampAudioProcessor::setBoard (const damp::BoardOrder& b)
{
    boardOnMessageThread = b;
    boardSequence.fetch_add (1, std::memory_order_acq_rel);
    for (size_t i = 0; i < b.slots.size(); ++i)
        boardSlots[i].store (b.slots[i], std::memory_order_relaxed);
    boardSequence.fetch_add (1, std::memory_order_release);
}

damp::BoardOrder DampAudioProcessor::readBoard() const
{
    damp::BoardOrder b;
    for (;;)
    {
        const unsigned before = boardSequence.load (std::memory_order_acquire);
        if ((before & 1u) != 0)
            continue; // a write is under way (it takes nanoseconds)
        for (size_t i = 0; i < b.slots.size(); ++i)
            b.slots[i] = boardSlots[i].load (std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_acquire);
        if (boardSequence.load (std::memory_order_relaxed) == before)
            return b;
    }
}

// ---- Tuner and meters ----

void DampAudioProcessor::setTunerActive (bool active)
{
    tunerActive = active;
}

int DampAudioProcessor::readTunerSamples (float* dest, int maxSamples)
{
    const auto scope = tunerFifo.read (juce::jmin (maxSamples, tunerFifo.getNumReady()));
    if (scope.blockSize1 > 0)
        std::copy_n (tunerBuffer.data() + scope.startIndex1, scope.blockSize1, dest);
    if (scope.blockSize2 > 0)
        std::copy_n (tunerBuffer.data() + scope.startIndex2, scope.blockSize2, dest + scope.blockSize1);
    return scope.blockSize1 + scope.blockSize2;
}

void DampAudioProcessor::feedTuner (const juce::AudioBuffer<float>& buffer, int numInputs)
{
    const int n = buffer.getNumSamples();
    const auto* left = buffer.getReadPointer (0);
    const auto* right = numInputs > 1 ? buffer.getReadPointer (1) : left;
    for (int i = 0; i < n; ++i)
    {
        float x = 0.5f * (left[i] + right[i]);
        for (auto& f : tunerFilter)
            x = f.process (x);
        if (++decimationPhase < decimation)
            continue;
        decimationPhase = 0;
        if (tunerFifo.getFreeSpace() > 0) // the editor fell behind: drop rather than block
        {
            const auto scope = tunerFifo.write (1);
            tunerBuffer[(size_t) (scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2)] = x;
        }
    }
}

void DampAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // Tuner feed at about 12 kHz: Butterworth low-pass at 4 kHz, then decimate
    decimation = juce::jmax (1, juce::roundToInt (sampleRate / 12000.0));
    tunerRate = sampleRate / decimation;
    for (size_t i = 0; i < tunerFilter.size(); ++i)
    {
        const double q = i == 0 ? 0.5412 : 1.3066; // 4th-order Butterworth pole pairs
        const double w = 2.0 * juce::MathConstants<double>::pi * juce::jmin (4000.0, 0.4 * sampleRate) / sampleRate;
        const double alpha = std::sin (w) / (2.0 * q), cosw = std::cos (w), a0 = 1.0 + alpha;
        auto& f = tunerFilter[i];
        f.b0 = f.b2 = (float) ((1.0 - cosw) / 2.0 / a0);
        f.b1 = (float) ((1.0 - cosw) / a0);
        f.a1 = (float) (-2.0 * cosw / a0);
        f.a2 = (float) ((1.0 - alpha) / a0);
        f.z1 = f.z2 = 0.0f;
    }
    tunerFifo.reset();

    auto& looper = rig.looper();
    looper.audioStopped();
    rig.setPatch (currentPatch());
    rig.setBoard (readBoard());
    rig.prepare (sampleRate, (int) (damp::maxImpulseSeconds * sampleRate) + 1);

    // The looper's pool is in frames at the host rate: at a new rate its loops are resampled
    // (undo history is dropped), and loops from a session loaded before now go in
    {
        const std::lock_guard lock (looperLock);
        looperRate = sampleRate;
        if (pendingLoops.has_value())
        {
            if (! juce::approximatelyEqual (looper.poolRate(), sampleRate))
                looper.createPool (sampleRate);
            looper.loadSession (damp::looper::resampleSession (*pendingLoops, sampleRate));
            pendingLoops.reset();
        }
        else if (looper.hasPool() && ! juce::approximatelyEqual (looper.poolRate(), sampleRate))
        {
            const auto loops = looper.saveSession();
            looper.releasePool();
            looper.createPool (sampleRate);
            if (! loops.empty())
                looper.loadSession (damp::looper::resampleSession (loops, sampleRate));
        }
    }

    // Audio is stopped: rebuild the cabinet for this rate and install it directly
    cabRate = sampleRate;
    const auto settings = damp::cabSettings (currentPatch());
    auto kernel = buildCabKernel (settings);
    rig.setCabKernelNow (kernel.get());
    {
        const std::lock_guard lock (kernelLock);
        builtSettings = settings;
        builtImpulseVersion = impulseVersion;
        pendingKernel = nullptr;
        takenKernel = kernel.get();
        kernels.clear();
        kernels.push_back (std::move (kernel));
    }

    dryDelay.clear();
    dryOut.setSize (2, juce::jmax (1, samplesPerBlock));
    dryPos = 0;
}

bool DampAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    const auto mono = juce::AudioChannelSet::mono(), stereo = juce::AudioChannelSet::stereo();
    return (in == mono && (out == mono || out == stereo)) || (in == stereo && out == stereo);
}

// Pushes the block's input through the bypass delay into dryOut
void DampAudioProcessor::delayDry (const juce::AudioBuffer<float>& buffer, int numInputs)
{
    const int numSamples = buffer.getNumSamples();
    if (dryOut.getNumSamples() < numSamples)
        dryOut.setSize (2, numSamples, false, false, true); // only if the host exceeds its promised block size

    constexpr int length = damp::Rig::latencySamples;
    for (int ch = 0; ch < 2; ++ch)
    {
        const auto* in = buffer.getReadPointer (juce::jmin (ch, numInputs - 1));
        auto* line = dryDelay.getWritePointer (ch);
        auto* out = dryOut.getWritePointer (ch);
        int pos = dryPos;
        for (int i = 0; i < numSamples; ++i)
        {
            out[i] = line[pos];
            line[pos] = in[i];
            pos = pos + 1 == length ? 0 : pos + 1;
        }
    }
    dryPos = (dryPos + numSamples) % length;
}

void DampAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numInputs = juce::jmax (1, getTotalNumInputChannels());
    delayDry (buffer, numInputs);

    float peak = 0.0f;
    for (int ch = 0; ch < juce::jmin (numInputs, buffer.getNumChannels()); ++ch)
        peak = juce::jmax (peak, buffer.getMagnitude (ch, 0, numSamples));
    if (peak > inputPeak.load (std::memory_order_relaxed))
        inputPeak = peak;
    const bool tuning = tunerActive.load (std::memory_order_relaxed);
    if (tuning)
        feedTuner (buffer, numInputs);

    // One kernel at a time: the cab switches at its next 64-sample boundary and then crossfades,
    // reading the kernel before until it's done, and rebuildCab() frees all but the last two
    // taken. Taking another sooner (e.g. two in blocks shorter than 64 samples) could leave the
    // cab playing a freed one. A newer kernel waits in pendingKernel meanwhile.
    if (! rig.cabSwitching())
        if (auto* kernel = pendingKernel.exchange (nullptr))
        {
            rig.setCabKernel (kernel);
            takenKernel = kernel;
        }

    // The tempo: the host's when following it (a Standalone app's play head isn't a host's), else
    // the tempo parameter. The looper's beats follow the host's position while it plays.
    const auto patch = currentPatch();
    const bool follow = patch.on (damp::PID::TempoFollowHost) && wrapperType != wrapperType_Standalone;
    double bpm = patch[damp::PID::Tempo], ppq = 0.0;
    bool fromHost = false, hostRunning = false;
    if (auto* host = getPlayHead(); host != nullptr && follow)
        if (const auto position = host->getPosition())
            if (const auto hostBpm = position->getBpm(); hostBpm && *hostBpm > 0.0)
            {
                bpm = *hostBpm;
                fromHost = true;
                const auto hostPpq = position->getPpqPosition();
                hostRunning = position->getIsPlaying() && hostPpq.hasValue();
                ppq = hostPpq.orFallback (0.0);
            }
    effectiveTempo.store (bpm, std::memory_order_relaxed);
    tempoFromHost.store (fromHost, std::memory_order_relaxed);
    rig.setTransport (bpm, hostRunning, ppq);
    rig.setPatch (patch);
    rig.setBoard (readBoard());
    auto* left = buffer.getWritePointer (0);
    auto* right = buffer.getNumChannels() >= 2 ? buffer.getWritePointer (1) : nullptr;
    rig.process (left, right, numSamples, numInputs < 2); // a mono input can come out stereo

    if (tuning && tunerMute.load (std::memory_order_relaxed))
        buffer.clear();
    const float out = buffer.getMagnitude (0, numSamples);
    if (out > outputPeak.load (std::memory_order_relaxed))
        outputPeak = out;
}

void DampAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const int numInputs = juce::jmax (1, getTotalNumInputChannels());
    delayDry (buffer, numInputs);
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        buffer.copyFrom (ch, 0, dryOut, juce::jmin (ch, 1), 0, buffer.getNumSamples());
}

juce::AudioProcessorEditor* DampAudioProcessor::createEditor()
{
    return new DampAudioProcessorEditor (*this);
}

float DampAudioProcessor::onSaveGrid (const damp::ParamInfo& info, float value) const
{
    auto* param = apvts.getParameter (toJuce (info.id));
    return param == nullptr ? value : dustify::snapToSaveGrid (*param, value);
}

// Session state: the parameters, byte-for-byte reproducible (loading a state and saving it again
// gives the same data)
void DampAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    for (auto child : state)
        if (child.hasType ("PARAM"))
            if (const auto* info = damp::findParam (child["id"].toString().toStdString()))
                child.setProperty ("value", onSaveGrid (*info, (float) child["value"]), nullptr);

    if (auto xml = state.createXml())
    {
        xml->setAttribute ("uiScale", juce::String (uiScale, 2));
        xml->setAttribute ("tunerMute", tunerMute.load() ? 1 : 0);
        xml->setAttribute ("tunerA4", juce::String (tunerReference.load(), 1));
        xml->setAttribute ("board", juce::String (getBoard().format()));

        // A user IR travels with the session, so projects stay portable
        double rate = 0.0;
        const auto samples = getUserImpulse (rate);
        if (! samples.empty())
        {
            auto* ir = xml->createNewChildElement ("IMPULSE");
            ir->setAttribute ("name", getUserImpulseName());
            ir->setAttribute ("rate", rate);
            juce::MemoryBlock data (samples.data(), samples.size() * sizeof (float)); // little-endian float32
            ir->setAttribute ("data", data.toBase64Encoding());
        }
        copyXmlToBinary (*xml, destData);

        // The looper's loops follow the XML in a binary block of their own (see LoopSession: FLAC,
        // 24-bit at a power-of-two scale per track; older sessions' float32 blocks still load),
        // which getXmlFromBinary ignores: it reads only the XML's own length. A session without
        // loops has no block. Undo history isn't saved.
        std::optional<damp::looper::LoopSession> loops;
        {
            const std::lock_guard lock (looperLock);
            loops = pendingLoops;
        }
        if (! loops.has_value() && rig.looper().hasPool())
            loops = rig.looper().saveSession();
        if (loops.has_value() && ! loops->empty())
        {
            std::vector<std::byte> block;
            damp::looper::encodeSession (*loops, block, &LoopFlacPacker::instance());
            destData.append (block.data(), block.size());
        }
    }
}

void DampAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    // A looper block after the XML (magic, string length, string, its terminating zero)
    std::optional<damp::looper::LoopSession> loops;
    const auto xmlBytes = (size_t) juce::ByteOrder::littleEndianInt (juce::addBytesToPointer (data, 4)) + 9;
    if ((size_t) sizeInBytes > xmlBytes)
        loops = damp::looper::decodeSession (juce::addBytesToPointer (data, (int) xmlBytes), (size_t) sizeInBytes - xmlBytes,
                                             &LoopFlacPacker::instance());

    uiScale = juce::jlimit (0.6f, 2.0f, (float) xml->getDoubleAttribute ("uiScale", 1.0));
    xml->removeAttribute ("uiScale");
    tunerMute = xml->getIntAttribute ("tunerMute", 1) != 0;
    tunerReference = (float) juce::jlimit (415.0, 466.0, xml->getDoubleAttribute ("tunerA4", 440.0));
    setBoard (damp::BoardOrder::parse (xml->getStringAttribute ("board").toStdString()));
    xml->removeAttribute ("tunerMute");
    xml->removeAttribute ("tunerA4");
    xml->removeAttribute ("board");

    std::vector<float> samples;
    juce::String irName;
    double irRate = 48000.0;
    if (auto* ir = xml->getChildByName ("IMPULSE"))
    {
        juce::MemoryBlock encoded;
        if (encoded.fromBase64Encoding (ir->getStringAttribute ("data")) && encoded.getSize() >= sizeof (float))
        {
            samples.resize (encoded.getSize() / sizeof (float));
            std::memcpy (samples.data(), encoded.getData(), samples.size() * sizeof (float));
            irName = ir->getStringAttribute ("name");
            irRate = juce::jlimit (8000.0, 768000.0, ir->getDoubleAttribute ("rate", 48000.0));
        }
        xml->removeChildElement (ir, true);
    }
    setUserImpulse (irName, std::move (samples), irRate);

    // replaceState() leaves parameters the session doesn't mention as they are: they go to their
    // defaults. A session from before the master output (master_out) applied output_volume after
    // the looper: its preset level becomes part of each loop's level, so the loops keep their
    // level against the live sound.
    std::map<juce::String, juce::XmlElement*> saved;
    for (auto* child : xml->getChildWithTagNameIterator ("PARAM"))
        saved[child->getStringAttribute ("id")] = child;
    const bool beforeMaster = saved.find ("master_out") == saved.end();
    const float oldVolume = saved.count ("output_volume") != 0 ? (float) saved["output_volume"]->getDoubleAttribute ("value") : 0.0f;
    for (const auto& info : damp::allParams())
        if (saved.find (toJuce (info.id)) == saved.end())
        {
            auto* param = xml->createNewChildElement ("PARAM");
            param->setAttribute ("id", toJuce (info.id));
            param->setAttribute ("value", info.def);
            saved[toJuce (info.id)] = param;
        }
    if (beforeMaster && loops.has_value() && ! loops->empty())
        for (int t = 0; t < damp::looperTracks; ++t)
        {
            const auto& info = damp::paramInfo (damp::trackParam (t, damp::TrackParam::Level));
            auto* param = saved[toJuce (info.id)];
            const float db = (float) param->getDoubleAttribute ("value", info.def);
            param->setAttribute ("value", db <= info.min ? (double) info.min : (double) juce::jlimit (info.min, info.max, db + oldVolume));
        }
    apvts.replaceState (juce::ValueTree::fromXml (*xml));

    // replaceState() skips parameters whose restored value rounds to their current one, which
    // leaves a bool holding e.g. 0.8 instead of exactly 1 as far as the host can see. Push those
    // back (only those: see the dsynth processor)
    for (const auto& info : damp::allParams())
        if (auto* param = apvts.getParameter (toJuce (info.id)))
            if (const float target = param->convertTo0to1 (rawParams[(size_t) info.pid]->load());
                std::abs (param->getValue() - target) > 1.0e-6f)
                param->setValueNotifyingHost (target);
    updateHostDisplay (ChangeDetails().withProgramChanged (true));

    // The session's loops replace the looper's (none: it's cleared). Before prepareToPlay the
    // rate isn't known, so they wait.
    auto& looper = rig.looper();
    looper.resyncTransport(); // restored buttons that are "on" mustn't act
    const std::lock_guard lock (looperLock);
    const double rate = looperRate.load();
    if (loops.has_value() && ! loops->empty() && rate <= 0.0)
    {
        pendingLoops = std::move (loops);
        return;
    }
    pendingLoops.reset();
    if (loops.has_value() && ! loops->empty())
    {
        if (! juce::approximatelyEqual (looper.poolRate(), rate))
            looper.createPool (rate);
        looper.loadSession (damp::looper::resampleSession (*loops, rate));
    }
    else if (looper.hasPool())
    {
        damp::looper::LoopSession none;
        none.sampleRate = looper.poolRate();
        looper.loadSession (none);
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DampAudioProcessor();
}
