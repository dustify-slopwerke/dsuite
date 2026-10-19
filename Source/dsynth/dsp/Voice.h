#pragma once

#include "LadderFilter.h"
#include "Oscillator.h"
#include "OtaFilter.h"
#include "Params.h"
#include "SemFilter.h"

#include "common/dsp/Envelope.h"
#include "common/dsp/Lfo.h"
#include "common/dsp/Noise.h"

#include <cstdint>

namespace dsynth
{

// Per-channel MPE expression (member channels), normalised: bend -1..1, pressure/timbre 0..1
struct NoteExpression
{
    float bend = 0.0f, pressure = 0.0f, timbre = 0.0f;
};

// Values shared by all voices that change at most once per host block (or on a MIDI controller).
struct BlockParams
{
    double sampleRate = 96000.0; // internal (oversampled) rate

    bool oscOn[3] {};
    Wave wave[3] {};
    float octave[3] {};          // relative to 8'; Lo = -7
    float tune[3] {};            // semitones
    bool keyTrack[3] { true, true, true };
    bool sync1 = false;          // osc 1 hard-synced to osc 2
    bool dco = false;            // stable pitch: no drift or tuning tolerance
    bool phaseReset = false;     // oscillators restart their waveform on notes from silence
    double subDivide = 2.0;      // sub-oscillator frequency divider (one or two octaves down)
    float crossMod = 0.0f;       // linear FM depth, osc 2 -> osc 1
    bool osc2AsSource = false;   // osc 2 runs even when off: sync, cross-mod, poly-mod or ring mod use it
    bool osc3AsSource = false;   // osc 3 runs even when off: wheel mod or the matrix use it
    NoiseColour noiseColour = NoiseColour::White;

    // Wheel mod (Mini / Prophet): source mixed with noise, scaled by amount + mod wheel
    ModSource wheelSource = ModSource::Osc3;
    float modMix = 0.0f;
    float modPitchSemis = 0.0f;  // depths at full source swing, zero when the destination is off
    float modFilterOct = 0.0f;
    float modPW = 0.0f;

    // Poly-Mod (Prophet): filter env and osc 2 into osc 1 freq / PW and the filter
    float pmFenvSemis = 0.0f, pmOsc2Semis = 0.0f;
    float pmFenvPW = 0.0f, pmOsc2PW = 0.0f;
    float pmFenvOct = 0.0f, pmOsc2Oct = 0.0f;

    // Mod matrix: only active slots, with the amount already scaled to destination units
    struct MatrixSlot
    {
        MatrixSource source;
        MatrixDest dest;
        float depth;
    };
    MatrixSlot matrix[numMatrixSlots] {};
    int numMatrix = 0;
    float modWheel = 0.0f, aftertouch = 0.0f, timbre = 0.0f;

    // LFO fade-in per note (global LFO) and the per-voice LFO 2, which only runs when a matrix
    // slot uses it
    float lfoDelay = 0.0f;
    double lfo2Rate = 5.0;
    LfoWave lfo2Wave = LfoWave::Sine;
    float lfo2Delay = 0.0f;
    bool lfo2KeySync = true;
    bool lfo2Used = false;

    // Microtuning: offset from equal temperament per MIDI note class (note % 12), semitones
    float tuning[12] {};

    // Per-note expression: poly aftertouch by note, MPE by channel (pointers into the engine,
    // refreshed on every render)
    bool mpe = false;
    float mpeBendRange = 48.0f;
    const NoteExpression* channels = nullptr;
    const float* notePressure = nullptr;

    FilterModel filterModel = FilterModel::Ladder24;
    float envAmountOct = 0.0f;
    float keyTrackFilter = 0.0f; // 0..1 octave per octave
    float velFilterOct = 0.0f;
    float resComp = 0.0f;
    bool filterBandpass = false;  // SEM
    float voiceHpfCutoff = 10.0f; // resonant pre-filter high-pass
    float voiceHpfReso = 0.0f;

    float fenv[4] {}, aenv[4] {}; // A, D, S, R
    bool linearEnv = false;
    float ampVel = 0.0f;
    bool vcaGate = false;        // amp follows the key instead of the amp envelope

    GlideType glideType = GlideType::Exponential;
    float glideTime = 0.0f;
    float pitchOffset = 0.0f;    // bend + master tune, semitones

    float drift = 0.0f;
    float tolerance = 0.0f;
};

// Per-sample smoothed / global values shared by all voices.
struct Frame
{
    float cutoffOct;    // log2 Hz
    float resonance;
    float inputGain;    // mixer drive
    float level[3];
    float noise;
    float pw[3];
    float sub;
    float ring;
    float shape;        // SEM filter shape
    float lfo;
};

class Voice
{
public:
    // controlStep: internal samples per modulation update. Envelopes, glide, LFO 2 and the pitch /
    // cutoff maths run at internalRate / controlStep (96 kHz in both quality modes at 48 kHz);
    // oscillators and filters always run at the full internal rate.
    void prepare (double internalRate, int index, int controlStep = 1);

    // Called once per block (and after prepare) so per-voice derived values follow the patch.
    void update (const BlockParams&);

    // retrigger: restart envelopes (from their current level). glide: slide from the current pitch.
    void start (int note, float velocity, bool retrigger, bool glide, const BlockParams&, int midiChannel = 0);
    void release();
    void kill();

    // Fixed pitch offset for unison spread
    void setDetune (float semitones) { detune = semitones; }

    // Duophonic mode: osc 2 plays this many semitones above the voice's note
    void setDuoOffset (float semitones) { duoOffset = semitones; }
    float getDuoOffset() const { return duoOffset; }

    // Stereo position, -1 (left) .. 1 (right); constant power, unity per side at centre
    void setPan (float position);

    // Adds this voice's output into left/right[0..n)
    void render (float* left, float* right, int n, const Frame* frames, const BlockParams&);

    bool isActive() const { return active; }                         // needs rendering
    bool isFree() const { return ! gate && (vcaGate ? gateLevel < 1.0e-4f : ! ampEnv.isActive()); } // silent, reusable
    bool isGateOn() const { return gate; }
    int getNote() const { return note; }
    int getChannel() const { return channel; }
    float getPitch() const { return (float) pitch; } // current (gliding) pitch in semitones
    float getDetune() const { return detune; }
    uint64_t getAge() const { return age; }
    void setAge (uint64_t a) { age = a; }
    uint32_t getTriggerCount() const { return triggerCount; }
    const Envelope& ampEnvelope() const { return ampEnv; }

private:
    void controlTick (const BlockParams&);
    float filter (float input, double cutoffHz, float resonance, float shape, const BlockParams&);
    bool filterSelfOscillates (float resonance, const BlockParams&) const;

    double sampleRate = 96000.0, controlRate = 96000.0;
    int controlStep = 1, controlPhase = 0;
    Oscillator osc[3];
    Oscillator subOsc;
    LadderFilter ladder;
    OtaFilter ota;
    SemFilter sem;
    Ms20Filter ms20;
    VoiceHighpass voiceHpf;
    Envelope filterEnv, ampEnv;
    NoiseSource noise;
    Wander pitchWander, cutoffWander;

    int note = 60, channel = 0;
    float exprPressure = 0.0f, exprBend = 0.0f, exprTimbre = 0.0f, exprCoeff = 0.01f;
    float velocity = 1.0f;
    float detune = 0.0f, duoOffset = 0.0f;
    float panLeft = 1.0f, panRight = 1.0f;
    bool vcaGate = false;
    float gateLevel = 0.0f, gateCoeff = 1.0f;
    bool gate = false, active = false, hasPlayed = false;
    uint64_t age = 0;
    uint32_t triggerCount = 0;

    double pitch = 60.0, targetPitch = 60.0, linearGlideStep = 0.0;
    double glideCoeff = 1.0;

    // Analog character: fixed per-voice offsets (tolerance) and slow wander (drift)
    float tolOsc[3] {}, tolCutoff = 0.0f, tolEnvTime[2] {};
    float driftPitchCents = 0.0f, driftCutoffOct = 0.0f;
    int controlCounter = 0;
    float lastOsc3 = 0.0f;

    // Modulation computed on control ticks and held in between
    struct Control
    {
        double dt[3] {};            // osc 1 without its per-sample poly-mod / cross-mod terms
        float pwMod[2] {};          // added to the panel PW of osc 1 (without poly-mod osc 2) and osc 2
        float levelMod[3] {};       // osc 1, osc 2, noise level offsets
        double cutoffOct = 0.0;     // without the per-sample poly-mod osc 2 term
        double cutoffHz = 1000.0;
        float resonance = 0.0f;
        float amp = 0.0f;
        float fenv = 0.0f;
    } ctl;
    void updateControl (const Frame&, float noiseSample, const BlockParams&);

    Lfo lfo2;
    float lfoFade[2] { 1.0f, 1.0f }, lfoFadeStep[2] { 1.0f, 1.0f }; // global LFO, LFO 2
};

} // namespace dsynth
