#include "EditSession.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace claudetune::edit
{

namespace
{
    constexpr double kCommitIdleSec = 0.15;      // a capture run that paused this long is analysed
    constexpr double kCommitEverySec = 0.75;     // ... or every this much audio while it keeps going
    constexpr double kSealBackupSec = 1.0;       // re-take backups close after this much quiet
    constexpr double kRetireSec = 2.0;           // replaced playback chunks are freed after this
    constexpr double kFifoSeconds = 6.0;
    constexpr double kSilentMeanSquare = 1e-10;  // -100 dBFS
    constexpr double kRecaptureMeanSquare = 1e-6; // -60 dBFS: quieter input never replaces a capture
    constexpr double kSameCorrelation = 0.99;
    constexpr int kSameWindowsNeeded = 4;        // ~5 ms of agreement before the edit is applied again
    constexpr int kMaxChannels = 16;

    enum Verdict { Same = 0, Different = 1, InputSilent = 2 };

    size_t tableEntries() { return (size_t) (EditSession::kMaxSamplesAt192k / EditSession::kChunk) + 1; }
}

//==============================================================================
EditSession::EditSession()
{
    tableSize = tableEntries();
    table.reset (new std::atomic<PlaybackChunk*>[tableSize]);
    for (size_t i = 0; i < tableSize; ++i)
        table[i].store (nullptr);

    lastActivity = Clock::now();
    snap = std::make_shared<SessionSnapshot>();
    worker = std::thread ([this] { workerLoop(); });
}

EditSession::~EditSession()
{
    {
        std::lock_guard<std::mutex> lock (wakeMutex);
        stopping = true;
    }
    wakeCv.notify_all();
    if (worker.joinable())
        worker.join();

    for (size_t i = 0; i < tableSize; ++i)
        delete table[i].exchange (nullptr);
    freeRetired (true);
}

void EditSession::prepare (double sampleRate, int numChannels, int /*maxBlockSize*/)
{
    std::lock_guard<std::mutex> fifoLock (fifoMutex);
    std::lock_guard<std::mutex> docLock (docMutex);

    preparedSr = sampleRate;
    preparedChannels = std::clamp (numChannels, 1, kMaxChannels);
    fifo.allocate (preparedChannels, (int) (sampleRate * kFifoSeconds), 16384);

    if (! docInitialised)
    {
        sessionSr.store (sampleRate);
        sessionChannels = preparedChannels;
    }
    rateMismatch.store (docInitialised && std::abs (sessionSr.load() - sampleRate) > 0.5);

    pvFadeLen = std::max (1, (int) (0.005 * sampleRate));
    pvFadeStep = 1.0f / (float) pvFadeLen;
    confirmSamples = std::max (1, (int) (0.020 * sampleRate));
    wetStep = (float) (1.0 / (0.001 * sampleRate));
    lookbackCapacity = (int) (0.150 * sampleRate);
    lookback.assign ((size_t) preparedChannels, std::vector<float> ((size_t) lookbackCapacity, 0.0f));
    resetRetake();
    expectedNext = -1;
    ready.store (true);
}

//==============================================================================
// Audio thread

void EditSession::process (float* const* io, int numChannels, int n, const Transport& tr) noexcept
{
    playingNow.store (tr.playing && tr.hasPosition, std::memory_order_relaxed);
    if (tr.hasPosition)
        playhead.store (tr.timeInSamples, std::memory_order_relaxed);
    if (tr.bpm > 0.0 && tr.hasPosition)
    {
        tempoBpm.store (tr.bpm, std::memory_order_relaxed);
        tempoNum.store (tr.timeSigNumerator, std::memory_order_relaxed);
        tempoDen.store (tr.timeSigDenominator, std::memory_order_relaxed);
        if (tr.hasPpq)
        {
            tempoPpq.store (tr.ppqPosition, std::memory_order_relaxed);
            tempoSample.store (tr.timeInSamples, std::memory_order_relaxed);
        }
    }

    // ---- preview requests from the editor ---------------------------------------
    const uint32_t ps = previewSerial.load (std::memory_order_acquire);
    if (ps != seenPreviewSerial)
    {
        seenPreviewSerial = ps;
        pvFrom = std::max<int64_t> (0, previewFrom.load (std::memory_order_relaxed));
        pvTo = previewTo.load (std::memory_order_relaxed);
        pvLoop = previewLoop.load (std::memory_order_relaxed);
        pvPos = pvFrom;
        previewing = pvTo > pvFrom;
        previewStopping = false;
        pvGain = 0.0f;
    }
    const uint32_t ss = previewStopSerial.load (std::memory_order_acquire);
    if (ss != seenStopSerial)
    {
        seenStopSerial = ss;
        if (previewing && (int32_t) (ss - seenPreviewSerial) > 0) // only a stop issued after the current start
            previewStopping = true;
    }
    if (previewing && tr.playing)
        previewing = false; // the host's transport always wins

    if (previewing && ready.load (std::memory_order_acquire) && ! rateMismatch.load (std::memory_order_relaxed) && numChannels > 0)
    {
        renderPreview (io, numChannels, n);
        previewActive.store (previewing, std::memory_order_relaxed);
        previewPos.store (pvPos, std::memory_order_relaxed);
        expectedNext = -1;
        return;
    }
    if (previewActive.load (std::memory_order_relaxed) && ! previewing)
        previewActive.store (false, std::memory_order_relaxed);

    if (! ready.load (std::memory_order_acquire) || rateMismatch.load (std::memory_order_relaxed)
        || ! tr.playing || ! tr.hasPosition || numChannels <= 0 || n <= 0)
    {
        expectedNext = -1; // input passes through untouched
        return;
    }

    // a new start or a jump: trust the capture until the audio proves otherwise
    if (tr.timeInSamples != expectedNext)
    {
        resetRetake();
        lastVerdict = Same;
        sameRun = kSameWindowsNeeded;
        wet = bypassEdits.load (std::memory_order_relaxed) ? 0.0f : 1.0f;
        gain = 1.0f;
    }
    expectedNext = tr.timeInSamples + n;

    const int64_t base = tr.timeInSamples;
    const auto maxSamples = (int64_t) tableSize * kChunk;
    int i = 0;
    while (i < n)
    {
        const int64_t t = base + i;
        if (t < 0)
        {
            i += (int) std::min<int64_t> (n - i, -t);   // count-in before the timeline start
            continue;
        }
        if (t >= maxSamples)
            break;

        const auto ci = (size_t) (t / kChunk);
        const int off = (int) (t % kChunk);
        const int span = std::min (n - i, kChunk - off);
        const PlaybackChunk* chunk = table[ci].load (std::memory_order_acquire);

        if (chunk == nullptr || ! chunk->anyCovered)
            handleUncovered (io, numChannels, i, span, t, tr.realtime);
        else
        {
            int j = 0;
            while (j < span)
            {
                const bool cov = chunk->covered[(size_t) (off + j)] != 0;
                int k = j;
                while (k < span && (chunk->covered[(size_t) (off + k)] != 0) == cov)
                    ++k;
                if (cov)
                    handleCovered (*chunk, io, numChannels, i + j, k - j, off + j, t + j, tr.realtime);
                else
                    handleUncovered (io, numChannels, i + j, k - j, t + j, tr.realtime);
                j = k;
            }
        }
        i += span;
    }
}

void EditSession::renderPreview (float* const* io, int numChannels, int n) noexcept
{
    const auto maxSamples = (int64_t) tableSize * kChunk;
    for (int i = 0; i < n; ++i)
    {
        if (! previewing)
        {
            for (int c = 0; c < numChannels; ++c) io[c][i] = 0.0f;
            continue;
        }

        const float target = previewStopping ? 0.0f : 1.0f;
        if (pvGain < target)      pvGain = std::min (target, pvGain + pvFadeStep);
        else if (pvGain > target) pvGain = std::max (target, pvGain - pvFadeStep);

        // short fades at the edges of the preview range (and at every loop)
        float env = std::min (1.0f, (float) (pvTo - pvPos) / (float) pvFadeLen);
        if (pvLoop)
            env = std::min (env, (float) (pvPos - pvFrom + 1) / (float) pvFadeLen);
        env = std::max (0.0f, env) * pvGain;

        const PlaybackChunk* chunk = (pvPos >= 0 && pvPos < maxSamples)
                                         ? table[(size_t) (pvPos / kChunk)].load (std::memory_order_acquire) : nullptr;
        const auto off = (size_t) (pvPos % kChunk);
        for (int c = 0; c < numChannels; ++c)
        {
            float v = 0.0f;
            if (chunk != nullptr && chunk->covered[off] != 0)
            {
                const auto sc = (size_t) std::min (c, chunk->numChannels - 1);
                v = chunk->src[sc][off];
                if (chunk->edited)
                    v += chunk->delta[sc][off];
            }
            io[c][i] = v * env;
        }

        if (previewStopping && pvGain <= 0.0f)
            previewing = false;

        if (++pvPos >= pvTo)
        {
            if (pvLoop) pvPos = pvFrom;
            else previewing = false;
        }
    }
}

void EditSession::handleUncovered (float* const* io, int numChannels, int offset, int len, int64_t t, bool realtime) noexcept
{
    wet = 0.0f; // nothing to apply here: the input passes through
    resetRetake();
    pushCapture (t, io, numChannels, offset, len, realtime);
}

bool EditSession::stashLookback (int64_t t, float* const* io, int numChannels, int offset, int len) noexcept
{
    if (lookbackStart >= 0 && lookbackStart + lookbackFrames != t)
        lookbackFrames = 0, lookbackStart = -1;     // not contiguous: start over
    if (lookbackFrames + len > lookbackCapacity || lookback.empty())
        return false;
    if (lookbackStart < 0)
        lookbackStart = t;
    for (size_t c = 0; c < lookback.size(); ++c)
        std::copy (io[std::min ((int) c, numChannels - 1)] + offset, io[std::min ((int) c, numChannels - 1)] + offset + len,
                   lookback[c].begin() + lookbackFrames);
    lookbackFrames += len;
    return true;
}

void EditSession::flushLookback (bool realtime) noexcept
{
    if (lookbackFrames > 0 && lookbackStart >= 0)
    {
        float* ptrs[kMaxChannels];
        const int nc = std::min ((int) lookback.size(), kMaxChannels);
        for (int c = 0; c < nc; ++c)
            ptrs[c] = lookback[(size_t) c].data();
        pushCapture (lookbackStart, ptrs, nc, 0, lookbackFrames, realtime);
    }
    lookbackFrames = 0;
    lookbackStart = -1;
}

void EditSession::handleCovered (const PlaybackChunk& chunk, float* const* io, int numChannels, int offset, int len,
                                 int chunkOffset, int64_t t, bool realtime) noexcept
{
    constexpr int kWindow = 64; // ~1.3 ms: differences are found (and handled) at this resolution
    const int sc = chunk.numChannels;
    const float inScale = 1.0f / (float) numChannels, srcScale = 1.0f / (float) sc;
    const bool bypass = bypassEdits.load (std::memory_order_relaxed);

    for (int w0 = 0; w0 < len; w0 += kWindow)
    {
        const int wl = std::min (kWindow, len - w0);
        const int io0 = offset + w0, ck0 = chunkOffset + w0;

        // ---- is this the audio that was captured? ------------------------------
        double dot = 0.0, ein = 0.0, esrc = 0.0;
        for (int s = 0; s < wl; ++s)
        {
            float a = 0.0f, b = 0.0f;
            for (int c = 0; c < numChannels; ++c) a += io[c][io0 + s];
            for (int c = 0; c < sc; ++c) b += chunk.src[(size_t) c][(size_t) (ck0 + s)];
            a *= inScale;
            b *= srcScale;
            dot += (double) a * b;
            ein += (double) a * a;
            esrc += (double) b * b;
        }
        const double msIn = ein / wl, msSrc = esrc / wl;

        int verdict;
        bool reliable = true;
        if (msIn < kSilentMeanSquare && msSrc < kSilentMeanSquare)
            verdict = Same;
        else if (msIn < kSilentMeanSquare)
            verdict = InputSilent;
        else if (msSrc < kSilentMeanSquare)
            verdict = Different;
        else if (wl < 8)
        {
            verdict = lastVerdict; // too short to judge
            reliable = false;
        }
        else
        {
            const double corr = dot / std::sqrt (ein * esrc);
            verdict = corr >= kSameCorrelation ? Same : Different;
        }
        lastVerdict = verdict;

        // Short windows of two different performances can look alike by chance,
        // so "same audio" needs several windows in a row; "different" acts at once.
        if (verdict == Same) { if (reliable) ++sameRun; }
        else sameRun = 0;
        const bool confirmedSame = sameRun >= kSameWindowsNeeded;
        if (confirmedSame && msSrc >= kSilentMeanSquare && msIn >= kSilentMeanSquare)
            gain += 0.8f * ((float) std::clamp (dot / esrc, 0.0, 8.0) - gain);

        // ---- re-capture only differences that last (a real re-take) --------------
        if (confirmedSame)
            resetRetake();
        else if (retakeConfirmed)
            pushCapture (t + w0, io, numChannels, io0, wl, realtime);
        else if (verdict == InputSilent)
            resetRetake();      // muted or deleted region: keep the capture and its edits
        else
        {
            if (! stashLookback (t + w0, io, numChannels, io0, wl))
            {
                resetRetake();
                stashLookback (t + w0, io, numChannels, io0, wl);
            }
            if (verdict == Different && msIn >= kRecaptureMeanSquare)
                mismatchRun += wl;
            if (mismatchRun >= confirmSamples)
            {
                retakeConfirmed = true;
                flushLookback (realtime);
            }
        }

        // ---- output: live input + (rendered - captured) * gain --------------------
        // Applying the edit as a difference keeps unedited audio bit-identical to
        // the input and makes switching in and out of edited audio click-free.
        const float target = (confirmedSame && ! bypass) ? 1.0f : 0.0f;
        if (! chunk.edited)
        {
            wet = target;
            continue;
        }
        for (int s = 0; s < wl; ++s)
        {
            if (wet < target)      wet = std::min (target, wet + wetStep);
            else if (wet > target) wet = std::max (target, wet - wetStep);
            if (wet <= 0.0f) continue;
            const float k = wet * gain;
            for (int c = 0; c < numChannels; ++c)
                io[c][io0 + s] += k * chunk.delta[(size_t) std::min (c, sc - 1)][(size_t) (ck0 + s)];
        }
    }
}

void EditSession::pushCapture (int64_t t, float* const* io, int numChannels, int offset, int len, bool realtime) noexcept
{
    if (captureLocked.load (std::memory_order_relaxed) || len <= 0)
        return;

    const float* ptrs[kMaxChannels];
    const int nc = std::min (numChannels, kMaxChannels);
    for (int c = 0; c < nc; ++c)
        ptrs[c] = io[c];

    int attempts = 0;
    while (! fifo.push (t, ptrs, nc, offset, len))
    {
        if (realtime || ++attempts > 200000)
        {
            overflow.store (true, std::memory_order_relaxed);
            return;
        }
        std::this_thread::yield(); // offline bounce: wait for the worker to catch up
        wake();
    }
    captureTicks.fetch_add (1, std::memory_order_relaxed);
}

//==============================================================================
// Worker

void EditSession::workerLoop()
{
    std::unique_lock<std::mutex> lock (wakeMutex);
    while (! stopping)
    {
        wakeCv.wait_for (lock, std::chrono::milliseconds (15));
        if (stopping) break;
        lock.unlock();
        workerBusy.store (true);
        workOnce();
        workerBusy.store (false);
        lock.lock();
    }
}

void EditSession::workOnce()
{
    const auto now = Clock::now();

    if (clearRequested.exchange (false))
        doClear();

    if (importPending.load())
    {
        std::unique_ptr<SessionState> req;
        {
            std::lock_guard<std::mutex> lock (commandMutex);
            req = std::move (importRequest);
        }
        if (req)
            doImport (std::move (*req));
        importPending.store (false);
    }

    if (restoreRequested.exchange (false))
        doRestoreBackup();

    drainFifo();

    // analyse capture runs that paused, or that have grown long enough
    for (size_t i = 0; i < pending.size();)
    {
        auto& seg = pending[i];
        const double idle = std::chrono::duration<double> (now - seg.lastAppend).count();
        const double sr = std::max (1.0, sessionSr.load());
        if (seg.length() > 0 && (idle >= kCommitIdleSec || seg.length() >= (int64_t) (kCommitEverySec * sr)))
        {
            commit (seg);
            const int64_t end = seg.start + seg.length();
            seg.start = end;
            for (auto& ch : seg.audio) ch.clear();
        }
        if (seg.length() == 0 && idle >= kCommitIdleSec)
        {
            pending.erase (pending.begin() + (long) i);
            continue;
        }
        ++i;
    }
    pendingCount.store ((int) pending.size());

    if (renderRequested.exchange (false))
        applyEdits();

    // capture activity, re-take backups
    const uint32_t ticks = captureTicks.load();
    if (ticks != lastTicks)
    {
        lastTicks = ticks;
        lastActivity = now;
    }
    const double quiet = std::chrono::duration<double> (now - lastActivity).count();
    if (quiet >= kSealBackupSec)
    {
        std::lock_guard<std::mutex> lock (docMutex);
        if (docInitialised)
            doc.sealCaptureBackup();
    }

    freeRetired (false);

    SessionStatus st = SessionStatus::Empty;
    bool hasAudio = false;
    {
        std::lock_guard<std::mutex> lock (docMutex);
        hasAudio = docInitialised && ! coverage.empty();
    }
    if (rateMismatch.load())                 st = SessionStatus::SampleRateMismatch;
    else if (quiet < 0.3 && playingNow.load()) st = SessionStatus::Capturing;
    else if (! pending.empty() || importPending.load()) st = SessionStatus::Analysing;
    else if (hasAudio)                       st = SessionStatus::Ready;
    statusValue.store ((int) st);
}

void EditSession::drainFifo()
{
    std::lock_guard<std::mutex> lock (fifoMutex);
    if (! fifo.isAllocated())
        return;

    CaptureFifo::Header h;
    std::vector<std::vector<float>> data;
    const auto now = Clock::now();
    while (true)
    {
        for (auto& ch : data) ch.clear();
        if (! fifo.pop (h, data))
            break;

        Segment* target = nullptr;
        for (auto& seg : pending)
            if (seg.start + seg.length() == h.timeline)
            {
                target = &seg;
                break;
            }
        if (target == nullptr)
        {
            pending.push_back ({});
            target = &pending.back();
            target->start = h.timeline;
        }
        if (target->audio.size() != data.size())
            target->audio.resize (data.size());
        for (size_t c = 0; c < data.size(); ++c)
            target->audio[c].insert (target->audio[c].end(), data[c].begin(), data[c].end());
        target->lastAppend = now;
    }
    pendingCount.store ((int) pending.size());
}

void EditSession::ensureDocument()
{
    if (docInitialised)
        return;
    doc.initEmpty (sessionChannels, sessionSr.load());
    renderer.prepare (doc);
    curves = doc.buildCurves();
    out.assign ((size_t) doc.numChannels(), {});
    docInitialised = true;
}

void EditSession::commit (Segment& seg)
{
    std::lock_guard<std::mutex> lock (docMutex);
    if (rateMismatch.load())
        return;
    ensureDocument();

    const SampleRange range { seg.start, seg.start + seg.length() };
    const auto r = doc.replaceAudio (seg.start, seg.audio);
    if (r.audioChanged)
        renderer.refreshMarks (doc, r.changed);
    else if (r.newLength != r.oldLength)
        renderer.refreshMarks (doc, { r.oldLength, r.newLength });

    RenderCurves after = doc.buildCurves();
    std::vector<SampleRange> forced;
    if (r.audioChanged)
        forced.push_back (r.changed);
    auto ranges = renderer.update (doc, curves, after, out, forced);
    curves = std::move (after);

    addCoverage (range);
    ranges.push_back (range);
    publish (std::move (ranges));
    publishSnapshot();
}

void EditSession::applyEdits()
{
    std::lock_guard<std::mutex> lock (docMutex);
    if (! docInitialised)
        return;
    RenderCurves after = doc.buildCurves();
    auto ranges = renderer.update (doc, curves, after, out);
    curves = std::move (after);
    publish (std::move (ranges));
    publishSnapshot();
}

void EditSession::doRestoreBackup()
{
    std::lock_guard<std::mutex> lock (docMutex);
    if (! docInitialised || ! doc.hasCaptureBackup())
        return;
    const auto range = doc.restoreCaptureBackup();
    renderer.refreshMarks (doc, range);
    RenderCurves after = doc.buildCurves();
    auto ranges = renderer.update (doc, curves, after, out, { range });
    curves = std::move (after);
    ranges.push_back (range);
    publish (std::move (ranges));
    publishSnapshot();
}

void EditSession::doClear()
{
    std::lock_guard<std::mutex> lock (docMutex);
    for (size_t i = 0; i < tableSize; ++i)
        if (auto* old = table[i].exchange (nullptr, std::memory_order_acq_rel))
            retire (old);
    doc = EditDocument {};
    renderer = Renderer {};
    out.clear();
    curves = {};
    coverage.clear();
    docInitialised = false;
    sessionSr.store (preparedSr);
    sessionChannels = preparedChannels;
    rateMismatch.store (false);
    publishSnapshot();
}

void EditSession::doImport (SessionState state)
{
    std::lock_guard<std::mutex> lock (docMutex);
    for (size_t i = 0; i < tableSize; ++i)
        if (auto* old = table[i].exchange (nullptr, std::memory_order_acq_rel))
            retire (old);
    coverage.clear();
    out.clear();
    docInitialised = false;

    if (state.isEmpty() || state.sampleRate <= 0.0 || state.numChannels <= 0)
    {
        doc = EditDocument {};
        publishSnapshot();
        return;
    }

    sessionSr.store (state.sampleRate);
    sessionChannels = std::clamp (state.numChannels, 1, kMaxChannels);
    rateMismatch.store (preparedSr > 0.0 && std::abs (preparedSr - state.sampleRate) > 0.5);

    std::vector<std::vector<float>> channels ((size_t) sessionChannels, std::vector<float> ((size_t) std::max<int64_t> (0, state.length), 0.0f));
    for (size_t k = 0; k < state.coverage.size() && k < state.audio.size(); ++k)
    {
        const auto& r = state.coverage[k];
        for (size_t c = 0; c < channels.size(); ++c)
        {
            if (state.audio[k].empty()) break;
            const auto& src = state.audio[k][std::min (c, state.audio[k].size() - 1)];
            const int64_t n = std::min<int64_t> ((int64_t) src.size(), state.length - r.first);
            if (n > 0)
                std::copy (src.begin(), src.begin() + n, channels[c].begin() + r.first);
        }
    }

    doc.loadWithTrack (std::move (channels), state.sampleRate, std::move (state.frames));
    doc.restoreEdits (state.edits);
    renderer.prepare (doc);
    curves = doc.buildCurves();
    renderer.renderAll (doc, curves, out);
    docInitialised = true;

    for (const auto& r : state.coverage)
        addCoverage (r);
    publish (coverage);
    publishSnapshot();
}

//==============================================================================
void EditSession::addCoverage (SampleRange r)
{
    if (r.second <= r.first) return;
    coverage.push_back (r);
    std::sort (coverage.begin(), coverage.end());
    std::vector<SampleRange> merged;
    for (const auto& x : coverage)
    {
        if (! merged.empty() && x.first <= merged.back().second)
            merged.back().second = std::max (merged.back().second, x.second);
        else
            merged.push_back (x);
    }
    coverage = std::move (merged);
}

void EditSession::publish (std::vector<SampleRange> ranges)
{
    if (ranges.empty() || ! docInitialised)
        return;

    std::sort (ranges.begin(), ranges.end());
    std::vector<size_t> chunks;
    for (const auto& r : ranges)
    {
        if (r.second <= r.first) continue;
        const auto a = (size_t) (std::max<int64_t> (0, r.first) / kChunk);
        const auto b = (size_t) ((r.second - 1) / kChunk);
        for (size_t c = a; c <= b && c < tableSize; ++c)
            chunks.push_back (c);
    }
    std::sort (chunks.begin(), chunks.end());
    chunks.erase (std::unique (chunks.begin(), chunks.end()), chunks.end());

    const auto& src = doc.sourceAudio();
    const int64_t length = doc.numSamples();
    const int nc = doc.numChannels();

    for (const size_t idx : chunks)
    {
        const int64_t cs = (int64_t) idx * kChunk, ce = cs + kChunk;
        auto chunk = std::make_unique<PlaybackChunk>();
        chunk->numChannels = nc;
        chunk->covered.assign (kChunk, 0);

        for (const auto& r : coverage)
        {
            if (r.second <= cs || r.first >= ce) continue;
            for (int64_t t = std::max (cs, r.first); t < std::min (ce, r.second); ++t)
                chunk->covered[(size_t) (t - cs)] = 1;
            chunk->anyCovered = true;
        }

        if (! chunk->anyCovered)
        {
            if (auto* old = table[idx].exchange (nullptr, std::memory_order_acq_rel))
                retire (old);
            continue;
        }

        chunk->src.assign ((size_t) nc, std::vector<float> (kChunk, 0.0f));
        const int64_t avail = std::clamp<int64_t> (length - cs, 0, kChunk);
        for (int c = 0; c < nc; ++c)
            std::copy (src[(size_t) c].begin() + cs, src[(size_t) c].begin() + cs + avail, chunk->src[(size_t) c].begin());

        const int64_t outAvail = out.empty() ? 0 : std::clamp<int64_t> ((int64_t) out[0].size() - cs, 0, kChunk);
        for (int c = 0; c < nc && ! chunk->edited; ++c)
            for (int64_t k = 0; k < outAvail; ++k)
                if (std::memcmp (&out[(size_t) c][(size_t) (cs + k)], &chunk->src[(size_t) c][(size_t) k], sizeof (float)) != 0)
                {
                    chunk->edited = true;
                    break;
                }

        if (chunk->edited)
        {
            chunk->delta.assign ((size_t) nc, std::vector<float> (kChunk, 0.0f));
            for (int c = 0; c < nc; ++c)
                for (int64_t k = 0; k < outAvail; ++k)
                    chunk->delta[(size_t) c][(size_t) k] = out[(size_t) c][(size_t) (cs + k)] - chunk->src[(size_t) c][(size_t) k];
        }

        if (auto* old = table[idx].exchange (chunk.release(), std::memory_order_acq_rel))
            retire (old);
    }
}

void EditSession::publishSnapshot()
{
    auto s = std::make_shared<SessionSnapshot>();
    if (docInitialised)
    {
        s->sampleRate = doc.sampleRate();
        s->hop = doc.pitchTrack().hop;
        s->length = doc.numSamples();
        s->notes = doc.notes();
        s->coverage = coverage;
        const auto& frames = doc.pitchTrack().frames;
        s->pitch.resize (frames.size());
        s->outPitch.resize (frames.size());
        for (size_t f = 0; f < frames.size(); ++f)
        {
            s->pitch[f] = frames[f].midi;
            const float shift = f < curves.shift.size() ? curves.shift[f] : 0.0f;
            s->outPitch[f] = frames[f].midi >= 0.0f ? frames[f].midi + shift : -1.0f;
        }
        s->map = curves.map;
        s->canUndo = doc.canUndo();
        s->canRedo = doc.canRedo();
        s->hasBackup = doc.hasCaptureBackup();
        s->backupRange = doc.captureBackupRange();
    }
    std::lock_guard<std::mutex> lock (snapshotMutex);
    s->revision = ++snapshotRevision;
    snap = std::move (s);
}

void EditSession::retire (PlaybackChunk* c)
{
    retired.push_back ({ Clock::now(), c });
}

void EditSession::freeRetired (bool all)
{
    std::lock_guard<std::mutex> lock (docMutex);
    const auto now = Clock::now();
    for (size_t i = 0; i < retired.size();)
    {
        if (all || std::chrono::duration<double> (now - retired[i].first).count() >= kRetireSec)
        {
            delete retired[i].second;
            retired[i] = retired.back();
            retired.pop_back();
        }
        else
            ++i;
    }
}

//==============================================================================
// Message-thread API

bool EditSession::undo()
{
    bool ok;
    {
        std::lock_guard<std::mutex> lock (docMutex);
        ok = docInitialised && doc.undo();
    }
    if (ok) requestRender();
    return ok;
}

bool EditSession::redo()
{
    bool ok;
    {
        std::lock_guard<std::mutex> lock (docMutex);
        ok = docInitialised && doc.redo();
    }
    if (ok) requestRender();
    return ok;
}

TempoInfo EditSession::tempo() const noexcept
{
    TempoInfo t;
    t.bpm = tempoBpm.load (std::memory_order_relaxed);
    t.ppqAnchor = tempoPpq.load (std::memory_order_relaxed);
    t.sampleAnchor = tempoSample.load (std::memory_order_relaxed);
    t.numerator = std::max (1, tempoNum.load (std::memory_order_relaxed));
    t.denominator = std::max (1, tempoDen.load (std::memory_order_relaxed));
    return t;
}

void EditSession::startPreview (int64_t from, int64_t to, bool loop)
{
    from = std::max<int64_t> (0, from);
    if (to <= from)
        return;
    previewFrom.store (from, std::memory_order_relaxed);
    previewTo.store (to, std::memory_order_relaxed);
    previewLoop.store (loop, std::memory_order_relaxed);
    previewPos.store (from, std::memory_order_relaxed);
    previewActive.store (true, std::memory_order_relaxed);
    previewSerial.store (previewCommandCounter.fetch_add (1) + 1, std::memory_order_release);
}

void EditSession::stopPreview()
{
    previewActive.store (false, std::memory_order_relaxed);
    previewStopSerial.store (previewCommandCounter.fetch_add (1) + 1, std::memory_order_release);
}

void EditSession::restorePreviousCapture()
{
    restoreRequested.store (true);
    wake();
}

void EditSession::clearAll()
{
    clearRequested.store (true);
    wake();
}

std::shared_ptr<const SessionSnapshot> EditSession::snapshot() const
{
    std::lock_guard<std::mutex> lock (snapshotMutex);
    return snap;
}

uint64_t EditSession::audioRevision() const
{
    std::lock_guard<std::mutex> lock (docMutex);
    return docInitialised ? doc.audioRevision() : 0;
}

SessionState EditSession::exportState() const
{
    std::lock_guard<std::mutex> lock (docMutex);
    SessionState s;
    if (! docInitialised || coverage.empty())
        return s;

    s.sampleRate = doc.sampleRate();
    s.numChannels = doc.numChannels();
    s.length = doc.numSamples();
    s.coverage = coverage;
    const auto& src = doc.sourceAudio();
    for (const auto& r : coverage)
    {
        std::vector<std::vector<float>> part ((size_t) s.numChannels);
        const int64_t a = std::clamp<int64_t> (r.first, 0, s.length), b = std::clamp<int64_t> (r.second, a, s.length);
        for (int c = 0; c < s.numChannels; ++c)
            part[(size_t) c].assign (src[(size_t) c].begin() + a, src[(size_t) c].begin() + b);
        s.audio.push_back (std::move (part));
    }
    s.frames = doc.pitchTrack().frames;
    s.edits = doc.serialiseEdits();
    s.audioRevision = doc.audioRevision();
    return s;
}

std::string EditSession::exportEdits() const
{
    std::lock_guard<std::mutex> lock (docMutex);
    return docInitialised ? doc.serialiseEdits() : std::string();
}

void EditSession::importState (SessionState state)
{
    {
        std::lock_guard<std::mutex> lock (commandMutex);
        importRequest = std::make_unique<SessionState> (std::move (state));
    }
    importPending.store (true);
    wake();
}

bool EditSession::waitUntilIdle (double timeoutSeconds)
{
    const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration> (std::chrono::duration<double> (timeoutSeconds));
    int calm = 0;
    while (Clock::now() < deadline)
    {
        const bool idle = fifo.isEmpty() && pendingCount.load() == 0 && ! renderRequested.load() && ! importPending.load()
                          && ! restoreRequested.load() && ! clearRequested.load() && ! workerBusy.load();
        calm = idle ? calm + 1 : 0;
        if (calm >= 3)
            return true;
        wake();
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    return false;
}

} // namespace claudetune::edit
