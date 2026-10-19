#include "Amp.h"

#include "common/dsp/Denormals.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace damp
{
namespace
{
float dbToGain (float db) { return std::pow (10.0f, db / 20.0f); }

// Audio-taper pot: about a tenth of the signal at half way
float audioTaper (float position)
{
    constexpr float curve = 4.6f;
    return (std::exp (curve * position) - 1.0f) / (std::exp (curve) - 1.0f);
}
} // namespace

Amp::Amp()
{
    // Tube curves take a moment to build: do it here, never on the audio thread
    for (int m = 0; m < numModels; ++m)
        modelCurves ((AmpModel) m);
}

void Amp::setPatch (const Patch& p)
{
    patch = p;
    applyPatch (false);
}

void Amp::prepare (double sampleRate)
{
    rate = sampleRate;
    smoothing = dustify::Smoother::coefficient (0.02, rate);
    dcCoeff = (float) (1.0 - 2.0 * std::numbers::pi * 10.0 / rate);
    gate.prepare (rate);
    prepared = true;
    model = -1; // reconfigure for the rate
    reset();
}

void Amp::reset()
{
    gate.reset();
    for (auto& c : channels)
    {
        c.oversampler.reset();
        c.dcIn = c.dcOut = 0.0f;
    }
    applyPatch (true);
}

void Amp::applyPatch (bool snap)
{
    inputGain.target = dbToGain (patch[PID::InputGain]);
    gainPot.target = audioTaper (patch[PID::Gain] / 10.0f);
    masterPot.target = audioTaper (patch[PID::Master] / 10.0f);
    bass.target = patch[PID::Bass] / 10.0f;
    middle.target = patch[PID::Middle] / 10.0f;
    treble.target = patch[PID::Treble] / 10.0f;
    if (snap)
    {
        for (auto* s : { &inputGain, &gainPot, &masterPot, &bass, &middle, &treble })
            s->snap();
        toneSet[0] = -1.0f; // recompute the tone stack
    }

    gateOn = patch.on (PID::GateOn);
    gate.setThreshold (patch[PID::GateThreshold]);
    gate.setRelease (patch[PID::GateRelease]);

    const int newStages = 1 + std::clamp (patch.index (PID::Oversampling), 0, 2);
    const int newModel = std::clamp (patch.index (PID::Model), 0, numModels - 1);
    if (newStages != stages || newModel != model)
    {
        stages = newStages;
        model = newModel;
        for (auto& c : channels)
            c.oversampler.setStages (stages);
        if (prepared)
            configureModel();
    }

    const float presence = patch[PID::Presence] / 10.0f, resonance = patch[PID::Resonance] / 10.0f;
    for (auto& c : channels)
        c.power.setControls (presence, resonance, patch[PID::Sag]);
}

void Amp::configureModel()
{
    const auto& spec = modelSpec ((AmpModel) model);
    const auto& curves = modelCurves ((AmpModel) model);
    const double highRate = rate * (1 << stages);
    for (auto& c : channels)
    {
        for (int s = 0; s < spec.numStages; ++s)
            c.stages[(size_t) s].prepare (spec.stages[(size_t) s], curves.stage[(size_t) s], curves.unbypassed[(size_t) s], highRate);
        c.toneStack.prepare (highRate);
        c.bright.setCutoff (std::max (spec.brightHz, 20.0), highRate);
        c.bright.reset();
        c.power.prepare (spec.power, highRate);
    }
    toneSet[0] = -1.0f;
}

void Amp::process (float* left, float* right, int numSamples)
{
    dustify::ScopedFlushDenormals noDenormals;
    for (int done = 0; done < numSamples;)
    {
        const int n = std::min (numSamples - done, dustify::Oversampler::maxBlock);
        processChunk (left + done, right == nullptr ? nullptr : right + done, n);
        done += n;
    }
}

void Amp::processChunk (float* left, float* right, int n)
{
    float* io[2] { left, right };
    const int numChannels = right == nullptr ? 1 : 2;
    const auto& spec = modelSpec ((AmpModel) model);

    // Host rate: input gain and gate
    for (int i = 0; i < n; ++i)
    {
        const float in = inputGain.next (smoothing);
        float level = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            io[ch][i] *= in;
            level = std::max (level, std::abs (io[ch][i]));
        }
        const float g = gateOn ? gate.next (level) : 1.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            io[ch][i] *= g;
        gainRamp[(size_t) i] = gainPot.next (smoothing);
        masterRamp[(size_t) i] = masterPot.next (smoothing);
    }

    // Tone controls: move the stack once per chunk, when they've moved
    const float chunkSmoothing = 1.0f - std::pow (1.0f - smoothing, (float) n);
    const float t = treble.next (chunkSmoothing), m = middle.next (chunkSmoothing), b = bass.next (chunkSmoothing);
    if (std::abs (t - toneSet[0]) > 1.0e-4f || std::abs (m - toneSet[1]) > 1.0e-4f || std::abs (b - toneSet[2]) > 1.0e-4f)
    {
        toneSet[0] = t;
        toneSet[1] = m;
        toneSet[2] = b;
        for (auto& c : channels)
            c.toneStack.set (spec.toneStack, t, m, b);
    }

    const int high = n << stages;
    if (board != nullptr)
        board->beginBeforeAmp (oversampledRate(), high);
    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto& c = channels[(size_t) ch];
        float* x = c.oversampler.upsample (io[ch], n);
        if (board != nullptr)
            board->processBeforeAmp (ch, x, high);
        runModel (c, x, n);
        c.oversampler.downsample (io[ch], n);

        // Asymmetric clipping leaves DC
        for (int i = 0; i < n; ++i)
        {
            const float y = io[ch][i] - c.dcIn + dcCoeff * c.dcOut;
            c.dcIn = io[ch][i];
            c.dcOut = y;
            io[ch][i] = y;
        }
    }

    const auto trim = (float) spec.outputTrim;
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < n; ++i)
            io[ch][i] *= trim;
}

void Amp::runModel (Channel& c, float* x, int n)
{
    const auto& spec = modelSpec ((AmpModel) model);
    const bool bright = patch.on (PID::Bright) && spec.brightHz > 0.0;
    const int high = n << stages;
    const float drive = (float) spec.power.drive;

    // A bright cap across the top of the gain pot (wiper ratio a): treble rises from a at
    // 1 / (R_upper C) to full level at 1 / ((R_lower || R_upper) C), which with brightHz = 1 / (R C)
    // is brightHz / (a (1 - a)). Turned down, that corner is far above the audio band and the cap
    // adds a steep treble lift; turned up, there's little left to lift. Set once per chunk.
    if (bright)
    {
        const double a = std::clamp ((double) gainRamp[0], 1.0e-4, 1.0 - 1.0e-4);
        c.bright.setCutoffUnwarped (spec.brightHz / (a * (1.0 - a)), rate * (1 << stages));
    }

    for (int j = 0; j < high; ++j)
    {
        const int base = j >> stages;
        float v = x[j] * (float) spec.inputVolts; // volts at the first grid
        for (int s = 0; s < spec.numStages; ++s)
        {
            v = c.stages[(size_t) s].process (v);
            if (s == spec.toneStackAfter)
                v = c.toneStack.process (v);
            if (s == spec.gainPotAfter)
            {
                const float pot = gainRamp[(size_t) base];
                v = bright ? pot * v + (1.0f - pot) * c.bright.highpass (v) : pot * v;
            }
        }
        x[j] = c.power.process (v * masterRamp[(size_t) base] * drive);
    }
}

} // namespace damp
