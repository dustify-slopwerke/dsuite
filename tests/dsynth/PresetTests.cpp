#include "common/dsp/Noise.h"
#include "dsp/Preset.h"
#include "ui/Relevance.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <set>

using namespace dsynth;

TEST_CASE ("Parameter table is consistent")
{
    std::set<std::string_view> ids;
    for (const auto& info : allParams())
    {
        INFO (info.id);
        CHECK (ids.insert (info.id).second);
        CHECK (info.min < info.max);
        CHECK (info.def >= info.min);
        CHECK (info.def <= info.max);
        if (info.type == ParamType::Choice)
            CHECK (info.choices.size() == (size_t) info.max + 1);
        if (info.centre > 0.0f)
            CHECK ((info.centre > info.min && info.centre < info.max));
    }
}

TEST_CASE ("Presets round-trip through text")
{
    Random rng (99);
    Patch p;
    for (const auto& info : allParams())
    {
        const float u = (float) rng.unipolar();
        p[info.pid] = info.type == ParamType::Float ? info.min + u * (info.max - info.min) : std::round (u * info.max);
    }

    const auto parsed = parsePreset (serialisePreset (p, "Round Trip"));
    CHECK (parsed.warnings.empty());
    CHECK (parsed.name == "Round Trip");
    for (const auto& info : allParams())
    {
        INFO (info.id);
        CHECK (parsed.patch[info.pid] == p[info.pid]);
    }
}

TEST_CASE ("Preset parsing is forgiving")
{
    const auto parsed = parsePreset ("# comment\n"
                                     "name = Test\n"
                                     "osc1_wave = pulse   # case-insensitive choice\n"
                                     "filter_cutoff = 99999\n"
                                     "osc2_on = on\n"
                                     "no_such_param = 1\n"
                                     "osc1_level = loud\n");
    CHECK (parsed.name == "Test");
    CHECK (parsed.patch.choice<Wave> (PID::Osc1Wave) == Wave::Pulse);
    CHECK (parsed.patch[PID::FilterCutoff] == 20000.0f); // clamped
    CHECK (parsed.patch.on (PID::Osc2On));
    CHECK (parsed.patch[PID::Osc1Level] == paramInfo (PID::Osc1Level).def);
    CHECK (parsed.warnings.size() == 2);
}

TEST_CASE ("Factory presets all parse cleanly")
{
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator (DSYNTH_PRESET_DIR))
    {
        if (entry.path().extension() != ".dsynth")
            continue;
        std::ifstream f (entry.path());
        const std::string text { std::istreambuf_iterator<char> (f), {} };
        const auto parsed = parsePreset (text);
        INFO (entry.path().filename().string());
        CHECK (parsed.warnings.empty());
        CHECK_FALSE (parsed.name.empty());
        ++count;
    }
    CHECK (count >= 6);
}

TEST_CASE ("Two-layer presets round-trip; layer setup always lives in layer A")
{
    auto a = Patch::defaults();
    auto b = Patch::defaults();
    a[PID::LayerMode] = (float) LayerMode::Split;
    a[PID::SplitNote] = 55.0f;
    a[PID::FilterCutoff] = 500.0f;
    b[PID::FilterCutoff] = 3000.0f;
    b[PID::Osc1Wave] = (float) Wave::Pulse;

    const auto text = serialisePreset (a, "Layered", &b);
    const auto parsed = parsePreset (text);
    CHECK (parsed.warnings.empty());
    REQUIRE (parsed.hasLayerB);
    CHECK (parsed.patch[PID::FilterCutoff] == 500.0f);
    CHECK (parsed.layerB[PID::FilterCutoff] == 3000.0f);
    CHECK (parsed.layerB.choice<Wave> (PID::Osc1Wave) == Wave::Pulse);
    CHECK (parsed.patch.choice<LayerMode> (PID::LayerMode) == LayerMode::Split);
    CHECK (parsed.patch[PID::SplitNote] == 55.0f);
    CHECK (text.find ("[layer b]") != std::string::npos);

    // A layer-setup id inside the B section still sets layer A
    const auto stray = parsePreset ("filter_cutoff = 400\n[layer b]\nsplit_note = 40\nfilter_cutoff = 900\n");
    CHECK (stray.patch[PID::SplitNote] == 40.0f);
    CHECK (stray.layerB[PID::FilterCutoff] == 900.0f);
}

TEST_CASE ("Controls that don't apply are reported as not relevant")
{
    auto p = Patch::defaults();
    CHECK_FALSE (isRelevant (PID::DelayTime, p));          // delay off
    p[PID::DelayMode] = (float) DelayMode::Stereo;
    CHECK (isRelevant (PID::DelayNote, p));                 // synced by default
    CHECK_FALSE (isRelevant (PID::DelayTime, p));
    p[PID::DelaySync] = 0.0f;
    CHECK (isRelevant (PID::DelayTime, p));
    CHECK_FALSE (isRelevant (PID::DelayNote, p));

    CHECK_FALSE (isRelevant (PID::FilterShape, p));         // ladder by default
    p[PID::FilterModel] = (float) FilterModel::Sem12;
    CHECK (isRelevant (PID::FilterShape, p));
    p[PID::FilterBandpass] = 1.0f;
    CHECK_FALSE (isRelevant (PID::FilterShape, p));

    CHECK_FALSE (isRelevant (PID::Osc1PW, p));               // saw
    p[PID::Osc1Wave] = (float) Wave::SawPulse;
    CHECK (isRelevant (PID::Osc1PW, p));

    CHECK_FALSE (isRelevant (PID::Lfo2Rate, p));             // LFO 2 not routed
    p[PID::Mx3Source] = (float) MatrixSource::Lfo2;
    p[PID::Mx3Amount] = 0.4f;
    CHECK (isRelevant (PID::Lfo2Rate, p));

    CHECK_FALSE (isRelevant (PID::TuningRoot, p));
    p[PID::Tuning] = (float) Tuning::Werckmeister3;
    CHECK (isRelevant (PID::TuningRoot, p));

    CHECK_FALSE (isRelevant (PID::KeyAssign, p));            // mono
    p[PID::VoiceMode] = (float) VoiceMode::Poly;
    CHECK (isRelevant (PID::KeyAssign, p));

    p[PID::VcaMode] = (float) VcaMode::Gate;
    CHECK_FALSE (isRelevant (PID::AEnvA, p));
    CHECK (isRelevant (PID::FilterCutoff, p));              // unconditional parameters stay relevant
}
