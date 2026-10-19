#include "Voice.h"

namespace dsynth
{
namespace
{
constexpr int controlInterval = 32;    // internal samples between drift updates
constexpr float inputScale = 0.5f;     // mixer -> filter input level at 0 dB drive
constexpr float outputScale = 2.0f;    // filter output makeup
constexpr float noiseFloor = 1.0e-5f;  // thermal noise; also lets a resonant filter start oscillating
constexpr int numDests = (int) MatrixDest::Amp + 1;

double noteToHz (double semis) { return 440.0 * fastExp2 ((semis - 69.0) * (1.0 / 12.0)); }
} // namespace

void Voice::prepare (double internalRate, int index, int step)
{
    sampleRate = internalRate;
    controlStep = std::max (1, step);
    controlRate = sampleRate / controlStep;
    controlPhase = 0;
    ladder.setSampleRate (sampleRate);
    ladder.reset();
    ota.setSampleRate (sampleRate);
    ota.reset();
    sem.setSampleRate (sampleRate);
    sem.reset();
    ms20.setSampleRate (sampleRate);
    ms20.reset();
    voiceHpf.setSampleRate (sampleRate);
    voiceHpf.reset();
    filterEnv.setSampleRate (controlRate);
    ampEnv.setSampleRate (controlRate);
    filterEnv.reset();
    ampEnv.reset();

    // Deterministic per-voice character so renders and tests are reproducible
    const auto seed = 0x1234567u + (uint32_t) index * 0x9e3779b9u;
    Random rng (seed);
    for (int i = 0; i < 3; ++i)
    {
        osc[i].reset (rng.unipolar()); // free-running oscillators start at arbitrary phases
        tolOsc[i] = rng.bipolar();
    }
    tolCutoff = rng.bipolar();
    tolEnvTime[0] = rng.bipolar();
    tolEnvTime[1] = rng.bipolar();
    noise.seed (seed ^ 0xabcdefu);
    lfo2.setSampleRate (controlRate);
    lfo2.seed (seed ^ 0x1f02u);
    lfo2.reset (rng.unipolar()); // without key sync each voice's LFO 2 runs at its own phase
    lfoFade[0] = lfoFade[1] = 1.0f;

    const double driftRate = controlRate / controlInterval;
    pitchWander.prepare (driftRate, 0.3, seed + 1);
    cutoffWander.prepare (driftRate, 0.2, seed + 2);

    subOsc.reset();
    gateCoeff = (float) (1.0 - std::exp (-1.0 / (0.0015 * controlRate))); // 1.5 ms declick in gate mode
    gateLevel = 0.0f;
    exprCoeff = (float) (1.0 - std::exp (-1.0 / (0.005 * controlRate))); // 5 ms expression smoothing
    ctl = {};
    exprPressure = exprBend = exprTimbre = 0.0f;
    active = gate = hasPlayed = false;
    detune = duoOffset = 0.0f;
    setPan (0.0f);
    controlCounter = 0;
}

void Voice::update (const BlockParams& bp)
{
    auto envTime = [&] (float t, int which) { return t * (1.0f + 0.08f * bp.tolerance * tolEnvTime[which]); };

    filterEnv.setParameters (envTime (bp.fenv[0], 0), envTime (bp.fenv[1], 0), bp.fenv[2], envTime (bp.fenv[3], 0), bp.linearEnv);
    ampEnv.setParameters (envTime (bp.aenv[0], 1), envTime (bp.aenv[1], 1), bp.aenv[2], envTime (bp.aenv[3], 1), bp.linearEnv);

    glideCoeff = bp.glideTime <= 0.0f ? 1.0 : 1.0 - std::exp (-std::log (100.0) / (bp.glideTime * controlRate));
    vcaGate = bp.vcaGate;
    ota.setCharacter (bp.filterModel == FilterModel::Cem24 ? OtaFilter::cem : OtaFilter::ir3109);
    ladder.setCharacter (bp.filterModel == FilterModel::Arp24 ? LadderFilter::arp : LadderFilter::moog);
    voiceHpf.setup (bp.voiceHpfCutoff, bp.voiceHpfReso);

    auto fadeStep = [&] (float seconds) { return seconds <= 0.0f ? 1.0f : (float) (1.0 / (seconds * controlRate)); };
    lfoFadeStep[0] = fadeStep (bp.lfoDelay);
    lfoFadeStep[1] = fadeStep (bp.lfo2Delay);
}

void Voice::setPan (float position)
{
    const double angle = (std::clamp (position, -1.0f, 1.0f) + 1.0) * std::numbers::pi / 4.0;
    panLeft = (float) (std::sqrt (2.0) * std::cos (angle));
    panRight = (float) (std::sqrt (2.0) * std::sin (angle));
}

void Voice::start (int newNote, float newVelocity, bool retrigger, bool glide, const BlockParams& bp, int midiChannel)
{
    note = newNote;
    channel = midiChannel;
    targetPitch = (double) newNote + bp.tuning[newNote % 12];

    if (! glide || ! hasPlayed || bp.glideTime <= 0.0f)
        pitch = targetPitch;
    linearGlideStep = bp.glideTime > 0.0f ? std::abs (targetPitch - pitch) / (bp.glideTime * controlRate) : 0.0;

    // Phase reset gives an identical attack every time. Only from silence, so voices that are
    // stolen or legato don't click.
    if (bp.phaseReset && isFree())
    {
        for (auto& o : osc)
            o.reset();
        subOsc.reset();
    }

    // An idle voice stops processing, so its filter holds a stale state; start it clean. A
    // self-oscillating filter is still running (active) and keeps its ringing.
    if (! active)
    {
        ladder.reset();
        ota.reset();
        sem.reset();
        ms20.reset();
        voiceHpf.reset();
    }

    if (retrigger || ! gate)
    {
        velocity = newVelocity;
        filterEnv.gateOn();
        ampEnv.gateOn();
        ++triggerCount;

        // LFO delay: modulation fades in again with every (re)triggered note
        lfoFade[0] = bp.lfoDelay > 0.0f ? 0.0f : 1.0f;
        lfoFade[1] = bp.lfo2Delay > 0.0f ? 0.0f : 1.0f;
        if (bp.lfo2KeySync)
            lfo2.reset();
    }

    gate = active = hasPlayed = true;
}

void Voice::release()
{
    gate = false;
    filterEnv.gateOff();
    ampEnv.gateOff();
}

void Voice::kill()
{
    gate = active = false;
    filterEnv.reset();
    ampEnv.reset();
}

void Voice::controlTick (const BlockParams& bp)
{
    driftPitchCents = bp.drift * 6.0f * pitchWander.next();
    driftCutoffOct = bp.drift * 0.08f * cutoffWander.next();
}

float Voice::filter (float input, double cutoffHz, float resonance, float shape, const BlockParams& bp)
{
    switch (bp.filterModel)
    {
        case FilterModel::Ladder24:
        case FilterModel::Arp24: return ladder.process (input, cutoffHz, resonance, bp.resComp);
        case FilterModel::Sem12: return sem.process (input, cutoffHz, resonance, shape, bp.filterBandpass);
        case FilterModel::Ms20: return ms20.process (input, cutoffHz, resonance);
        case FilterModel::Ladder12: return ladder.process (input, cutoffHz, resonance, bp.resComp, true);
        case FilterModel::Cem24:
        case FilterModel::Ir3109_24: return ota.process (input, cutoffHz, resonance, bp.resComp);
        case FilterModel::Ir3109_12: return ota.process (input, cutoffHz, resonance, bp.resComp, true);
    }
    return input;
}

bool Voice::filterSelfOscillates (float resonance, const BlockParams& bp) const
{
    switch (bp.filterModel)
    {
        case FilterModel::Ladder24:
        case FilterModel::Ladder12:
        case FilterModel::Arp24: return ladder.isSelfOscillating (resonance);
        case FilterModel::Cem24:
        case FilterModel::Ir3109_24:
        case FilterModel::Ir3109_12: return ota.isSelfOscillating (resonance);
        case FilterModel::Ms20: return Ms20Filter::isSelfOscillating (resonance);
        case FilterModel::Sem12: return false;
    }
    return false;
}

void Voice::updateControl (const Frame& f, float noiseSample, const BlockParams& bp)
{
    if (controlCounter-- <= 0)
    {
        controlCounter = controlInterval - 1;
        controlTick (bp);
    }

    // Glide
    if (bp.glideType == GlideType::Linear && bp.glideTime > 0.0f)
        pitch = pitch < targetPitch ? std::min (targetPitch, pitch + linearGlideStep)
                                    : std::max (targetPitch, pitch - linearGlideStep);
    else
        pitch += (targetPitch - pitch) * glideCoeff;

    // Expression: the strongest of channel and per-note pressure; MPE adds per-note bend/timbre
    {
        const bool member = bp.mpe && channel > 0 && bp.channels != nullptr;
        const NoteExpression none;
        const auto& ex = member ? bp.channels[channel] : none;
        const float notePressure = bp.notePressure != nullptr ? bp.notePressure[note] : 0.0f;
        exprPressure += (std::max ({ bp.aftertouch, notePressure, ex.pressure }) - exprPressure) * exprCoeff;
        exprBend += (ex.bend * bp.mpeBendRange - exprBend) * exprCoeff;
        exprTimbre += ((member ? ex.timbre : bp.timbre) - exprTimbre) * exprCoeff;
    }
    const float fenv = filterEnv.next();
    const float aenv = ampEnv.next();

    lfoFade[0] = std::min (1.0f, lfoFade[0] + lfoFadeStep[0]);
    const float lfo1 = f.lfo * lfoFade[0];
    float lfo2Out = 0.0f;
    if (bp.lfo2Used)
    {
        lfoFade[1] = std::min (1.0f, lfoFade[1] + lfoFadeStep[1]);
        lfo2Out = lfo2.next (bp.lfo2Rate, bp.lfo2Wave) * lfoFade[1];
    }

    // Mod matrix (osc 3 is the previous sample, to avoid a delay-free loop)
    float dest[numDests] {};
    if (bp.numMatrix > 0)
    {
        const float keySource = (float) (note - 60) / 60.0f;
        const float sources[] { 0.0f, lfo1, fenv, aenv, lastOsc3, velocity, keySource, bp.modWheel, exprPressure, noiseSample, exprTimbre, lfo2Out };
        static_assert (std::size (sources) == (size_t) MatrixSource::Lfo2 + 1, "one value per matrix source");
        for (int m = 0; m < bp.numMatrix; ++m)
            dest[(int) bp.matrix[m].dest] += bp.matrix[m].depth * sources[(int) bp.matrix[m].source];
    }
    auto d = [&] (MatrixDest which) { return dest[(int) which]; };

    // Wheel mod
    const float wheelSource = bp.wheelSource == ModSource::Lfo ? lfo1 : lastOsc3;
    const float wheelMod = wheelSource + (noiseSample - wheelSource) * bp.modMix;
    const double wheelPitch = bp.modPitchSemis * wheelMod;
    const float wheelPW = bp.modPW * wheelMod;

    // DCOs don't drift or carry tuning tolerance; the filter and envelopes still do
    const double vco = bp.dco ? 0.0 : 1.0;
    const double basePitch = pitch + detune + exprBend + bp.pitchOffset + vco * driftPitchCents * 0.01 + d (MatrixDest::Pitch);
    auto oscPitch = [&] (int o, double extra)
    {
        const double key = bp.keyTrack[o] ? basePitch : 60.0 + bp.pitchOffset + d (MatrixDest::Pitch);
        return key + 12.0 * bp.octave[o] + bp.tune[o] + vco * 0.04 * bp.tolerance * tolOsc[o] + extra;
    };

    const double invRate = 1.0 / sampleRate;
    ctl.dt[2] = noteToHz (oscPitch (2, d (MatrixDest::Osc3Pitch))) * invRate;
    ctl.dt[1] = noteToHz (oscPitch (1, wheelPitch + duoOffset + d (MatrixDest::Osc2Pitch))) * invRate;
    ctl.dt[0] = noteToHz (oscPitch (0, wheelPitch + d (MatrixDest::Osc1Pitch) + bp.pmFenvSemis * fenv)) * invRate;
    ctl.pwMod[0] = wheelPW + d (MatrixDest::Osc1PW) + bp.pmFenvPW * fenv;
    ctl.pwMod[1] = wheelPW + d (MatrixDest::Osc2PW);
    ctl.levelMod[0] = d (MatrixDest::Osc1Level);
    ctl.levelMod[1] = d (MatrixDest::Osc2Level);
    ctl.levelMod[2] = d (MatrixDest::NoiseLevel);

    ctl.cutoffOct = f.cutoffOct + bp.envAmountOct * fenv
                    + bp.keyTrackFilter * (basePitch - 60.0) / 12.0
                    + bp.velFilterOct * velocity + bp.modFilterOct * wheelMod
                    + bp.pmFenvOct * fenv + d (MatrixDest::Cutoff)
                    + driftCutoffOct + 0.1 * bp.tolerance * tolCutoff;
    ctl.cutoffHz = fastExp2 (ctl.cutoffOct);
    ctl.resonance = std::clamp (f.resonance + d (MatrixDest::Resonance), 0.0f, 1.0f);

    gateLevel += ((gate ? 1.0f : 0.0f) - gateLevel) * gateCoeff;
    const float velAmp = 1.0f - bp.ampVel * (1.0f - velocity);
    ctl.amp = (vcaGate ? gateLevel : aenv) * velAmp * std::max (0.0f, 1.0f + d (MatrixDest::Amp));
    ctl.fenv = fenv;
}

void Voice::render (float* left, float* right, int n, const Frame* frames, const BlockParams& bp)
{
    if (! active)
        return;

    // Oscillators nobody hears or uses as a modulation source are skipped (their phase just stalls)
    const bool osc2Needed = bp.oscOn[1] || bp.osc2AsSource;
    const bool osc3Needed = bp.oscOn[2] || bp.osc3AsSource;
    const bool polyModOsc2 = bp.pmOsc2Semis != 0.0f || bp.crossMod > 0.0f;

    for (int i = 0; i < n; ++i)
    {
        const Frame& f = frames[i];
        const float noiseSample = bp.noiseColour == NoiseColour::Pink ? noise.pink() : noise.white();

        if (controlPhase-- <= 0)
        {
            controlPhase = controlStep - 1;
            updateControl (f, noiseSample, bp);
        }

        // Osc 3 first (mod source), then osc 2 (poly-mod source and sync master), then osc 1
        float s3 = 0.0f;
        if (osc3Needed)
            lastOsc3 = s3 = osc[2].process (ctl.dt[2], bp.wave[2], f.pw[2]);

        const float s2 = osc2Needed ? osc[1].process (ctl.dt[1], bp.wave[1], f.pw[1] + ctl.pwMod[1]) : 0.0f;

        double dt1 = ctl.dt[0];
        if (polyModOsc2)
            dt1 *= fastExp2 (bp.pmOsc2Semis * s2 * (1.0 / 12.0))
                   * std::max (0.0, 1.0 + 3.0 * bp.crossMod * s2); // Jupiter-style linear cross-mod
        const float pw1 = f.pw[0] + ctl.pwMod[0] + bp.pmOsc2PW * s2;
        const double sync = bp.sync1 && osc[1].didWrap() ? osc[1].wrapDistanceSamples() : -1.0;
        const float s1 = bp.oscOn[0] || f.ring > 0.0f ? osc[0].process (dt1, bp.wave[0], pw1, sync) : 0.0f;
        const float sub = f.sub > 0.0f ? subOsc.process (dt1 / bp.subDivide, Wave::Square, 0.5f) : 0.0f;

        float mix = noiseFloor * noiseSample;
        if (bp.oscOn[0]) mix += s1 * std::max (0.0f, f.level[0] + ctl.levelMod[0]);
        if (bp.oscOn[1]) mix += s2 * std::max (0.0f, f.level[1] + ctl.levelMod[1]);
        if (bp.oscOn[2]) mix += s3 * f.level[2];
        mix += noiseSample * std::max (0.0f, f.noise + ctl.levelMod[2]) + sub * f.sub + s1 * s2 * f.ring;

        // Filter: poly-mod from osc 2 moves the cutoff at audio rate, everything else at control rate
        const double cutoffHz = bp.pmOsc2Oct != 0.0f ? fastExp2 (ctl.cutoffOct + bp.pmOsc2Oct * s2) : ctl.cutoffHz;
        const float filtered = filter (voiceHpf.process (mix) * inputScale * f.inputGain, cutoffHz, ctl.resonance, f.shape, bp);

        const float y = filtered * outputScale * ctl.amp;
        left[i] += y * panLeft;
        right[i] += y * panRight;
    }

    // A self-oscillating filter keeps ringing behind the closed VCA, like the hardware, so the
    // voice stays active; otherwise the voice frees itself once its VCA has closed.
    if (isFree() && ! filterSelfOscillates (frames[n - 1].resonance, bp))
        active = false;
}

} // namespace dsynth
