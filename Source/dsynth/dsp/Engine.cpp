#include "Engine.h"

#include "common/dsp/Denormals.h"

#include <algorithm>
#include <cmath>

namespace dsynth
{
namespace
{
float octaveFor (Range r)
{
    switch (r)
    {
        case Range::Lo: return -7.0f;
        case Range::Ft32: return -2.0f;
        case Range::Ft16: return -1.0f;
        case Range::Ft8: return 0.0f;
        case Range::Ft4: return 1.0f;
        case Range::Ft2: return 2.0f;
    }
    return 0.0f;
}

float dbToGain (float db) { return db <= -48.0f ? 0.0f : std::pow (10.0f, db / 20.0f); }
} // namespace

void Engine::prepare (double hostSampleRate)
{
    hostRate = hostSampleRate;
    prepareInternalRate();

    chorus.prepare (hostRate);
    delay.prepare (hostRate);
    reverb.prepare (hostRate);
    arp.prepare (hostRate);
    lfo.reset();
    hostSmoothCoeff = Smoother::coefficient (0.02, hostRate);
    dcCoeff = (float) std::exp (-2.0 * 3.14159265358979 * 5.0 / hostRate);
    dcX[0] = dcX[1] = dcY[0] = dcY[1] = 0.0f;

    heldCount = 0;
    sustained.fill (false);
    monoNote = -1;
    firstPatch = true;
    setPatch (patch);
}

// Everything that runs at the oversampled rate. Also called when the quality setting changes,
// which restarts the voices.
void Engine::prepareInternalRate()
{
    internalRate = hostRate * oversampling;

    for (int i = 0; i < maxVoices; ++i)
        voices[(size_t) i].prepare (internalRate, i, oversampling / 2);

    decimatorL.reset();
    decimatorR.reset();
    firstStageL.reset();
    firstStageR.reset();
    hpfL.prepare (internalRate);
    hpfR.prepare (internalRate);
    lfo.setSampleRate (internalRate);
    smoothCoeff = Smoother::coefficient (0.003, internalRate);
}

void Engine::setPatch (const Patch& newPatch)
{
    patch = newPatch;

    const int newOversampling = patch.choice<Quality> (PID::Quality) == Quality::High ? 4 : 2;
    if (newOversampling != oversampling)
    {
        allNotesOff();
        oversampling = newOversampling;
        prepareInternalRate();
    }

    const auto newMode = patch.choice<VoiceMode> (PID::VoiceMode);
    const int newUnisonCount = (int) patch[PID::UnisonVoices];
    if (newMode != mode || (mode == VoiceMode::Unison && newUnisonCount != unisonCount))
    {
        allNotesOff();
        for (auto& v : voices)
        {
            v.setDetune (0.0f);
            v.setDuoOffset (0.0f);
        }
        mode = newMode;
    }
    unisonCount = std::clamp (newUnisonCount, 1, maxVoices);
    // Unison voices are mostly coherent: scale so stacking adds thickness rather than ~N x level
    stackGain = mode == VoiceMode::Unison ? 1.0f / std::sqrt ((float) unisonCount) : 1.0f;

    const bool newArpOn = patch.on (PID::ArpOn);
    if (newArpOn != arpOn)
    {
        arp.reset();
        allNotesOff();
        arpOn = newArpOn;
    }
    constexpr double stepBeats[] { 1.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125 };
    arp.setParams (patch.choice<ArpMode> (PID::ArpMode), (int) patch[PID::ArpOctaves],
                   stepBeats[std::clamp (patch.index (PID::ArpRate), 0, 5)], patch[PID::ArpGate], patch.on (PID::ArpLatch));
    updateArpClock();

    hpfL.setMode (patch.choice<HpfMode> (PID::HpfMode));
    hpfR.setMode (patch.choice<HpfMode> (PID::HpfMode));
    chorus.setMode (patch.choice<ChorusMode> (PID::ChorusMode));
    updateEffects();
    lfoRate = patch[PID::LfoRate];
    lfoWave = patch.choice<LfoWave> (PID::LfoWave);

    updateBlockParams();

    smoothers[sCutoff].target = std::log2 (patch[PID::FilterCutoff]);
    smoothers[sReso].target = patch[PID::FilterReso];
    smoothers[sDrive].target = dbToGain (patch[PID::MixerDrive]);
    smoothers[sLevel1].target = patch[PID::Osc1Level];
    smoothers[sLevel2].target = patch[PID::Osc2Level];
    smoothers[sLevel3].target = patch[PID::Osc3Level];
    smoothers[sNoise].target = patch[PID::NoiseLevel];
    smoothers[sPW1].target = patch[PID::Osc1PW];
    smoothers[sPW2].target = patch[PID::Osc2PW];
    smoothers[sPW3].target = patch[PID::Osc3PW];
    smoothers[sSub].target = patch[PID::SubLevel];
    smoothers[sRing].target = patch[PID::RingMod];
    smoothers[sShape].target = patch[PID::FilterShape];
    volume.target = dbToGain (patch[PID::MasterVolume]);

    if (firstPatch)
    {
        for (auto& s : smoothers)
            s.snap();
        volume.snap();
        firstPatch = false;
    }

    for (auto& v : voices)
        v.update (block);
}

void Engine::updateBlockParams()
{
    auto& b = block;
    b.sampleRate = internalRate;

    constexpr PID on[] { PID::Osc1On, PID::Osc2On, PID::Osc3On };
    constexpr PID wave[] { PID::Osc1Wave, PID::Osc2Wave, PID::Osc3Wave };
    constexpr PID range[] { PID::Osc1Range, PID::Osc2Range, PID::Osc3Range };
    constexpr PID tune[] { PID::Osc1Tune, PID::Osc2Tune, PID::Osc3Tune };
    for (int i = 0; i < 3; ++i)
    {
        b.oscOn[i] = patch.on (on[i]);
        b.wave[i] = patch.choice<Wave> (wave[i]);
        b.octave[i] = octaveFor (patch.choice<Range> (range[i]));
        b.tune[i] = patch[tune[i]];
    }
    b.keyTrack[0] = true;
    b.keyTrack[1] = patch.on (PID::Osc2KeyTrack);
    b.keyTrack[2] = patch.on (PID::Osc3KeyTrack);
    b.sync1 = patch.on (PID::Osc1Sync);
    b.dco = patch.choice<OscType> (PID::OscType) == OscType::Dco;
    b.phaseReset = patch.on (PID::PhaseReset);
    b.subDivide = patch.choice<SubOctave> (PID::SubOctave) == SubOctave::Down2 ? 4.0 : 2.0;
    b.crossMod = patch[PID::CrossMod];
    b.noiseColour = patch.choice<NoiseColour> (PID::NoiseColour);

    // Wheel mod: the panel amount plus the mod wheel
    const float modAmount = std::clamp (patch[PID::ModAmount] + wheel, 0.0f, 1.0f);
    b.wheelSource = patch.choice<ModSource> (PID::ModSource);
    b.modMix = patch[PID::ModMix];
    b.modPitchSemis = patch.on (PID::ModToPitch) ? modAmount * modAmount * 12.0f : 0.0f;
    b.modFilterOct = patch.on (PID::ModToFilter) ? modAmount * 4.0f : 0.0f;
    b.modPW = patch.on (PID::ModToPW) ? modAmount * 0.45f : 0.0f;

    // Poly-Mod. Pitch depths are squared so small amounts stay controllable (48 st at full).
    const float pmEnv = patch[PID::PmFilterEnv], pmOsc = patch[PID::PmOsc2];
    const bool pmFreq = patch.on (PID::PmToFreq), pmPW = patch.on (PID::PmToPW), pmFilter = patch.on (PID::PmToFilter);
    b.pmFenvSemis = pmFreq ? pmEnv * std::abs (pmEnv) * 48.0f : 0.0f;
    b.pmOsc2Semis = pmFreq ? pmOsc * pmOsc * 48.0f : 0.0f;
    b.pmFenvPW = pmPW ? pmEnv * 0.45f : 0.0f;
    b.pmOsc2PW = pmPW ? pmOsc * 0.45f : 0.0f;
    b.pmFenvOct = pmFilter ? pmEnv * 6.0f : 0.0f;
    b.pmOsc2Oct = pmFilter ? pmOsc * 4.0f : 0.0f;

    // Mod matrix: keep active slots only, amounts scaled into destination units
    constexpr PID mxSource[] { PID::Mx1Source, PID::Mx2Source, PID::Mx3Source, PID::Mx4Source };
    constexpr PID mxDest[] { PID::Mx1Dest, PID::Mx2Dest, PID::Mx3Dest, PID::Mx4Dest };
    constexpr PID mxAmount[] { PID::Mx1Amount, PID::Mx2Amount, PID::Mx3Amount, PID::Mx4Amount };
    b.numMatrix = 0;
    for (int i = 0; i < numMatrixSlots; ++i)
    {
        const auto source = patch.choice<MatrixSource> (mxSource[i]);
        const float amount = patch[mxAmount[i]];
        if (source == MatrixSource::Off || amount == 0.0f)
            continue;

        const auto dest = patch.choice<MatrixDest> (mxDest[i]);
        float depth = amount;
        switch (dest)
        {
            case MatrixDest::Pitch:
            case MatrixDest::Osc1Pitch:
            case MatrixDest::Osc2Pitch:
            case MatrixDest::Osc3Pitch: depth = amount * std::abs (amount) * 24.0f; break;
            case MatrixDest::Osc1PW:
            case MatrixDest::Osc2PW: depth = amount * 0.45f; break;
            case MatrixDest::Cutoff: depth = amount * 6.0f; break;
            default: break;
        }
        b.matrix[b.numMatrix++] = { source, dest, depth };
    }
    b.modWheel = wheel;
    b.aftertouch = pressure;
    b.timbre = globalTimbre;

    b.osc2AsSource = b.sync1 || b.crossMod > 0.0f || b.pmOsc2Semis != 0.0f || b.pmOsc2PW != 0.0f || b.pmOsc2Oct != 0.0f
                     || patch[PID::RingMod] > 0.0f || mode == VoiceMode::Duo;
    const bool wheelUsesOsc3 = b.wheelSource == ModSource::Osc3 && b.modMix < 1.0f
                               && (b.modPitchSemis != 0.0f || b.modFilterOct != 0.0f || b.modPW != 0.0f);
    b.osc3AsSource = wheelUsesOsc3 || std::any_of (b.matrix, b.matrix + b.numMatrix, [] (const auto& m) { return m.source == MatrixSource::Osc3; });

    b.lfoDelay = patch[PID::LfoDelay];
    b.lfo2Rate = patch[PID::Lfo2Rate];
    b.lfo2Wave = patch.choice<LfoWave> (PID::Lfo2Wave);
    b.lfo2Delay = patch[PID::Lfo2Delay];
    b.lfo2KeySync = patch.on (PID::Lfo2KeySync);
    b.lfo2Used = std::any_of (b.matrix, b.matrix + b.numMatrix, [] (const auto& m) { return m.source == MatrixSource::Lfo2; });

    const auto& offsets = tuningOffsets (patch.choice<Tuning> (PID::Tuning));
    const int root = std::clamp (patch.index (PID::TuningRoot), 0, 11);
    for (int i = 0; i < 12; ++i)
        b.tuning[i] = offsets[(size_t) ((i - root + 12) % 12)];
    b.mpe = patch.on (PID::MpeMode);
    b.mpeBendRange = patch[PID::MpeBendRange];

    constexpr float keyTrackAmounts[] { 0.0f, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f };
    b.filterModel = patch.choice<FilterModel> (PID::FilterModel);
    b.envAmountOct = patch[PID::FilterEnvAmt] * 7.0f;
    b.keyTrackFilter = keyTrackAmounts[std::clamp (patch.index (PID::FilterKeyTrack), 0, 3)];
    b.velFilterOct = patch[PID::FilterVel] * 3.0f;
    b.resComp = patch[PID::FilterResComp];
    b.filterBandpass = patch.on (PID::FilterBandpass);
    b.voiceHpfCutoff = patch[PID::VoiceHpfCutoff];
    b.voiceHpfReso = patch[PID::VoiceHpfReso];

    const bool releaseUsesDecay = patch.choice<ReleaseMode> (PID::EnvReleaseMode) == ReleaseMode::UseDecay;
    b.fenv[0] = patch[PID::FEnvA];
    b.fenv[1] = patch[PID::FEnvD];
    b.fenv[2] = patch[PID::FEnvS];
    b.fenv[3] = releaseUsesDecay ? patch[PID::FEnvD] : patch[PID::FEnvR];
    b.aenv[0] = patch[PID::AEnvA];
    b.aenv[1] = patch[PID::AEnvD];
    b.aenv[2] = patch[PID::AEnvS];
    b.aenv[3] = releaseUsesDecay ? patch[PID::AEnvD] : patch[PID::AEnvR];
    b.linearEnv = patch.choice<EnvCurve> (PID::EnvCurve) == EnvCurve::Linear;
    b.ampVel = patch[PID::AmpVel];
    b.vcaGate = patch.choice<VcaMode> (PID::VcaMode) == VcaMode::Gate;

    b.glideType = patch.choice<GlideType> (PID::GlideType);
    b.glideTime = patch[PID::GlideTime];
    b.pitchOffset = bend * patch[PID::BendRange] + patch[PID::MasterTune] * 0.01f;

    b.drift = patch[PID::Drift];
    b.tolerance = patch[PID::Tolerance];
}

void Engine::setTransport (double bpm, bool playing, double ppqPosition)
{
    hostBpm = bpm;
    hostPlaying = playing;
    hostPpq = ppqPosition;
    updateArpClock();
    updateEffects();
}

void Engine::updateEffects()
{
    // Synced delay follows the host tempo, or the internal (arp) tempo without one
    constexpr double beats[] { 0.125, 1.0 / 6.0, 0.25, 0.375, 1.0 / 3.0, 0.5, 0.75, 2.0 / 3.0, 1.0, 1.5, 2.0, 4.0 };
    const double bpm = hostBpm > 0.0 ? hostBpm : (double) patch[PID::ArpTempo];
    const double seconds = patch.on (PID::DelaySync)
                               ? beats[std::clamp (patch.index (PID::DelayNote), 0, 11)] * 60.0 / bpm
                               : patch[PID::DelayTime] * 0.001;
    delay.setParameters (patch.choice<DelayMode> (PID::DelayMode), seconds, patch[PID::DelayFeedback],
                         patch[PID::DelayTone], patch[PID::DelayMix]);

    reverb.setParameters (patch[PID::ReverbSize], patch[PID::ReverbDecay], patch[PID::ReverbDamping], patch[PID::ReverbPredelay]);
    reverb.setMix (patch[PID::ReverbMix]);
}

void Engine::updateArpClock()
{
    const bool sync = patch.on (PID::ArpSync);
    const double bpm = sync && hostBpm > 0.0 ? hostBpm : (double) patch[PID::ArpTempo];
    arp.setClock (bpm, sync && hostPlaying, hostPpq);
}

float Engine::shapeVelocity (float velocity) const
{
    const float v = std::clamp (velocity, 0.0f, 1.0f);
    switch (patch.choice<VelocityCurve> (PID::VelocityCurve))
    {
        case VelocityCurve::Linear: return v;
        case VelocityCurve::Soft: return std::sqrt (v);   // light playing already reaches high values
        case VelocityCurve::Hard: return v * v;           // needs a firm touch
        case VelocityCurve::SCurve: return v * v * (3.0f - 2.0f * v);
        case VelocityCurve::Fixed: return 1.0f;
    }
    return v;
}

void Engine::noteOn (int note, float velocity, int channel)
{
    velocity = shapeVelocity (velocity);
    if (arpOn)
        arp.keyDown (note, velocity);
    else
        voiceNoteOn (note, velocity, channel);
}

void Engine::noteOff (int note, int)
{
    if (arpOn)
        arp.keyUp (note);
    else
        voiceNoteOff (note);
}

void Engine::voiceNoteOn (int note, float velocity, int channel)
{
    if (note < 0 || note > 127)
        return;
    heldChannel[(size_t) note] = std::clamp (channel, 0, 15);
    notePressure[(size_t) note] = 0.0f;

    // Ignore a duplicate note-on for a held key, but keep its position fresh for priority
    auto end = held.begin() + heldCount;
    if (auto it = std::find (held.begin(), end, note); it != end)
    {
        std::rotate (it, it + 1, end);
        --heldCount;
    }
    if (heldCount == 0 && patch.on (PID::LfoKeySync))
        lfo.reset();
    held[(size_t) heldCount++] = note;
    sustained[(size_t) note] = false;
    lastVelocity = velocity;

    if (mode == VoiceMode::Poly)
    {
        const bool otherKeysHeld = heldCount > 1;
        const bool glide = patch.choice<GlideMode> (PID::GlideMode) == GlideMode::Always || otherKeysHeld;
        auto& v = allocatePolyVoice (note);
        // Fixed per-voice positions, alternating sides from the outside in (Jupiter-8 style)
        constexpr float positions[maxVoices] { -1.0f, 1.0f, -0.71f, 0.71f, -0.43f, 0.43f, -0.14f, 0.14f };
        v.setPan (positions[&v - voices.data()] * patch[PID::PanSpread]);
        v.start (note, velocity, true, glide, block, heldChannel[(size_t) note]);
        v.setAge (++ageCounter);
    }
    else
    {
        monoUpdate (true, velocity);
    }
}

void Engine::voiceNoteOff (int note)
{
    if (note < 0 || note > 127)
        return;

    auto end = held.begin() + heldCount;
    auto it = std::find (held.begin(), end, note);
    if (it == end)
        return;
    std::rotate (it, it + 1, end);
    --heldCount;

    if (mode == VoiceMode::Poly)
    {
        if (sustainDown)
            sustained[(size_t) note] = true;
        else
            releaseNote (note);
    }
    else
    {
        monoUpdate (false, lastVelocity);
    }
}

int Engine::pickMonoNote() const
{
    if (heldCount == 0)
        return -1;

    if (mode == VoiceMode::Duo)
        return *std::min_element (held.begin(), held.begin() + heldCount);

    const auto begin = held.begin();
    const auto end = begin + heldCount;
    switch (patch.choice<NotePriority> (PID::NotePriority))
    {
        case NotePriority::Last: return held[(size_t) heldCount - 1];
        case NotePriority::Low: return *std::min_element (begin, end);
        case NotePriority::High: return *std::max_element (begin, end);
    }
    return held[(size_t) heldCount - 1];
}

void Engine::monoUpdate (bool newKeyPressed, float velocity)
{
    // Mono and Legato drive voice 0; Unison drives the first unisonCount voices as one.
    const int count = monoVoiceCount();
    const int target = pickMonoNote();

    if (target < 0)
    {
        if (! sustainDown)
        {
            for (int i = 0; i < count; ++i)
                voices[(size_t) i].release();
            monoNote = -1;
        }
        return;
    }

    // Duo (ARP Odyssey): osc 1 takes the lowest key, osc 2 the highest
    if (mode == VoiceMode::Duo)
    {
        const auto end = held.begin() + heldCount;
        const int highest = *std::max_element (held.begin(), end);
        voices[0].setDuoOffset ((float) (highest - target) + block.tuning[highest % 12] - block.tuning[target % 12]);
    }

    const bool legato = voices[0].isGateOn();
    if (legato && target == monoNote)
        return; // e.g. low-note priority and a higher key went down: nothing changes

    // Mono and Unison retrigger on every new key; Legato only when coming from silence.
    // Returning to a still-held key after a release never retriggers.
    const bool retrigger = ! legato || (mode != VoiceMode::Legato && newKeyPressed);
    const bool glide = patch.choice<GlideMode> (PID::GlideMode) == GlideMode::Always || legato;

    // Unison voices spread evenly across +/- 30 cents at full detune
    const float spread = mode == VoiceMode::Unison ? patch[PID::UnisonDetune] * 0.3f : 0.0f;
    for (int i = 0; i < count; ++i)
    {
        auto& v = voices[(size_t) i];
        const float position = count > 1 ? 2.0f * (float) i / (float) (count - 1) - 1.0f : 0.0f;
        v.setDetune (spread * position);
        v.setPan (position * patch[PID::PanSpread]);
        v.start (target, velocity, retrigger, glide, block, heldChannel[(size_t) target]);
        v.setAge (++ageCounter);
    }
    monoNote = target;
}

Voice& Engine::allocatePolyVoice (int note)
{
    // Same note still sounding: reuse its voice
    for (auto& v : voices)
        if (v.getNote() == note && ! v.isFree())
            return v;

    auto oldest = [this] (auto&& predicate) -> Voice*
    {
        Voice* best = nullptr;
        for (auto& v : voices)
            if (predicate (v) && (best == nullptr || v.getAge() < best->getAge()))
                best = &v;
        return best;
    };

    switch (patch.choice<KeyAssign> (PID::KeyAssign))
    {
        case KeyAssign::Poly1:
            // Jupiter-8 Poly 1: the voice that has been silent longest, so release tails ring on
            if (auto* v = oldest ([] (const Voice& candidate) { return candidate.isFree(); }))
                return *v;
            if (auto* v = oldest ([] (const Voice& candidate) { return ! candidate.isGateOn(); }))
                return *v;
            break;

        case KeyAssign::Poly2:
            // Jupiter-8 Poly 2: always the lowest-numbered released voice, cutting its release short
            for (auto& v : voices)
                if (! v.isGateOn())
                    return v;
            break;

        case KeyAssign::Rotate:
            // Juno / OB-style cycling: every note moves on to the next released voice in turn, so
            // repeated notes pass through voices with different component tolerances
            for (int i = 0; i < maxVoices; ++i)
            {
                auto& v = voices[(size_t) ((rotateNext + i) % maxVoices)];
                if (! v.isGateOn())
                {
                    rotateNext = (int) (&v - voices.data() + 1) % maxVoices;
                    return v;
                }
            }
            break;
    }
    return *oldest ([] (const Voice&) { return true; });
}

void Engine::releaseNote (int note)
{
    for (auto& v : voices)
        if (v.isGateOn() && v.getNote() == note)
            v.release();
}

void Engine::pitchBend (float normalised, int channel)
{
    if (block.mpe && channel > 0 && channel < 16)
    {
        channels[(size_t) channel].bend = std::clamp (normalised, -1.0f, 1.0f);
        return;
    }
    bend = std::clamp (normalised, -1.0f, 1.0f);
    block.pitchOffset = bend * patch[PID::BendRange] + patch[PID::MasterTune] * 0.01f;
}

void Engine::modWheel (float normalised)
{
    wheel = std::clamp (normalised, 0.0f, 1.0f);
    updateBlockParams();
}

void Engine::aftertouch (float normalised, int channel)
{
    if (block.mpe && channel > 0 && channel < 16)
    {
        channels[(size_t) channel].pressure = std::clamp (normalised, 0.0f, 1.0f);
        return;
    }
    pressure = std::clamp (normalised, 0.0f, 1.0f);
    block.aftertouch = pressure;
}

void Engine::polyPressure (int note, float normalised)
{
    if (note >= 0 && note < 128)
        notePressure[(size_t) note] = std::clamp (normalised, 0.0f, 1.0f);
}

void Engine::timbre (float normalised, int channel)
{
    if (block.mpe && channel > 0 && channel < 16)
    {
        channels[(size_t) channel].timbre = std::clamp (normalised, 0.0f, 1.0f);
        return;
    }
    globalTimbre = std::clamp (normalised, 0.0f, 1.0f);
    block.timbre = globalTimbre;
}

void Engine::sustainPedal (bool down)
{
    sustainDown = down;
    if (down)
        return;

    if (mode == VoiceMode::Poly)
    {
        for (int n = 0; n < 128; ++n)
            if (sustained[(size_t) n])
            {
                sustained[(size_t) n] = false;
                releaseNote (n);
            }
    }
    else if (heldCount == 0)
    {
        for (int i = 0; i < monoVoiceCount(); ++i)
            voices[(size_t) i].release();
        monoNote = -1;
    }
}

void Engine::allNotesOff()
{
    arp.reset();
    notePressure.fill (0.0f);
    heldCount = 0;
    sustained.fill (false);
    monoNote = -1;
    for (auto& v : voices)
        v.release();
}

int Engine::numActiveVoices() const
{
    return (int) std::count_if (voices.begin(), voices.end(), [] (const Voice& v) { return ! v.isFree(); });
}

void Engine::process (float* left, float* right, int n)
{
    if (! arpOn)
    {
        render (left, right, n);
        return;
    }

    // Render up to each arpeggiator event, then apply it, so arp timing is sample-accurate
    for (int pos = 0; pos < n;)
    {
        const int k = arp.samplesUntilNextEvent (n - pos);
        if (k > 0)
        {
            render (left + pos, right + pos, k);
            arp.advance (k);
            pos += k;
        }

        Arpeggiator::Event events[3];
        const int count = arp.popEvents (events);
        for (int e = 0; e < count; ++e)
        {
            if (events[e].on)
                voiceNoteOn (events[e].note, events[e].velocity);
            else
                voiceNoteOff (events[e].note);
        }

        if (k == 0 && count == 0)
            break; // can't happen, but never spin
    }
}

void Engine::render (float* left, float* right, int n)
{
    ScopedFlushDenormals noDenormals;

    // Refreshed here rather than stored once, so a copied Engine never points into another
    block.channels = channels.data();
    block.notePressure = notePressure.data();

    for (int offset = 0; offset < n; offset += chunk)
    {
        const int m = std::min (chunk, n - offset);
        const int mi = m * oversampling;

        for (int i = 0; i < mi; ++i)
        {
            auto& f = frames[(size_t) i];
            f.cutoffOct = smoothers[sCutoff].next (smoothCoeff);
            f.resonance = smoothers[sReso].next (smoothCoeff);
            f.inputGain = smoothers[sDrive].next (smoothCoeff);
            f.level[0] = smoothers[sLevel1].next (smoothCoeff);
            f.level[1] = smoothers[sLevel2].next (smoothCoeff);
            f.level[2] = smoothers[sLevel3].next (smoothCoeff);
            f.noise = smoothers[sNoise].next (smoothCoeff);
            f.pw[0] = smoothers[sPW1].next (smoothCoeff);
            f.pw[1] = smoothers[sPW2].next (smoothCoeff);
            f.pw[2] = smoothers[sPW3].next (smoothCoeff);
            f.sub = smoothers[sSub].next (smoothCoeff);
            f.ring = smoothers[sRing].next (smoothCoeff);
            f.shape = smoothers[sShape].next (smoothCoeff);
            f.lfo = lfo.next (lfoRate, lfoWave);
        }

        std::fill_n (bufferL.begin(), mi, 0.0f);
        std::fill_n (bufferR.begin(), mi, 0.0f);
        for (auto& v : voices)
            if (v.isActive())
                v.render (bufferL.data(), bufferR.data(), mi, frames.data(), block);

        float* outL = left + offset;
        float* outR = right + offset;
        // Halfband stages down to the host rate (HQ: 4x -> 2x -> 1x)
        auto decimate = [this] (float* x, size_t at, JunoHpf& hpf, auto& first, HalfbandDecimator& last)
        {
            if (oversampling == 2)
                return last.process (hpf.process (x[at]), hpf.process (x[at + 1]));
            const float a = first.process (hpf.process (x[at]), hpf.process (x[at + 1]));
            const float b = first.process (hpf.process (x[at + 2]), hpf.process (x[at + 3]));
            return last.process (a, b);
        };

        for (int i = 0; i < m; ++i)
        {
            const size_t at = (size_t) (oversampling * i);
            float y[2] { decimate (bufferL.data(), at, hpfL, firstStageL, decimatorL),
                         decimate (bufferR.data(), at, hpfR, firstStageR, decimatorR) };
            for (int c = 0; c < 2; ++c)
            {
                const float hp = y[c] - dcX[c] + dcCoeff * dcY[c];
                dcX[c] = y[c];
                dcY[c] = hp;
                y[c] = hp;
            }
            outL[i] = y[0] * stackGain;
            outR[i] = y[1] * stackGain;
        }

        chorus.process (outL, outR, m);
        delay.process (outL, outR, m);
        reverb.process (outL, outR, m);

        for (int i = 0; i < m; ++i)
        {
            const float gain = volume.next (hostSmoothCoeff);
            outL[i] *= gain;
            outR[i] *= gain;
        }
    }
}

} // namespace dsynth
