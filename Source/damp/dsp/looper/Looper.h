#pragma once

#include "LoopPool.h"
#include "LoopSession.h"

#include "../Params.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace damp
{

// An 8-track looper at the very end of the chain (after every pedal, before output volume), so
// loops keep their effects while the player changes sound over them. Every track plays at once.
//
// Timing. The first track recorded is the master: its loop length (end - start) is the master
// length M. Every other track is either
//   Sync (default): locked to the master clock. Its period is k * M, k = max (1, round ((end -
//     start) / M)). A recorded frame r always sounds when the clock = anchor + r (mod period),
//     anchor being the clock when its recording began, so trimming start or end never moves audio
//     in time: it only chooses which frames sound (a shorter loop is followed by silence up to the
//     period; a longer one is cut at it). Editing the master's length changes M, and every sync
//     track follows (crossfaded).
//   Free: loops its own (end - start) on its own counter.
// Clearing the master leaves M in place for the other tracks; clearing the last track resets it.
//
// Closing a sync recording (Rec pressed on it) waits for its next bar: it records on until its
// length is a whole multiple of M, so it loops with no gap or cut. Pressed within
// `closeToleranceMs` (or a quarter of M, if less) after a bar, it closes at that bar at once (the
// frames past it are dropped). While it waits, trackStatus().ending is set; a second Rec changes
// nothing, and anything else that ends the recording (Stop, Play/Stop, Rec on another track,
// Clear, Undo, switching the looper off, a full pool) cancels the wait and acts at once, as
// before: the recording closes where it is (its period rounding to the nearest multiple of M)
// or, for Clear and Undo, goes. The master itself and free tracks close at once: the engine has
// no grid of its own (the host tempo is only the UI's snap), M is the grid.
//
// Edits are non-destructive and per track, in frames relative to the recording: start may be
// negative and end past the recording (silence, costing no memory). A seam crossfade (0-50 ms,
// default 5) blends the loop's end into the audio before its start, the recording's own edges
// fade in and out over the same length, and an overdub over the seam is written through the same
// equal-power weights so it plays back seamlessly. Edit and reverse / half-speed / sync changes
// crossfade between the old and new read positions (10 ms); undo, redo, clear and audio handed
// over from the message thread dip the track (3 ms each way) and swap at silence.
//
// Reverse and half speed (an octave down, twice as long) are playback only; a track can't be
// overdubbed while either is on. Overdub punches in and out over 5 ms.
//
// Overdubbing over the padding records into it: the take grows (in whole chunks, the frames not
// yet written being silent chunks that cost no memory) to cover what's written inside the loop
// (start to end), plus the seam fade's length of room so its own edge fades fall in silence. What
// the seam blends in from before the start still lands only on recorded frames, so an unpadded
// loop never grows. A full pool stops the overdub as usual. The recording's old edge fades,
// no longer at an edge, are written into its audio, so it sounds as before. Growing at the front
// renumbers the recording's frames: start, end and anchor move by the same amount (nothing is
// heard to move) and trackStatus().front counts the frames added, for views and for edits made
// against the old numbering (setTrackEdits takes the `front` they were made at).
//
// Memory: one LoopPool (5 minutes of stereo at the host rate by default) shared by every track and
// by undo. When it runs out, history (redo, then the oldest undo levels) is given up first; then
// recording stops as if Rec had been pressed, and poolFullCount() goes up.
//
// Threads. The message thread never touches the engine directly: it posts commands to a queue
// that whoever owns the engine applies at a block boundary. The audio thread takes ownership with
// a compare-exchange at the start of each block, applies the queue, renders, and lets go. When it
// hasn't run for `idleTimeout` (no audio, offline, before prepare) a message-thread call takes
// ownership itself and applies the queue at once, so state saving and loading work with no audio.
// Reading audio (saving, export) pins the takes it reads: the engine adds a reference to their
// chunks (so an overdub copies before writing) and the message thread reads them at leisure.
// Nothing on the audio thread allocates or locks; message-thread calls are serialised by a mutex.
class Looper
{
public:
    static constexpr int numTracks = looper::numTracks;
    static constexpr int maxBlock = 512;
    static constexpr double poolSeconds = 300.0;
    static constexpr int overviewBinFrames = looper::LoopPool::binFrames;
    static constexpr float maxCrossfadeMs = 50.0f;
    static constexpr int64_t minLoop = 16; // the shortest loop edits may leave (frames)
    static constexpr double closeToleranceMs = 50.0; // a sync recording closed this soon after a bar closes at it

    enum class State : int { Empty, Recording, Playing, Overdubbing, Stopped };
    enum class Action : int
    {
        Rec,      // the selected track: empty -> record -> play -> overdub -> play ...
        PlayStop, // every track
        Undo, Redo, Clear,
        ClearAll,
        Play, Stop // one track
    };

    struct TrackStatus
    {
        State state = State::Empty;
        int64_t length = 0;         // recorded frames
        int64_t start = 0, end = 0; // edits (recording frames)
        float crossfadeMs = 5.0f;
        int64_t loopLength = 0;     // end - start
        int64_t period = 0;         // frames per cycle as heard (doubled at half speed)
        int multiple = 0;           // of the master length (sync tracks; 1 for the master, 0 free)
        double position = 0.0;      // through the cycle, 0..1
        int64_t sourcePosition = 0; // the recording frame under the playhead (may be in the padding)
        bool sync = false, reverse = false, half = false, muted = false;
        bool master = false;
        bool canOverdub = false;    // not reversed or at half speed
        bool ending = false;        // a sync recording asked to close, recording on to its next bar
        int64_t front = 0;          // frames overdubs have added before the recording (see above)
        int undoLevels = 0, redoLevels = 0;
    };

    struct Status
    {
        bool on = false, poolReady = false;
        double sampleRate = 0.0, tempo = 120.0;
        int selected = 0;
        int masterTrack = -1;
        int64_t masterLength = 0;
        bool running = false;
        double poolUsedSeconds = 0.0, poolTotalSeconds = 0.0;
        int poolFullCount = 0; // goes up each time a recording stopped because the pool was full
    };

    Looper();
    ~Looper();

    // ---- Audio thread (prepare: with audio stopped) ----
    void prepare (double sampleRate);
    // looper_* and trackN_* parameters; the transport buttons act on off -> on
    void setPatch (const Patch&);
    void setTempo (double bpm) { tempo.store (bpm, std::memory_order_relaxed); }
    // In place: the loops are added to the signal, which is recorded. right == nullptr: mono.
    void process (float* left, float* right, int n);

    // ---- Any thread ----
    // The next setPatch adopts the transport buttons' values without acting (after a state load)
    void resyncTransport() { resync.store (true); }
    // Audio has stopped (prepare / release): message-thread calls needn't wait for a block
    void audioStopped() { lastBlock.store (0); }

    // ---- Message thread ----
    // The pool: allocated here (5 minutes stereo at `rate`), never on the audio thread. Replacing
    // it empties every track.
    void createPool (double rate, double seconds = poolSeconds);
    void releasePool();
    bool hasPool() const;
    double poolRate() const; // 0 without a pool

    // Transport from the UI (the host uses the parameters). track -1: the selected one.
    void press (Action, int track = -1);

    // Plays a track from `offset` frames after its start handle (0 <= offset < end - start, padding
    // included), crossfading from where it was (10 ms). A stopped track starts playing there. A sync
    // track (or the master) moves the shared clock, so every sync track jumps with it and they stay
    // aligned; a free track jumps alone. Ignored while it would disturb a recording: a sync seek
    // while any track records or overdubs, a free one while that track overdubs.
    void seek (int track, int64_t offset);

    // Edits, applied at the next block. Start / end in recording frames (clamped: end > start).
    // `front`: the track's trackStatus().front they were made against, if the edit should follow
    // the recording should an overdub grow it at the front first (-1: as they are).
    void setTrackEdits (int track, int64_t start, int64_t end, int64_t front = -1);
    void setTrackCrossfade (int track, float ms);

    TrackStatus trackStatus (int track) const;
    Status status() const;

    // Waveform overview: bin i is the min / max of both channels over recording frames
    // [i, i + 1) * overviewBinFrames, kept up to date while recording and overdubbing
    int overviewBins (int track) const;
    int readOverview (int track, int firstBin, int numBins, float* mins, float* maxs) const;
    uint32_t overviewVersion (int track) const; // changes whenever the track's audio does

    // The whole recording, or one cycle as heard (edits, padding, seams, reverse, half speed)
    bool copyTrackAudio (int track, std::vector<float>& left, std::vector<float>& right);
    bool renderLoop (int track, std::vector<float>& left, std::vector<float>& right);
    // The zero crossing (of the two channels' mean) nearest `frame`, within `radius`; else frame
    int64_t nearestZeroCrossing (int track, int64_t frame, int64_t radius);

    // Replaces a track's audio (an import), at the pool's rate; edits reset, history cleared. Sync
    // tracks start on the master's loop start. Returns the frames that fit (0 without a pool).
    int64_t setTrackAudio (int track, const float* left, const float* right, int64_t frames);

    // Sessions: every track's audio and edits. loadSession needs a pool at the session's rate and
    // replaces everything; the tracks come back stopped.
    looper::LoopSession saveSession();
    bool loadSession (const looper::LoopSession&);

    // How long the audio thread may go without a block before message calls act on their own
    // (default 50 ms; offline tools and tests that drive both sides from one thread use 0)
    void setIdleTimeout (double seconds) { idleTimeout.store ((int64_t) (seconds * 1.0e9)); }

private:
    enum class Op : int { None, Undo, Redo, Clear, Install };

    struct Mapping // how a voice reads a take
    {
        int64_t start = 0, loop = 1, period = 1, anchor = 0;
        int fade = 0;
        bool reverse = false, half = false, sync = false;
        int64_t cycle() const { return half ? 2 * period : period; }
        bool operator== (const Mapping&) const = default;
    };
    struct Voice
    {
        Mapping m;
        int64_t phase = 0; // frames into the cycle (free voices; sync voices follow the clock)
    };
    struct Track
    {
        State state = State::Empty;
        int64_t start = 0, end = 0, anchor = 0;
        float crossfadeMs = 5.0f;
        bool reverse = false, half = false, sync = true, mute = false;
        float level = 1.0f;
        Voice voice, old;
        float blend = 1.0f; // 1: the voice alone, else crossfading from old
        bool remapPending = false;
        float gate = 0.0f, gateTarget = 0.0f, gain = 1.0f;
        float writeGain = 0.0f, writeTarget = 0.0f;
        Op pending = Op::None;
        bool resume = false;
        bool ending = false; // recording on to the next bar, then closing
    };

    struct Command
    {
        enum class Type : int { Action, Edits, Crossfade, Swap, Claim, Unclaim, Install, Pin, Unpin, SetMaster, ClearAll, Seek };
        Type type = Type::Action;
        int track = -1, action = 0;
        int64_t a = 0, b = 0;
        float f = 0.0f;
        void* payload = nullptr;
        int64_t c = 0;
    };
    struct Claim
    {
        std::vector<int> ids;
        int got = 0;
    };
    struct Install
    {
        int track = 0;
        std::vector<int> ids;
        int count = 0;
        int64_t length = 0, start = 0, end = 0, anchor = 0;
        bool explicitAnchor = false;
        float crossfadeMs = 5.0f;
    };
    struct Pinned
    {
        std::array<bool, numTracks> wanted {};
        std::array<looper::Take, numTracks> takes;
        std::array<Mapping, numTracks> maps;
        std::array<float, numTracks> crossfadeMs {};
        int masterTrack = -1;
        int64_t masterLength = 0, origin = 0;
    };

    // Owner side (audio thread, or a message call while the audio thread is idle)
    void service();
    void apply (const Command&);
    void doAction (Action, int track);
    void rec (int t);
    void startRecording (int t);
    void closeRecording (int t);
    void requestClose (int t); // closes a sync recording at its next bar
    int64_t closeBar (int t) const; // the grid a recording closes on (0: none, it closes at once)
    void truncateRecording (int t, int64_t length);
    void punchIn (int t);
    void punchOut (int t, bool now);
    void playAll();
    void stopAll();
    void playTrack (int t);
    void stopTrack (int t);
    void schedule (int t, Op);
    void applyOp (int t);
    void finishPending(); // every pending op at once (no audio to dip)
    void pushUndo (int t);
    void clearHistory (int t, bool undoToo);
    void takeChanged (int t, bool adoptEdits);
    void trackEmptied (int t);
    void clearTrack (int t);
    void setEdits (int t, int64_t start, int64_t end, int64_t front);
    void doSeek (int t, int64_t offset);
    void jumpFrom (Track&); // crossfades from the voice as it is now
    Mapping mappingFor (int t) const;
    void remap (int t, bool crossfade);
    void remapAll (bool crossfade);
    bool running() const;
    bool canOverdub (int t) const { return ! tracks[(size_t) t].reverse && ! tracks[(size_t) t].half; }
    int allocateChunk(); // gives up history if it must; -1 when the pool is full
    bool dropHistory();
    int64_t phaseAt (const Voice&, int64_t clockAt) const;
    void renderTrack (int t, int n, bool mono, float* outL, float* outR);
    int record (int t, int n, bool mono); // the frames taken (fewer if the recording closed)
    bool overdubAt (int t, int64_t phase, float l, float r);
    bool addAt (int t, int64_t i, float l, float r);
    int64_t grow (int t, int64_t i); // covers frame i (in the padding); its new index, or -1 (pool full)
    int writableChunk (int t, int64_t chunkIndex); // unshared, real (copy on write / fill a silent one); -1: pool full
    bool bakeEdge (int t, bool front); // writes the recording's fade at that edge into its audio
    void markDirty (int t, int64_t i);
    void flushDirty (int t);
    void publishStatus();
    looper::LoopStore* usable() const; // the store, if it matches the rate

    static void readRecorded (const looper::LoopPool&, const looper::Take&, int64_t i, int fade, float& l, float& r);
    static void contentAt (const looper::LoopPool&, const looper::Take&, const Mapping&, int64_t phi, float& l, float& r);
    static void voiceAt (const looper::LoopPool&, const looper::Take&, const Mapping&, int64_t phase, float& l, float& r);
    static int64_t sourcePhase (const Mapping&, int64_t phase);
    static int64_t phaseForSource (const Mapping&, int64_t sourcePhase);

    // Message side
    uint64_t post (const Command&);
    void flush (uint64_t sequence);
    bool audioIdle() const;
    bool serviceIfIdle();
    std::unique_ptr<Pinned> pin (uint32_t trackMask);
    void unpin (std::unique_ptr<Pinned>);
    int64_t installTrack (int track, const float* left, const float* right, int64_t frames, int64_t start, int64_t end,
                          float crossfadeMs, bool explicitAnchor, int64_t anchor);

    // ---- Owner state ----
    double rate = 48000.0;
    looper::LoopStore* store = nullptr;
    std::array<Track, numTracks> tracks;
    int64_t clock = 0, origin = 0, masterLength = 0;
    int masterTrack = -1;
    bool on = false;
    int selected = 0;
    float gateStep = 0.01f, blendStep = 0.002f, writeStep = 0.004f, gainCoeff = 0.001f;
    std::array<float, maxBlock> inL {}, inR {}, loopL {}, loopR {};
    struct Dirty { int64_t from = 0, to = -1; };
    std::array<Dirty, numTracks> dirty;
    std::array<bool, numTracks> listChanged {};

    // Set by setPatch (audio thread), taken by the owner
    struct Controls
    {
        bool on = false;
        int selected = 0;
        std::array<float, numTracks> level {};
        std::array<bool, numTracks> mute {}, reverse {}, half {}, sync {};
    } controls;
    std::array<bool, 5> buttons {}; // rec, play/stop, undo, redo, clear: last values
    bool buttonsKnown = false;
    std::array<int, 16> edgeActions {};
    int numEdgeActions = 0;
    std::atomic<bool> resync { false };

    // ---- Shared ----
    std::atomic<int> owner { 0 }; // 0 free, 1 audio thread, 2 a message call
    std::atomic<int64_t> lastBlock { 0 }, idleTimeout { 50'000'000 };
    std::atomic<double> tempo { 120.0 };
    static constexpr size_t queueSize = 512;
    std::array<Command, queueSize> queue;
    std::atomic<uint64_t> written { 0 }, done { 0 };

    struct Published
    {
        std::atomic<int> state { 0 }, multiple { 0 }, flags { 0 }, undo { 0 }, redo { 0 };
        std::atomic<int64_t> length { 0 }, start { 0 }, end { 0 }, loop { 0 }, period { 0 }, source { 0 }, front { 0 };
        std::atomic<float> crossfade { 5.0f };
        std::atomic<double> position { 0.0 };
    };
    std::array<Published, numTracks> published;
    std::atomic<int> pubMaster { -1 }, pubSelected { 0 }, poolFull { 0 };
    std::atomic<int64_t> pubMasterLength { 0 };
    std::atomic<bool> pubOn { false }, pubRunning { false };

    // ---- Message side ----
    mutable std::recursive_mutex api; // serialises message-thread calls
    std::mutex postLock;
    std::unique_ptr<looper::LoopStore> ownedStore;
};

} // namespace damp
