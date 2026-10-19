#pragma once

#include "dsp/Params.h"

#include <cmath>

namespace dsynth
{

// Whether a parameter currently affects the sound, given the rest of the patch. The editor dims
// controls that don't (they stay adjustable), so the ones that matter stand out.
#if defined(__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wswitch-enum" // only parameters with a rule are listed
#endif
inline bool isRelevant (PID pid, const Patch& p)
{
    auto pulseIn = [&] (PID wave)
    {
        const auto w = p.choice<Wave> (wave);
        return w == Wave::Pulse || w == Wave::SawPulse || w == Wave::TriPulse;
    };
    const auto voiceMode = p.choice<VoiceMode> (PID::VoiceMode);
    const auto model = p.choice<FilterModel> (PID::FilterModel);
    const bool delayOn = p.choice<DelayMode> (PID::DelayMode) != DelayMode::Off;
    const bool arpOn = p.on (PID::ArpOn);
    const auto layerMode = p.choice<LayerMode> (PID::LayerMode);
    auto matrixUses = [&] (MatrixSource source)
    {
        constexpr PID slots[][2] { { PID::Mx1Source, PID::Mx1Amount }, { PID::Mx2Source, PID::Mx2Amount },
                                   { PID::Mx3Source, PID::Mx3Amount }, { PID::Mx4Source, PID::Mx4Amount } };
        for (const auto& slot : slots)
            if (p.choice<MatrixSource> (slot[0]) == source && std::abs (p[slot[1]]) > 0.0f)
                return true;
        return false;
    };

    switch (pid)
    {
        case PID::Osc1PW: return pulseIn (PID::Osc1Wave);
        case PID::Osc2PW: return pulseIn (PID::Osc2Wave);
        case PID::Osc3PW: return pulseIn (PID::Osc3Wave);
        case PID::SubOctave: return p[PID::SubLevel] > 0.0f;

        case PID::PmToFreq:
        case PID::PmToPW:
        case PID::PmToFilter: return std::abs (p[PID::PmFilterEnv]) > 0.0f || p[PID::PmOsc2] > 0.0f;

        case PID::Mx1Dest: case PID::Mx1Amount: return p.choice<MatrixSource> (PID::Mx1Source) != MatrixSource::Off;
        case PID::Mx2Dest: case PID::Mx2Amount: return p.choice<MatrixSource> (PID::Mx2Source) != MatrixSource::Off;
        case PID::Mx3Dest: case PID::Mx3Amount: return p.choice<MatrixSource> (PID::Mx3Source) != MatrixSource::Off;
        case PID::Mx4Dest: case PID::Mx4Amount: return p.choice<MatrixSource> (PID::Mx4Source) != MatrixSource::Off;

        case PID::FilterShape: return model == FilterModel::Sem12 && ! p.on (PID::FilterBandpass);
        case PID::FilterBandpass: return model == FilterModel::Sem12;
        case PID::FilterResComp: return model != FilterModel::Sem12 && model != FilterModel::Ms20;
        case PID::VoiceHpfReso: return p[PID::VoiceHpfCutoff] > 10.5f;

        // Gate VCA ignores the amp envelope; "Use Decay" replaces both release times
        case PID::AEnvA: case PID::AEnvD: case PID::AEnvS: return p.choice<VcaMode> (PID::VcaMode) == VcaMode::Envelope;
        case PID::AEnvR:
            return p.choice<VcaMode> (PID::VcaMode) == VcaMode::Envelope
                   && p.choice<ReleaseMode> (PID::EnvReleaseMode) == ReleaseMode::Independent;
        case PID::FEnvR: return p.choice<ReleaseMode> (PID::EnvReleaseMode) == ReleaseMode::Independent;

        case PID::NotePriority: return voiceMode == VoiceMode::Mono || voiceMode == VoiceMode::Legato || voiceMode == VoiceMode::Unison;
        case PID::GlideType: case PID::GlideMode: return p[PID::GlideTime] > 0.0f;
        case PID::UnisonVoices: case PID::UnisonDetune: return voiceMode == VoiceMode::Unison;
        case PID::KeyAssign: return voiceMode == VoiceMode::Poly;

        // LFO 2 only reaches the sound through the mod matrix
        case PID::Lfo2Rate: case PID::Lfo2Wave: case PID::Lfo2Delay: case PID::Lfo2KeySync: return matrixUses (MatrixSource::Lfo2);
        case PID::TuningRoot: return p.choice<Tuning> (PID::Tuning) != Tuning::Equal;

        case PID::ArpMode: case PID::ArpRate: case PID::ArpOctaves: case PID::ArpGate: case PID::ArpLatch: case PID::ArpSync:
            return arpOn;

        case PID::DelaySync: case PID::DelayFeedback: case PID::DelayTone: case PID::DelayMix: return delayOn;
        case PID::DelayNote: return delayOn && p.on (PID::DelaySync);
        case PID::DelayTime: return delayOn && ! p.on (PID::DelaySync);

        case PID::ReverbSize: case PID::ReverbDecay: case PID::ReverbDamping: case PID::ReverbPredelay:
            return p[PID::ReverbMix] > 0.0f;

        case PID::SplitNote: return layerMode == LayerMode::Split;
        case PID::LayerBalance: case PID::LayerDetune: return layerMode != LayerMode::Single;
        case PID::MpeBendRange: return p.on (PID::MpeMode);

        default: return true;
    }
}
#if defined(__GNUC__)
 #pragma GCC diagnostic pop
#endif

} // namespace dsynth
