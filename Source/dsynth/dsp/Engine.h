#pragma once

#include "Arpeggiator.h"
#include "JunoHpf.h"
#include "Params.h"
#include "Voice.h"

#include "common/dsp/BbdChorus.h"
#include "common/dsp/Decimator.h"
#include "common/dsp/Delay.h"
#include "common/dsp/Lfo.h"
#include "common/dsp/Reverb.h"
#include "common/dsp/Smoother.h"

#include <array>
#include <vector>

namespace dsynth
{

// The whole synth minus the plugin wrapper: arpeggiator, voice allocation, smoothing, 2x or 4x
// oversampled stereo rendering, decimation and effects. Real-time safe after prepare().
class Engine
{
public:
    static constexpr int maxVoices = 8;
    static constexpr int maxOversampling = 4;

    void prepare (double hostSampleRate);

    // Call once per block before rendering; picks up all parameter changes.
    void setPatch (const Patch&);

    // Host transport for the arpeggiator; call once per block. bpm <= 0: no host tempo.
    void setTransport (double bpm, bool playing, double ppqPosition);

    // channel: MIDI channel 0..15. Only matters in MPE mode, where channel 0 is the master
    // (global) channel and every other channel carries one note's own bend/pressure/timbre.
    void noteOn (int note, float velocity, int channel = 0);
    void noteOff (int note, int channel = 0);
    void pitchBend (float normalised, int channel = 0); // -1..1
    void modWheel (float normalised);                   // 0..1
    void aftertouch (float normalised, int channel = 0); // channel pressure, 0..1
    void polyPressure (int note, float normalised);      // polyphonic aftertouch, 0..1
    void timbre (float normalised, int channel = 0);     // CC74, 0..1
    void sustainPedal (bool down);
    void allNotesOff();

    // Renders n stereo samples at the host rate, replacing the contents of left/right.
    void process (float* left, float* right, int n);

    // Introspection (tests / UI)
    int getMonoNote() const { return monoNote; }
    bool isArpOn() const { return arpOn; }
    const Voice& getVoice (int i) const { return voices[(size_t) i]; }
    int numActiveVoices() const;
    int getOversampling() const { return oversampling; }

private:
    void prepareInternalRate();
    void updateBlockParams();
    void render (float* left, float* right, int n);
    float shapeVelocity (float) const;
    void voiceNoteOn (int note, float velocity, int channel = 0);
    void voiceNoteOff (int note);
    void updateArpClock();
    void updateEffects();
    void monoUpdate (bool newKeyPressed, float velocity);
    int pickMonoNote() const;
    int monoVoiceCount() const { return mode == VoiceMode::Unison ? unisonCount : 1; }
    Voice& allocatePolyVoice (int note);
    void releaseNote (int note);
    bool anyKeyHeld() const { return heldCount > 0; }

    double hostRate = 48000.0, internalRate = 96000.0;
    int oversampling = 2;
    Patch patch = Patch::defaults();
    BlockParams block;
    VoiceMode mode = VoiceMode::Mono;

    std::array<Voice, maxVoices> voices;
    uint64_t ageCounter = 0;
    int rotateNext = 0; // Rotate key assign: where the search for the next voice starts

    // Keys in the order they were pressed (for mono note priority)
    std::array<int, 128> held {};
    int heldCount = 0;
    std::array<bool, 128> sustained {};
    std::array<int, 128> heldChannel {};
    std::array<NoteExpression, 16> channels {};
    std::array<float, 128> notePressure {};
    float globalTimbre = 0.0f;
    bool sustainDown = false;
    int monoNote = -1;

    float bend = 0.0f, wheel = 0.0f, pressure = 0.0f, lastVelocity = 1.0f;
    int unisonCount = 4;
    float stackGain = 1.0f;

    Lfo lfo;
    double lfoRate = 5.0;
    LfoWave lfoWave = LfoWave::Triangle;

    Arpeggiator arp;
    bool arpOn = false;
    double hostBpm = 0.0, hostPpq = -1.0;
    bool hostPlaying = false;

    // Smoothed global values
    enum { sCutoff, sReso, sDrive, sLevel1, sLevel2, sLevel3, sNoise, sPW1, sPW2, sPW3, sSub, sRing, sShape, numSmoothers };
    std::array<Smoother, numSmoothers> smoothers;
    Smoother volume;
    float smoothCoeff = 0.0f, hostSmoothCoeff = 0.0f;
    bool firstPatch = true;

    static constexpr int chunk = 32; // host samples per inner render pass
    std::array<Frame, chunk * maxOversampling> frames {};
    std::array<float, chunk * maxOversampling> bufferL {}, bufferR {};
    HalfbandDecimator decimatorL, decimatorR;
    HalfbandDecimatorT<11> firstStageL, firstStageR; // 4x -> 2x in HQ mode
    JunoHpf hpfL, hpfR;
    BbdChorus chorus;
    StereoDelay delay;
    FdnReverb reverb;
    float dcX[2] {}, dcY[2] {}, dcCoeff = 0.9995f;
};

} // namespace dsynth
