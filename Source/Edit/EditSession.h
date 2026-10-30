#pragma once
// ClaudeTune editor - capture and playback session.
//
// While the host plays, the audio thread hands any audio ClaudeTune hasn't
// heard yet (keyed by its timeline position) to a background worker, which
// adds it to the EditDocument and re-analyses around it. Rendered audio is
// published back to the audio thread as immutable chunks, so playback never
// locks or allocates.
//
// On playback the incoming audio is compared with what was captured:
//  * same audio (any gain)      -> the edit is applied on top of the live input
//  * different audio            -> the live input plays untouched and is re-captured
//  * silence where audio was    -> silence plays, the old capture and edits are kept
//
// The class has no JUCE dependency so it can be tested on its own.

#include "CaptureFifo.h"
#include "EditModel.h"
#include "Renderer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace claudetune::edit
{

struct Transport
{
    bool playing = false;
    bool hasPosition = false;
    int64_t timeInSamples = 0;
    bool realtime = true;   // false during offline bounces: capture may wait instead of dropping

    // musical position (optional; used to draw bars and beats)
    double bpm = 0.0;           // 0 = unknown
    bool hasPpq = false;
    double ppqPosition = 0.0;   // quarter notes at timeInSamples
    int timeSigNumerator = 4, timeSigDenominator = 4;
};

// The host's tempo as last seen, for drawing bars and beats.
struct TempoInfo
{
    double bpm = 0.0;            // 0 = unknown
    double ppqAnchor = 0.0;      // quarter notes at sampleAnchor
    int64_t sampleAnchor = 0;
    int numerator = 4, denominator = 4;

    bool isValid() const noexcept { return bpm > 0.0; }
    double quartersAt (double seconds, double sampleRate) const noexcept
    {
        return ppqAnchor + (seconds - (double) sampleAnchor / sampleRate) * bpm / 60.0;
    }
    double secondsAtQuarters (double q, double sampleRate) const noexcept
    {
        return (double) sampleAnchor / sampleRate + (q - ppqAnchor) * 60.0 / bpm;
    }
    double quartersPerBar() const noexcept { return numerator * 4.0 / denominator; }
};

enum class SessionStatus
{
    Empty,      // nothing captured yet
    Capturing,  // receiving new audio right now
    Analysing,  // new audio waiting to be analysed
    Ready,
    SampleRateMismatch
};

// Read-only view for the UI, rebuilt by the worker after every change.
struct SessionSnapshot
{
    double sampleRate = 0.0;
    int hop = 240;
    int64_t length = 0;
    std::vector<Note> notes;
    std::vector<SampleRange> coverage;
    std::vector<float> pitch;      // detected pitch per analysis frame (MIDI), -1 = unvoiced
    std::vector<float> outPitch;   // pitch after editing, per analysis frame (source time), -1 = unvoiced
    TimeMap map;                   // output <-> source time (samples)
    uint64_t revision = 0;
    bool canUndo = false, canRedo = false;
    bool hasBackup = false;
    SampleRange backupRange { 0, 0 };
};

// Everything needed to rebuild a session (project save / load).
struct SessionState
{
    double sampleRate = 0.0;
    int numChannels = 0;
    int64_t length = 0;
    std::vector<SampleRange> coverage;
    std::vector<std::vector<std::vector<float>>> audio; // per coverage range: channels x samples
    std::vector<PitchFrame> frames;
    std::string edits;
    uint64_t audioRevision = 0;     // identifies the audio + analysis (for caching)

    bool isEmpty() const noexcept { return coverage.empty(); }
};

class EditSession
{
public:
    static constexpr int kChunk = 8192;                  // playback chunk length (samples)
    static constexpr int64_t kMaxSamplesAt192k = 3600LL * 192000; // one hour at 192 kHz

    EditSession();
    ~EditSession();

    // Non-real-time. Safe to call again (sample rate or block size change).
    void prepare (double sampleRate, int numChannels, int maxBlockSize);

    // Audio thread. Processes in place.
    void process (float* const* io, int numChannels, int numSamples, const Transport& transport) noexcept;

    void setCaptureLocked (bool shouldLock) noexcept { captureLocked.store (shouldLock); }
    void setBypassEdits (bool shouldBypass) noexcept { bypassEdits.store (shouldBypass); }

    // ---- editing (message thread) --------------------------------------------
    // Runs fn(EditDocument&) under the document lock, then re-renders in the
    // background. Pass undoable = false for the continuing steps of a drag.
    template <typename Fn>
    bool edit (Fn&& fn, bool undoable = true)
    {
        {
            std::lock_guard<std::mutex> lock (docMutex);
            if (! docInitialised || doc.isEmpty())
                return false;
            if (undoable)
                doc.beginEdit();
            fn (doc);
        }
        requestRender();
        return true;
    }

    template <typename Fn>
    auto read (Fn&& fn) const
    {
        std::lock_guard<std::mutex> lock (docMutex);
        return fn (doc);
    }

    bool undo();
    bool redo();
    void restorePreviousCapture();   // async
    void clearAll();                 // async

    std::shared_ptr<const SessionSnapshot> snapshot() const;
    SessionStatus status() const noexcept { return (SessionStatus) statusValue.load(); }
    int64_t playheadSamples() const noexcept { return playhead.load(); }
    bool isPlaying() const noexcept { return playingNow.load(); }
    bool consumeOverflow() noexcept { return overflow.exchange (false); }
    double sessionSampleRate() const noexcept { return sessionSr.load(); }
    TempoInfo tempo() const noexcept;

    // ---- preview playback (message thread) ---------------------------------------
    // While the host is stopped, ClaudeTune can play the edited vocal itself
    // from any timeline position. Host playback always takes over.
    void startPreview (int64_t fromSample, int64_t toSample, bool loop);
    void stopPreview();
    bool isPreviewing() const noexcept { return previewActive.load(); }
    int64_t previewPosition() const noexcept { return previewPos.load(); }

    // ---- persistence ------------------------------------------------------------
    SessionState exportState() const;
    std::string exportEdits() const;          // just the notes/edits (cheap)
    void importState (SessionState state);   // async: rebuilt on the worker
    uint64_t audioRevision() const;           // changes whenever captured audio changes

    // Waits until captured audio is analysed and every edit is rendered.
    bool waitUntilIdle (double timeoutSeconds);

private:
    struct PlaybackChunk
    {
        int numChannels = 1;
        bool edited = false;                   // out differs from src somewhere
        std::vector<std::vector<float>> src;   // captured audio
        std::vector<std::vector<float>> delta; // rendered - captured (only when edited)
        std::vector<uint8_t> covered;
        bool anyCovered = false;
    };

    struct Segment
    {
        int64_t start = 0;
        std::vector<std::vector<float>> audio;
        std::chrono::steady_clock::time_point lastAppend;
        int64_t length() const noexcept { return audio.empty() ? 0 : (int64_t) audio[0].size(); }
    };

    using Clock = std::chrono::steady_clock;

    // audio thread helpers
    void renderPreview (float* const* io, int numChannels, int numSamples) noexcept;
    void handleUncovered (float* const* io, int numChannels, int offset, int len, int64_t t, bool realtime) noexcept;
    void handleCovered (const PlaybackChunk& chunk, float* const* io, int numChannels, int offset, int len,
                        int chunkOffset, int64_t t, bool realtime) noexcept;
    void pushCapture (int64_t t, float* const* io, int numChannels, int offset, int len, bool realtime) noexcept;

    // worker
    void workerLoop();
    void workOnce();
    void drainFifo();
    void commit (Segment& seg);
    void applyEdits();
    void doImport (SessionState state);
    void doRestoreBackup();
    void doClear();
    void ensureDocument();
    void publish (std::vector<SampleRange> ranges);
    void publishSnapshot();
    void addCoverage (SampleRange r);
    void retire (PlaybackChunk* c);
    void freeRetired (bool all);
    void requestRender() { renderRequested.store (true); wake(); }
    void wake() { wakeCv.notify_one(); }

    // ---- shared state -------------------------------------------------------------
    mutable std::mutex docMutex;          // doc, renderer, out, curves, coverage, table writes
    EditDocument doc;
    Renderer renderer;
    std::vector<std::vector<float>> out;
    RenderCurves curves;
    std::vector<SampleRange> coverage;
    bool docInitialised = false;
    int sessionChannels = 1;
    std::atomic<double> sessionSr { 0.0 };

    std::unique_ptr<std::atomic<PlaybackChunk*>[]> table;
    size_t tableSize = 0;
    std::vector<std::pair<Clock::time_point, PlaybackChunk*>> retired;

    std::mutex fifoMutex;                 // worker vs prepare (never the audio thread)
    CaptureFifo fifo;
    std::vector<Segment> pending;         // worker only
    std::atomic<int> pendingCount { 0 };

    mutable std::mutex snapshotMutex;
    std::shared_ptr<const SessionSnapshot> snap;
    uint64_t snapshotRevision = 0;

    std::mutex commandMutex;
    std::unique_ptr<SessionState> importRequest;
    std::atomic<bool> importPending { false }, restoreRequested { false }, clearRequested { false };
    std::atomic<bool> renderRequested { false };

    // ---- flags shared with the audio thread -----------------------------------
    std::atomic<bool> ready { false }, rateMismatch { false };
    std::atomic<bool> captureLocked { false }, bypassEdits { false };
    std::atomic<bool> overflow { false };
    std::atomic<uint32_t> captureTicks { 0 };
    std::atomic<int64_t> playhead { 0 };
    std::atomic<bool> playingNow { false };
    std::atomic<int> statusValue { (int) SessionStatus::Empty };

    // tempo (written by the audio thread, read by the UI)
    std::atomic<double> tempoBpm { 0.0 }, tempoPpq { 0.0 };
    std::atomic<int64_t> tempoSample { 0 };
    std::atomic<int> tempoNum { 4 }, tempoDen { 4 };

    // preview requests (message thread -> audio thread)
    // every start/stop request gets a sequence number, so a stop that arrives
    // late can never cancel a preview started after it
    std::atomic<uint32_t> previewCommandCounter { 0 };
    std::atomic<uint32_t> previewSerial { 0 }, previewStopSerial { 0 };
    std::atomic<int64_t> previewFrom { 0 }, previewTo { 0 };
    std::atomic<bool> previewLoop { false };
    std::atomic<bool> previewActive { false };
    std::atomic<int64_t> previewPos { 0 };

    // audio-thread-only state
    double preparedSr = 0.0;
    int preparedChannels = 1;
    int mismatchRun = 0, confirmSamples = 960;
    bool retakeConfirmed = false;
    float wet = 1.0f, wetStep = 1.0f / 48.0f, gain = 1.0f;
    int64_t expectedNext = -1;
    int lastVerdict = 0;
    int sameRun = 0;

    // preview state (audio thread)
    bool previewing = false, previewStopping = false, pvLoop = false;
    int64_t pvPos = 0, pvFrom = 0, pvTo = 0;
    uint32_t seenPreviewSerial = 0, seenStopSerial = 0;
    float pvGain = 0.0f, pvFadeStep = 1.0f / 240.0f;
    int pvFadeLen = 240;

    // Look-back buffer: audio that sounds different from the capture, held
    // until the difference has lasted long enough to count as a re-take.
    std::vector<std::vector<float>> lookback;
    int lookbackCapacity = 0, lookbackFrames = 0;
    int64_t lookbackStart = -1;
    bool stashLookback (int64_t t, float* const* io, int numChannels, int offset, int len) noexcept;
    void flushLookback (bool realtime) noexcept;
    void resetRetake() noexcept { mismatchRun = 0; retakeConfirmed = false; lookbackFrames = 0; lookbackStart = -1; }

    // worker-only state
    uint32_t lastTicks = 0;
    Clock::time_point lastActivity;
    std::atomic<bool> workerBusy { false };

    // worker thread
    std::mutex wakeMutex;
    std::condition_variable wakeCv;
    bool stopping = false;
    std::thread worker;
};

} // namespace claudetune::edit
