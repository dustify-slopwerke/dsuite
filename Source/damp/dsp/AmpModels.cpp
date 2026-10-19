#include "AmpModels.h"

#include <mutex>

namespace damp
{
namespace
{
constexpr double k = 1.0e3, M = 1.0e6, nF = 1.0e-9, pF = 1.0e-12, uF = 1.0e-6;

StageSpec stage (double plateR, double cathodeR, double bypass, double supply, double couplingHz, double lowpassHz,
                 double outputScale = 1.0, double gridLeakSeconds = 0.05)
{
    StageSpec s;
    s.circuit = { plateR, cathodeR, supply, bypass > 0.0 };
    s.bypassCap = bypass;
    s.couplingHz = couplingHz;
    s.lowpassHz = lowpassHz;
    s.outputScale = outputScale;
    s.gridLeakSeconds = gridLeakSeconds;
    return s;
}

std::array<ModelSpec, (size_t) numModels> buildSpecs()
{
    std::array<ModelSpec, (size_t) numModels> m;

    // Clean: blackface-style. The tone stack hangs straight off the first plate, the volume pot
    // follows it, then a second stage into a 6L6 pair with plenty of feedback: loud and clean
    // until the volume is well up.
    {
        auto& s = m[(size_t) AmpModel::Clean];
        s.name = "Clean";
        s.numStages = 2;
        s.stages[0] = stage (100 * k, 1.5 * k, 25 * uF, 270.0, 10.0, 18000.0);
        s.stages[1] = stage (100 * k, 1.5 * k, 25 * uF, 270.0, 15.0, 16000.0);
        s.toneStackAfter = 0;
        s.gainPotAfter = 0;
        s.brightHz = 1.0 / (2.0 * 3.14159265 * 1 * M * 120 * pF);
        s.toneStack = { 250 * k, 250 * k, 10 * k, 100 * k, 250 * pF, 100 * nF, 47 * nF };
        s.power = { .drive = 0.09, .openLoopGain = 5.0, .feedback = 0.8, .idle = 0.25, .mismatch = 0.03, .sag = 0.2,
                    .sagSeconds = 0.06, .presenceHz = 2500.0, .resonanceHz = 150.0, .bandwidthHz = 18000.0 };
        s.outputTrim = 2.5;
    }

    // Chime: AC-style top boost. Gain stage, volume, a second stage, the treble / bass network,
    // then cathode-biased EL84s with no feedback and a tube rectifier that sags.
    {
        auto& s = m[(size_t) AmpModel::Chime];
        s.name = "Chime";
        s.numStages = 2;
        s.stages[0] = stage (220 * k, 1.5 * k, 25 * uF, 280.0, 10.0, 15000.0, 0.2);
        s.stages[1] = stage (220 * k, 1.5 * k, 25 * uF, 280.0, 20.0, 14000.0, 0.7);
        s.gainPotAfter = 0;
        s.toneStackAfter = 1;
        s.brightHz = 1.0 / (2.0 * 3.14159265 * 1 * M * 47 * pF);
        s.toneStack = { 1 * M, 1 * M, 10 * k, 100 * k, 50 * pF, 22 * nF, 22 * nF };
        s.power = { .drive = 0.08, .openLoopGain = 1.0, .feedback = 0.0, .idle = 0.8, .mismatch = 0.06, .sag = 0.35,
                    .sagSeconds = 0.12, .presenceHz = 2500.0, .resonanceHz = 100.0, .bandwidthHz = 14000.0 };
        s.outputTrim = 3.2;
    }

    // Crunch: plexi / 800-style. A partly bypassed first stage (lean bass), a bright cap on the
    // gain pot, a cold unbypassed second stage that clips asymmetrically, the tone stack after a
    // cathode follower, EL34s with feedback and presence.
    {
        auto& s = m[(size_t) AmpModel::Crunch];
        s.name = "Crunch";
        s.numStages = 2;
        s.stages[0] = stage (100 * k, 2.7 * k, 0.68 * uF, 300.0, 15.0, 17000.0);
        s.stages[1] = stage (100 * k, 10 * k, 0.47 * uF, 300.0, 30.0, 12000.0);
        s.gainPotAfter = 0;
        s.toneStackAfter = 1;
        s.brightHz = 1.0 / (2.0 * 3.14159265 * 1 * M * 470 * pF);
        s.toneStack = { 220 * k, 1 * M, 22 * k, 33 * k, 470 * pF, 22 * nF, 22 * nF };
        s.power = { .drive = 0.06, .openLoopGain = 4.0, .feedback = 0.6, .idle = 0.2, .mismatch = 0.04, .sag = 0.25,
                    .sagSeconds = 0.08, .presenceHz = 2200.0, .resonanceHz = 150.0, .bandwidthHz = 16000.0 };
        s.outputTrim = 1.7;
    }

    // High gain: four cascaded stages with small coupling caps (tight lows), cold clippers and
    // plate caps taming the fizz, then 6L6s with a solid-state rectifier (little sag) and a deep
    // resonance control.
    {
        auto& s = m[(size_t) AmpModel::HighGain];
        s.name = "High Gain";
        s.numStages = 4;
        s.stages[0] = stage (100 * k, 1.8 * k, 1 * uF, 320.0, 30.0, 17000.0, 1.0);
        s.stages[1] = stage (100 * k, 10 * k, 0.47 * uF, 320.0, 40.0, 14000.0, 0.1, 0.03);
        s.stages[2] = stage (100 * k, 1.5 * k, 2.2 * uF, 320.0, 40.0, 12000.0, 0.22, 0.03);
        s.stages[3] = stage (100 * k, 10 * k, 0.0, 320.0, 40.0, 11000.0, 1.0, 0.03);
        s.gainPotAfter = 0;
        s.toneStackAfter = 3;
        s.brightHz = 1.0 / (2.0 * 3.14159265 * 1 * M * 180 * pF);
        s.toneStack = { 250 * k, 1 * M, 25 * k, 47 * k, 470 * pF, 20 * nF, 20 * nF };
        s.power = { .drive = 0.09, .openLoopGain = 5.0, .feedback = 0.8, .idle = 0.2, .mismatch = 0.03, .sag = 0.12,
                    .sagSeconds = 0.05, .presenceHz = 2200.0, .resonanceHz = 160.0, .bandwidthHz = 15000.0 };
        s.outputTrim = 0.7;
    }

    // Modern high gain: rectifier-style. A hotter input (more saturation and compression at soft
    // picking), a second stage with more headroom and a quicker grid leak so hard palm mutes don't
    // block, and a cold unbypassed clipper last whose plate cap rolls the fizz off early (darker
    // and smoother than High Gain). A Boogie-style stack (more bass, a deep scoop
    // near 1 kHz with the middle down), then 6L6s with deep feedback (so resonance has more to
    // take out) and a tube rectifier's sag: fatter, looser lows and more compression.
    {
        auto& s = m[(size_t) AmpModel::ModernHighGain];
        s.name = "Modern High Gain";
        s.numStages = 4;
        s.inputVolts = 1.6;
        s.stages[0] = stage (100 * k, 1.5 * k, 2.2 * uF, 330.0, 20.0, 16000.0, 1.0);
        s.stages[1] = stage (100 * k, 4.7 * k, 0.47 * uF, 330.0, 25.0, 13000.0, 0.09, 0.02);
        s.stages[2] = stage (100 * k, 1.5 * k, 2.2 * uF, 330.0, 25.0, 10000.0, 0.2, 0.03);
        s.stages[3] = stage (100 * k, 10 * k, 0.0, 330.0, 25.0, 4500.0, 1.0, 0.03);
        s.gainPotAfter = 0;
        s.toneStackAfter = 3;
        s.brightHz = 1.0 / (2.0 * 3.14159265 * 1 * M * 120 * pF);
        s.toneStack = { 250 * k, 1 * M, 20 * k, 33 * k, 330 * pF, 47 * nF, 20 * nF };
        s.power = { .drive = 0.09, .openLoopGain = 10.0, .feedback = 0.85, .idle = 0.25, .mismatch = 0.03, .sag = 0.28,
                    .sagSeconds = 0.1, .presenceHz = 2200.0, .resonanceHz = 140.0, .bandwidthHz = 11000.0 };
        s.outputTrim = 0.7;
    }

    return m;
}

const std::array<ModelSpec, (size_t) numModels>& specs()
{
    static const auto all = buildSpecs();
    return all;
}
} // namespace

const ModelSpec& modelSpec (AmpModel model)
{
    return specs()[(size_t) model];
}

const ModelCurves& modelCurves (AmpModel model)
{
    static std::array<ModelCurves, (size_t) numModels> curves;
    static std::once_flag once;
    std::call_once (once, []
    {
        for (int i = 0; i < numModels; ++i)
        {
            const auto& spec = specs()[(size_t) i];
            for (int st = 0; st < spec.numStages; ++st)
            {
                auto circuit = spec.stages[(size_t) st].circuit;
                curves[(size_t) i].stage[(size_t) st].build (circuit);
                circuit.cathodeBypassed = false;
                curves[(size_t) i].unbypassed[(size_t) st].build (circuit);
            }
        }
    });
    return curves[(size_t) model];
}

} // namespace damp
