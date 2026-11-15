#ifndef TIMELINE_BUFFER_H
#define TIMELINE_BUFFER_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "audio_time.h"
#include "log_sink.h"

/*
 * lock-free SPSC ring of int16 samples (interleaved frames); positions are
 * non-wrapping 64-bit counters so empty/full are unambiguous without spare slot
 */
class SpscRing {
public:
    explicit SpscRing(size_t capacitySamples)
        : mCapacity(capacitySamples), mBuf(new int16_t[capacitySamples]) {}

    size_t available() const {  // samples readable
        return mWrite.load(std::memory_order_acquire) - mRead.load(std::memory_order_acquire);
    }

    // producer: push up to n samples, drops what doesn't fit
    size_t write(const int16_t *src, size_t n) {
        const size_t w = mWrite.load(std::memory_order_relaxed);
        const size_t r = mRead.load(std::memory_order_acquire);
        const size_t freeSpace = mCapacity - (w - r);
        if (n > freeSpace) n = freeSpace;
        const size_t wi = w % mCapacity;
        const size_t first = (n < mCapacity - wi) ? n : (mCapacity - wi);
        memcpy(&mBuf[wi], src, first * sizeof(int16_t));
        if (n > first) memcpy(&mBuf[0], src + first, (n - first) * sizeof(int16_t));
        mWrite.store(w + n, std::memory_order_release);
        return n;
    }

    // consumer: pop up to n samples
    size_t read(int16_t *dst, size_t n) {
        const size_t r = mRead.load(std::memory_order_relaxed);
        const size_t w = mWrite.load(std::memory_order_acquire);
        const size_t avail = w - r;
        if (n > avail) n = avail;
        const size_t ri = r % mCapacity;
        const size_t first = (n < mCapacity - ri) ? n : (mCapacity - ri);
        memcpy(dst, &mBuf[ri], first * sizeof(int16_t));
        if (n > first) memcpy(dst + first, &mBuf[0], (n - first) * sizeof(int16_t));
        mRead.store(r + n, std::memory_order_release);
        return n;
    }

    // consumer: discard up to n oldest samples
    void skip(size_t n) {
        const size_t r = mRead.load(std::memory_order_relaxed);
        const size_t w = mWrite.load(std::memory_order_acquire);
        const size_t avail = w - r;
        if (n > avail) n = avail;
        mRead.store(r + n, std::memory_order_release);
    }

private:
    const size_t mCapacity;
    std::unique_ptr<int16_t[]> mBuf;
    std::atomic<size_t> mWrite{0};
    std::atomic<size_t> mRead{0};
};

/*
 * adaptive jitter cushion tuner, NetEQ-inspired but simplified: decaying histogram of
 * packet jitter, cushion = high percentile of it + 1 packet of margin.
 * observe() is producer-thread only; result published via atomic, consumer reads it live
 */
class DelayTracker {
public:
    // staticCushionMs > 0: fixed cushion, tuner disabled
    // staticCushionMs <= 0: tune between MIN_CUSHION_MS and MAX_CUSHION_MS
    DelayTracker(int sampleRate, int channels, int staticCushionMs, int percentilePct)
        : mSampleRate(sampleRate), mChannels(channels),
          mAdaptive(staticCushionMs <= 0),
          mFloor(msToSamples(sampleRate, channels, mAdaptive ? MIN_CUSHION_MS : staticCushionMs)),
          mCeil(msToSamples(sampleRate, channels, mAdaptive ? MAX_CUSHION_MS : staticCushionMs)),
          mPercentilePct(percentilePct),
          mTuned(mFloor) {}

    // ptsNs: packet presentation time; arrivalNs: local arrival time; durNs: packet duration
    void observe(int64_t ptsNs, int64_t arrivalNs, int64_t durNs) {
        if (!mAdaptive) return;

        const int64_t raw = arrivalNs - ptsNs; // arrival latency
        // jitter = latency relative to min latency seen so far (mBaseNs)
        if (!mHaveBase) { mBaseNs = raw; mHaveBase = true; }
        else if (raw < mBaseNs) mBaseNs = raw;
        // creep mBaseNs up to follow clock drift between the sender's pts and our
        // arrival rate. this has to be time-based: a per-packet fraction is a lag whose
        // time constant is ~1024 packets, so the measured delay settles at
        // (drift per second x 10s) — a sender whose pts runs a few percent fast then
        // inflates the cushion into the hundreds of ms, and the cushion depth is exactly
        // how long audio keeps playing after the sender stops (the tail). following over
        // BASE_FOLLOW_NS makes that error drift x 2s (a few ms) instead, while transient
        // jitter is still filtered because any early packet snaps the base back down
        if (mLastArrivalNs != 0) {
            const int64_t dt = arrivalNs - mLastArrivalNs;
            if (dt > 0 && raw > mBaseNs) {
                const int64_t creep = (raw - mBaseNs) * dt / BASE_FOLLOW_NS;
                mBaseNs += creep > 0 ? creep : 1;
            }
        }
        mLastArrivalNs = arrivalNs;
        int64_t rel = raw - mBaseNs;
        if (rel < 0) rel = 0;

        int bin = (int)(rel / BUCKET_NS);
        if (bin >= NBUCKETS) bin = NBUCKETS - 1;

        // decay: each bin bleeds 1/2^FORGET_SHIFT per packet, current bin topped up by
        // ONE/2^FORGET_SHIFT, so total converges to ONE
        int64_t sum = 0;
        for (int i = 0; i < NBUCKETS; i++) {
            mHist[i] -= (mHist[i] + (1 << FORGET_SHIFT) - 1) >> FORGET_SHIFT;
            sum += mHist[i];
        }
        mHist[bin] += ONE >> FORGET_SHIFT;
        sum += ONE >> FORGET_SHIFT;

        // cushion = smallest delay whose cumulative histogram share reaches percentile
        const int64_t thresh = sum * mPercentilePct / 100;
        int64_t cum = 0; int b = 0;
        for (; b < NBUCKETS - 1; b++) { cum += mHist[b]; if (cum >= thresh) break; }
        // bin is lower bound on delay: take top edge + margin. margin must keep output
        // fed for one data callback and tide us over until next packet
        const int64_t marginNs = std::max(durNs, mOutputBufferNs.load(std::memory_order_relaxed));
        const int64_t targetNs = (int64_t)(b + 1) * BUCKET_NS + marginNs;
        size_t samples = (size_t)(targetNs * mSampleRate / NS_PER_SEC) * mChannels;
        if (samples < mFloor) samples = mFloor;
        if (samples > mCeil) samples = mCeil;
        mTuned.store(samples, std::memory_order_relaxed);
    }

    // reset clock base after discontinuity or clock shift/resync so it doesn't poison
    // jitter calculations; producer-thread only (same as observe())
    void reanchor() { mHaveBase = false; mLastArrivalNs = 0; }

    // latest tuned cushion in samples; any thread
    size_t target() const { return mTuned.load(std::memory_order_relaxed); }

    void noteOutputBufferFrames(int frames) {
        mOutputBufferNs.store((int64_t)frames * NS_PER_SEC / mSampleRate, std::memory_order_relaxed);
    }

    // largest cushion tuner can reach, in samples
    size_t ceil() const { return mCeil; }

private:
    static size_t msToSamples(int sampleRate, int channels, int ms) {
        return (size_t)sampleRate * ms / 1000 * channels;
    }

    static constexpr int MIN_CUSHION_MS = 0;             // algorithm already enforces effective floor
    static constexpr int MAX_CUSHION_MS = 1000;
    static constexpr int BUCKET_MS = 5;                  // histogram granularity
    static constexpr int NBUCKETS = MAX_CUSHION_MS / BUCKET_MS;
    static constexpr int64_t BUCKET_NS = (int64_t)BUCKET_MS * 1000000LL;
    static constexpr int FORGET_SHIFT = 8;               // ~1/256 decay per packet (~6s @ 23ms)
    // time constant over which mBaseNs follows a sustained clock drift (see observe)
    static constexpr int64_t BASE_FOLLOW_NS = 2'000'000'000LL;
    static constexpr int64_t ONE = 1 << 20;              // histogram fixed-point unit

    const int mSampleRate;
    const int mChannels;
    const bool mAdaptive;                // false = fixed cushion (tuner disabled)
    const size_t mFloor;
    const size_t mCeil;
    const int mPercentilePct;            // arrival-delay percentile cushion targets
    std::atomic<size_t> mTuned;
    std::atomic<int64_t> mOutputBufferNs{0};  // output->producer: oboe output buffer duration
    int64_t mHist[NBUCKETS] = {};
    int64_t mBaseNs = 0;
    int64_t mLastArrivalNs = 0;          // for the time-based base follow
    bool mHaveBase = false;
};

// cumulative diagnostic counters, bumped from producer/consumer, read from stats thread
class TimelineMetrics {
public:
    // packed: nests into AudioDebugData with fixed layout
    struct __attribute__((packed)) Debug {
        uint32_t trims, drops, silences, underruns;  // cumulative; 32-bit to avoid wrap
    };

    void countTrim()     { mTrims.fetch_add(1, std::memory_order_relaxed); }
    void countDrop()     { mDrops.fetch_add(1, std::memory_order_relaxed); }
    void countSilence()  { mSilences.fetch_add(1, std::memory_order_relaxed); }
    void countUnderrun() { mUnderruns.fetch_add(1, std::memory_order_relaxed); }
    void countTailCut()  { mTailCuts.fetch_add(1, std::memory_order_relaxed); }

    // tail cuts are not part of the packed debug struct (the Java overlay mirrors it
    // byte for byte); they are logged, and read back here by the native harness
    uint32_t tailCuts() const { return mTailCuts.load(std::memory_order_relaxed); }

    // any thread
    Debug debugInfo() const {
        Debug d{};
        d.trims = mTrims.load(std::memory_order_relaxed);
        d.drops = mDrops.load(std::memory_order_relaxed);
        d.silences = mSilences.load(std::memory_order_relaxed);
        d.underruns = mUnderruns.load(std::memory_order_relaxed);
        return d;
    }

private:
    std::atomic<uint32_t> mTrims{0};      // backlog exceeded cap and was trimmed
    std::atomic<uint32_t> mDrops{0};      // late packets dropped, pts slot already passed
    std::atomic<uint32_t> mSilences{0};   // gaps filled with silence
    std::atomic<uint32_t> mUnderruns{0};  // ring ran dry, output padded with silence
    std::atomic<uint32_t> mTailCuts{0};   // stale tail dropped because the sender went quiet
};

/*
 * Timeline-synchronized playout buffer: absorbs jitter, clock drift and clock resyncs,
 * syncs audio to its presentation-time tag.
 *
 * One concurrent reader + one concurrent writer OK; multiple readers or writers NOT safe.
 * samples = frames * channels
 */
class TimelineBuffer {
public:
    // cushion policy lives in mTracker; timeline reads mTracker.target() live
    TimelineBuffer(int sampleRate, int channels, int staticCushionMs, int percentilePct,
                   LogSink *log = nullptr)
        : mSampleRate(sampleRate),
          mChannels(channels),
          mStaticCushion(staticCushionMs > 0),  // fixed cushion = low-latency session
          mTracker(sampleRate, channels, staticCushionMs, percentilePct),
          mLog(log),
          // ring can't be reallocated mid-stream: size for worst-case backlog, i.e. cap
          // of largest reachable cushion + one throttle window of overshoot (trims are
          // throttled), 2x slack
          mRing(2 * capOf(mTracker.ceil()) + (size_t)sampleRate * TRIM_THROTTLE_MS / 1000 * channels) {}

    void noteOutputBufferFrames(int frames) { mTracker.noteOutputBufferFrames(frames); }

    // consumer->diagnostics: measured presentation latency of the output stream, i.e. from
    // the moment read() hands samples over to the moment the device actually plays them
    // (from the audio API's own timestamp, for half of a "sound keeps going" report our
    // queue depth cannot show)
    void noteOutputLatencyNs(int64_t ns) { mOutLatencyNs.store(ns, std::memory_order_relaxed); }

    // stale tails dropped so far; diagnostics only (see TimelineMetrics::tailCuts)
    uint32_t tailCuts() const { return mMetrics.tailCuts(); }

    // flush requests rejected because the sender had just come back from a pause; diagnostics
    int resumeFlushes() const { return mResumeFlushes.load(std::memory_order_relaxed); }

    // producer: push samples
    void write(const int16_t *pcm, size_t samples, int64_t ptsNs) {
        if (samples == 0) return;
        // liveness probe for the consumer: any packet means the sender is still
        // feeding us, regardless of whether we end up playing it
        const int64_t arrivedAt = monoNs();
        mLastWriteNs.store(arrivedAt, std::memory_order_relaxed);
        if (mFirstWriteNs.load(std::memory_order_relaxed) == 0)
            mFirstWriteNs.store(arrivedAt, std::memory_order_relaxed);
        // level snapshot for the consumer's "did content actually end" test. a per-packet
        // peak test is not enough: iOS keeps sending packets after the app is left, and
        // those carry dither/low-level noise that a single-sample threshold calls "sound",
        // so the tail cut never fires. the window share below is what actually tracks
        // "is there any content left"
        int32_t peak = 0;
        const int32_t audible = countSignal(pcm, samples, &peak);
        // sender came back after a real pause (not a packet-interval hiccup): remember it.
        // requestFlush() uses this to reject a flush that would otherwise cut the first
        // frames of the resumed sound (see RESUME_FLUSH_GRACE_NS). the reference is the
        // last packet that *carried sound*, not the last packet: while a mirror video is
        // paused iOS keeps the audio stream alive with silence, so the packet gap this
        // test used to look for never appears — yet the video side still reports the
        // resume as a pts jump and asks for a flush that would land on fresh audio
        const int64_t prevAudible = mLastAudibleAtNs.load(std::memory_order_relaxed);
        if (audible > 0 && prevAudible != 0 && arrivedAt - prevAudible >= RESUME_SILENCE_NS) {
            mResumedAtNs.store(arrivedAt, std::memory_order_release);
            mResumeOutLogged.store(false, std::memory_order_relaxed);
            if (mLog) {
                mLog->info("audio resumed after %.0fms of silence, %.0fms queued, %.0fms cushion",
                           (arrivedAt - prevAudible) / 1e6, backlogMs(), tunedMs());
            }
        }
        if (audible > 0) mLastAudibleAtNs.store(arrivedAt, std::memory_order_relaxed);
        noteLevel(audible, peak, (int64_t)samples);
        // pts 0 = sender clock not NTP synced yet
        if (ptsNs == 0) {
            mRing.write(pcm, samples);
            return;
        }
        const int64_t durNs = samples * NS_PER_SEC / mChannels / mSampleRate;

        // consumer underran since last write: it already played the gap as real-time
        // silence, re-anchor so we don't also insert it here
        if (mUnderran.exchange(false, std::memory_order_relaxed)) {
            mExpectedPtsNs = ptsNs;
        }
        const int64_t gapNs = ptsNs - mExpectedPtsNs;

        // large gap: possible discontinuity or clock shift. reanchor() must precede
        // observe() so the reset applies to this observation
        if (gapNs > MAX_GAP_NS || gapNs < -MAX_GAP_NS) mTracker.reanchor();

        // no-op in static-cushion mode
        mTracker.observe(ptsNs, monoNs(), durNs);

        if (gapNs > MAX_GAP_NS || gapNs < -MAX_GAP_NS) {
            // discontinuity: drop whatever is still buffered from the old timeline,
            // then write immediately. without the flush the ring plays out the old
            // content first (heard as the previous video's audio for up to the whole
            // backlog) before the new sound starts
            requestFlush();
            // new sound after long silence resets backlog to ideal size (less
            // latency); on clock resync we keep the sound
            if (mLog) {
                mLog->warn("audio timeline discontinuity: gap %.0fms, %.0fms buffered",
                           gapNs / 1e6, backlogMs());
            }
        } else if (gapNs > SLACK_NS) {
            // sender left gap: reproduce as silence so timing is exact
            const size_t silenceFrames = (size_t)(gapNs * mSampleRate / NS_PER_SEC);
            writeSilenceFrames(silenceFrames);
            mMetrics.countSilence();
            if (mLog && gapNs > GAP_LOG_NS) {
                mLog->info("audio gap %.0fms filled with silence", gapNs / 1e6);
            }
        } else if (gapNs < -SLACK_NS) {
            // slot already passed (late/overlap): drop
            mMetrics.countDrop();
            if (mLog && -gapNs > GAP_LOG_NS) {
                mLog->info("audio packet %.0fms late dropped", -gapNs / 1e6);
            }
            return;
        }
        mRing.write(pcm, samples);
        mExpectedPtsNs = ptsNs + durNs;
    }

    // consumer: pop frames*channels samples into out, silence-padded on underrun
    void read(int16_t *out, int32_t frames) {
        const size_t need = (size_t)frames * mChannels;
        const size_t tuned = mTracker.target();
        const int64_t now = monoNs();

        // producer asked to drop buffered audio (stream discontinuity / sender FLUSH):
        // discard the old timeline's tail here on the consumer thread so skip() and
        // the priming state stay single-threaded. anything the producer writes after
        // the request simply refills the ring during re-prime, so no new audio is lost
        if (mFlushRequested.exchange(false, std::memory_order_acquire)) {
            const size_t dropped = mRing.available();
            mRing.skip(dropped);
            mPriming = true;
            mPrimeSilenceFrames = 0;
            mAboveCapSinceNs = 0;
            // the old timeline is gone; the anchor maps the previous content's pts
            // and must not keep steering the video renderer until the next prime
            // publishes a fresh one
            invalidateAnchor();
            if (mLog) {
                mLog->warn("audio flush: dropped %.0fms of buffered audio",
                           dropped / (double)mChannels * 1000.0 / mSampleRate);
            }
        }

        // sender went quiet while we still hold samples from before: that audio is
        // stale by definition, and playing it out is heard as sound that keeps going
        // after the phone stopped (its length is exactly the buffer depth)
        cutStaleTail(now);

        // one line per session so a "sound keeps going" report can be tied to a number:
        // the depth of the buffer IS the length of any tail
        if (!mSettledLogged && mFirstWriteNs.load(std::memory_order_relaxed) != 0
                && now - mFirstWriteNs.load(std::memory_order_relaxed) > SETTLE_REPORT_NS) {
            mSettledLogged = true;
            if (mLog) {
                const Debug d = debugInfo();
                mLog->info("audio buffer settled: %ums queued, %ums cushion, %u underruns, %u trims",
                           d.backlogMs, d.tunedCushionMs, d.metrics.underruns, d.metrics.trims);
            }
        }

        // what the sender is feeding us, every couple of seconds: the evidence a tail
        // report needs (see reportLevel)
        reportLevel(now);

        // bound latency: cap tracks tuned cushion live so a calmed link sheds latency
        // without an underrun. each trim costs a glitch, so be conservative: trim only
        // after backlog stayed above cap for the sustain window (skip transient spikes),
        // at most once per the throttle window, and not within that window of an underrun
        // (trimming while rebuilding cushion is counterproductive). low-latency (static
        // cushion) sessions use a tight cap + short windows: there a burst backlog is
        // stale audio (previous video) and must go, while music (adaptive) prefers
        // stability over shaving latency
        const size_t avail = mRing.available();
        if (avail > capOf(tuned)) {
            if (mAboveCapSinceNs == 0) mAboveCapSinceNs = now;
        } else {
            mAboveCapSinceNs = 0;
        }
        if (mAboveCapSinceNs != 0 && now - mAboveCapSinceNs >= trimSustainNs()
                && now - mLastTrimBlockNs >= trimThrottleNs()) {
            mRing.skip(avail - tuned);
            mMetrics.countTrim();
            mLastTrimBlockNs = now;
            mAboveCapSinceNs = 0;
        }

        // prebuffer: hold output until cushion fills, builds jitter headroom
        if (mPriming) {
            const size_t buffered = mRing.available();
            const size_t primeTarget = primeTargetSamples(tuned);
            // time the wait only while holding partial data; reset on empty ring so the
            // producer gets the full window once a sound starts, otherwise we could time
            // out on pre-sound silence and underrun the instant the first frame lands
            if (buffered == 0) mPrimeSilenceFrames = 0;
            else mPrimeSilenceFrames += (uint32_t)frames;
            // short sound can end before filling cushion; after holding partial data
            // through 2x cushion of silence the sound is complete: play it
            const uint32_t starveFrames = (uint32_t)(2 * primeTarget / mChannels);
            const bool starved = buffered > 0 && mPrimeSilenceFrames >= starveFrames;
            // also keep priming on empty ring: with 0 cushion `buffered < tuned` is
            // never true, so we'd otherwise underrun on every empty read between sounds
            if (buffered == 0 || (buffered < primeTarget && !starved)) {
                memset(out, 0, need * sizeof(int16_t));
                return;
            }
            mPriming = false;
            mPrimeSilenceFrames = 0;
            // the sender may have front-loaded the stream start (or a burst refilled
            // the ring past the cushion): shed the excess right here instead of
            // playing it out as added latency. no-op when the ring holds ~cushion
            const size_t primed = mRing.available();
            const size_t keep = std::min(primed, tuned);
            // publish the playout anchor: the first sample about to be played sits
            // `keep` samples behind the producer's write head, so its pts is the
            // expected write-head pts minus that backlog. the video renderer uses
            // this to align its NTP-scheduled frames to the audio clock (Oboe is
            // the master clock); without it the two sides each anchor to their own
            // stream start and drift apart by the cushion. snapshot error is at
            // most one audio packet (producer publishes ring before pts).
            publishAnchor(mExpectedPtsNs.load(std::memory_order_acquire) -
                          (int64_t)(keep * NS_PER_SEC / mChannels / mSampleRate));
            if (primed > keep) mRing.skip(primed - keep);
        }

        const size_t got = mRing.read(out, need);
        if (got < need) {
            memset(out + got, 0, (need - got) * sizeof(int16_t));
            mPriming = true;
            mLastTrimBlockNs = now;  // hold off trims while rebuilding
            mUnderran.store(true, std::memory_order_relaxed);  // producer re-anchors
            mMetrics.countUnderrun();
        }
        watchOutput(now, out, need);
    }

    // call after discontinuity or clock shift/resync; producer-side, must not run
    // concurrently with write()
    void reanchorTracker() { mTracker.reanchor(); }

    /*
     * playout anchor: maps the pts of the first sample played out to the wall
     * clock at which it was played. published once per (re)prime on the consumer
     * thread; epoch bumps on every publish so the video renderer can follow
     * re-anchors (flush, underrun recovery, pause/resume). seq counter guards the
     * fields: odd = a publish is in flight.
     *
     * this is the shared clock the two streams were missing: audio plays out on
     * the Oboe (master) clock, video frames are scheduled against this anchor so
     * both render the same NTP instant at the same wall-clock instant.
     */
    bool playoutAnchor(int64_t *ptsNs, int64_t *wallNs, uint32_t *epoch) const {
        for (int attempt = 0; attempt < 4; attempt++) {
            const uint32_t s1 = mAnchorSeq.load(std::memory_order_acquire);
            if (s1 & 1u) continue;  // publish in flight, retry
            const bool valid = mAnchorValid;
            const int64_t pts = mAnchorPtsNs;
            const int64_t wall = mAnchorWallNs;
            const uint32_t ep = mAnchorEpoch;
            if (s1 == mAnchorSeq.load(std::memory_order_acquire)) {
                if (!valid) return false;
                *ptsNs = pts;
                *wallNs = wall;
                *epoch = ep;
                return true;
            }
        }
        return false;  // contended: caller keeps its current anchor
    }

    // producer-side: drop all buffered audio + rebuild cushion before the next read.
    // used on stream discontinuity (new media timeline: the ring may still hold
    // seconds of the previous content, which would otherwise play out first) and on
    // sender FLUSH. the flag is honored by the consumer so skip() stays single-threaded
    //
    // two independent watchers report the *same* discontinuity: the audio stream's own
    // pts gap, and the video stream's pts jump. the second request lands a moment later,
    // by which time the consumer has already dropped the old timeline and is priming on
    // the new content — honouring it would throw that restart away and cost another full
    // cushion of silence, which is heard as the hiccup when playback resumes after a
    // pause. requests inside the coalescing window are therefore dropped
    void requestFlush() {
        const int64_t now = monoNs();
        // a flush right after the sender came back from a pause has nothing stale to drop:
        // the ring ran dry during the pause, so everything in it is the *new* sound. the
        // video side reports that same instant as a pts jump (the pause is a jump by
        // definition) and asks for a flush a few ms after the audio restarts — honouring
        // it cut the first 100-200ms of the resumed sound in half, which is heard as the
        // hiccup when playback resumes after a pause
        const int64_t resumedAt = mResumedAtNs.load(std::memory_order_acquire);
        if (resumedAt != 0 && now - resumedAt < RESUME_FLUSH_GRACE_NS) {
            if (mResumeFlushes.fetch_add(1, std::memory_order_relaxed) == 0 && mLog) {
                mLog->info("audio flush request ignored: audio resumed %.0fms ago "
                           "(nothing stale in the queue after a pause)", (now - resumedAt) / 1e6);
            }
            return;
        }
        const int64_t last = mLastFlushAtNs.load(std::memory_order_relaxed);
        if (last != 0 && now - last < FLUSH_COALESCE_NS) {
            if (mCoalescedFlushes.fetch_add(1, std::memory_order_relaxed) == 0 && mLog) {
                mLog->info("audio flush request ignored: a flush already ran %.0fms ago "
                           "(audio pts gap and video pts jump report one discontinuity)",
                           (now - last) / 1e6);
            }
            return;
        }
        mLastFlushAtNs.store(now, std::memory_order_relaxed);
        mFlushRequested.store(true, std::memory_order_release);
    }

    // rebuild prebuffer cushion before resuming, e.g. after output stream restart;
    // must not run concurrently with read()
    void reprime() { mPriming = true; }

    // drop buffered audio + rebuild cushion, so resume after pause doesn't play stale
    // tail; only while output callback is stopped (skip() is consumer-side)
    void flushAndReprime() {
        mRing.skip(mRing.available());
        mPriming = true;
    }

    // debug snapshot: backlog + tuned cushion (ms) + counters; reads only atomics, any thread
    struct __attribute__((packed)) Debug {
        uint16_t backlogMs;         // bounded by ring (few seconds)
        uint16_t tunedCushionMs;    // bounded by MAX_CUSHION_MS
        TimelineMetrics::Debug metrics;
    };
    Debug debugInfo() const {
        Debug d{};
        d.backlogMs = (uint16_t)(mRing.available() / mChannels * 1000 / mSampleRate);
        d.tunedCushionMs = (uint16_t)(mTracker.target() / mChannels * 1000 / mSampleRate);
        d.metrics = mMetrics.debugInfo();
        return d;
    }

private:

    // latency ceiling for cushion: adaptive sessions get a generous 2x cushion (music
    // prefers stability); static-cushion (low-latency mirroring) sessions cap at
    // cushion + TRIM_MARGIN_MS so burst backlog is shed instead of playing out as
    // stale audio. never below TRIM_FLOOR_MS so tiny cushion still gets slack before
    // a trim (trim costs a glitch)
    size_t capOf(size_t cushionSamples) const {
        const size_t margin = mStaticCushion
            ? (size_t)mSampleRate * TRIM_MARGIN_MS / 1000 * mChannels
            : cushionSamples;
        return std::max(cushionSamples + margin, (size_t)mSampleRate * TRIM_FLOOR_MS / 1000 * mChannels);
    }

    // how deep the ring must be before output starts. a static-cushion (mirror) session
    // races a picture that is already on screen and its sender keeps a lead of its own, so
    // waiting for the whole cushion on every restart (resume after pause, content switch,
    // codec restart) leaves the video moving while audio is still silent — that silence is
    // the hiccup heard when playback resumes. music (adaptive) keeps the full cushion:
    // there stability beats latency.
    size_t primeTargetSamples(size_t tuned) const {
        return mStaticCushion ? std::min(tuned, msToSamples(FAST_PRIME_MS)) : tuned;
    }

    // static (low-latency) sessions trim aggressively: a burst backlog there is the
    // previous video's audio, not jitter to absorb
    int64_t trimSustainNs() const {
        return (int64_t)(mStaticCushion ? TRIM_SUSTAIN_LOW_MS : TRIM_SUSTAIN_MS) * 1000000LL;
    }
    int64_t trimThrottleNs() const {
        return (int64_t)(mStaticCushion ? TRIM_THROTTLE_LOW_MS : TRIM_THROTTLE_MS) * 1000000LL;
    }

    /*
     * Tail cut. When the sender stops producing *content* (the phone's app went quiet,
     * the stream was torn down, a flush we did not see), whatever is still queued is
     * audio from *before* that moment: nobody will ever play it at the right time, and
     * playing it out is exactly the "sound keeps going" complaint.
     *
     * The trigger is the level window, NOT packet arrival. AirPlay keeps sending packets
     * after the user leaves an app, so "no packet for 250ms" never fires; and those
     * late packets still cross a single-sample threshold, so "no audible sample for
     * 250ms" never fired either — both left the stale tail playing. The window asks the
     * only question that matters: over the last ~200ms, did the sender carry any
     * content at all? Once it did not, anything still queued is by definition stale.
     *
     * Only fires when the leftover is deeper than the buffer's own latency bound
     * (capOf), never when it merely holds the cushion the jitter policy built. The
     * threshold used to be an absolute 400ms, which in a low-latency session is deeper
     * than the ring can ever hold (cushion + margin ~= 140ms): the cut was unreachable
     * dead code and every tail played out in full. Relative to the cap it fires exactly
     * when the trim would have but could not — the trim's throttle is held off by an
     * underrun, which is what lets a sender's flush burst park a second of audio here.
     *
     * Consumer thread only: skip() and the priming state stay single-threaded here.
     */
    void cutStaleTail(int64_t now) {
        if (!contentEnded()) {
            if (mEndedAtNs != 0) {                       // content came back: re-arm the report
                mEndedAtNs = 0;
                mPlayedOutLogged = false;
            }
            mIdle = false;                               // content flowing: re-arm for next time
            return;
        }
        if (mIdle) return;                               // already handled this silence
        mIdle = true;
        const size_t backlog = mRing.available();
        const size_t keep = msToSamples(TAIL_KEEP_MS);
        const int64_t lastWrite = mLastWriteNs.load(std::memory_order_relaxed);
        const int64_t audibleAt = mLastAudibleAtNs.load(std::memory_order_relaxed);
        int32_t peak = 0;
        int64_t audible = 0, total = 0;
        levelSnapshot(&peak, &audible, &total);
        const double pct = total > 0 ? audible * 100.0 / (double)total : 0.0;
        const size_t cutFloor = std::max(capOf(mTracker.target()), keep) +
                                msToSamples(TAIL_CUT_MIN_MS);
        const double windowMs = total / (double)mChannels * 1000.0 / mSampleRate;
        const double quietMs = (now - audibleAt) / 1e6;
        // which of the two contentEnded() branches fired decides how the line reads:
        // a frozen window next to "100% audible" would otherwise look self-contradictory
        const bool senderGone = now - lastWrite >= TAIL_IDLE_NS;
        // remember the state at the moment content ended: watchOutput() reports the
        // measured tail against it, and that single number says whether a "sound keeps
        // going" report is ours (deep queue) or somebody else's (our queue was short)
        mEndedAtNs = now;
        mPlayedOutLogged = false;
        mBacklogAtEndMs = backlogMs();
        const int64_t outNs = mOutLatencyNs.load(std::memory_order_relaxed);
        mOutLatencyAtEndMs = outNs >= 0 ? outNs / 1e6 : -1.0;
        if (mLog) {
            if (senderGone) {
                mLog->warn("audio end: sender stopped sending %.0fms ago, %.0fms queued, "
                           "%.0fms cushion, last sound %.0fms ago",
                           (now - lastWrite) / 1e6, mBacklogAtEndMs, tunedMs(), quietMs);
            } else {
                mLog->warn("audio end: window %.0fms holds %.2f%% audible, peak %.0f dBFS, "
                           "%.0fms queued, %.0fms cushion, last sound %.0fms ago",
                           windowMs, pct, peakToDbfs(peak), mBacklogAtEndMs, tunedMs(), quietMs);
            }
        }
        if (backlog <= cutFloor) {
            // short leftover: playing it is fine, but say so — a report of "sound
            // keeps going" needs to tell this apart from "the sender never stopped"
            if (mLog && backlog > msToSamples(TAIL_REPORT_MIN_MS)) {
                mLog->info("audio idle: %.0fms buffered is within the %.0fms cut floor, "
                           "playing it out (last sound %.0fms ago)",
                           backlogMs(), cutFloor / (double)mChannels * 1000.0 / mSampleRate,
                           quietMs);
            }
            return;
        }
        mRing.skip(backlog - keep);
        mMetrics.countTrim();
        mMetrics.countTailCut();
        if (mLog) {
            mLog->warn("audio tail cut: dropped %.0fms of stale audio, kept %.0fms "
                       "(last sound %.0fms ago, peak %.0f dBFS, %.2f%% audible)",
                       (backlog - keep) / (double)mChannels * 1000.0 / mSampleRate,
                       keep / (double)mChannels * 1000.0 / mSampleRate,
                       quietMs, peakToDbfs(peak), pct);
        }
    }

    /*
     * Tail measurement, output side. The one number a "sound keeps going after I left the
     * app" report cannot argue with: how long after the *last audible sample arrived* it
     * was actually handed to the audio device, and how long after we declared the content
     * over the output finally fell silent. Both are measured here, so:
     *
     *   - measured ~= backlog at content end + one output buffer -> the tail is ours, and
     *     it is bounded by capOf() (see cutStaleTail)
     *   - measured >> that -> the audio had already left our hands; whatever the user still
     *     hears sits in the device mixer / HDMI / TV, or never left the sender at all
     *
     * Consumer thread only.
     */
    void watchOutput(int64_t now, const int16_t *out, size_t samples) {
        int32_t peak = 0;
        if (countSignal(out, samples, &peak) > 0) {
            mLastAudibleReadNs = now;
            // resume accounting: how long the audible sound took to come back after the
            // sender started sending again. one line per resume, the number a "hiccup on
            // resume" report needs (cushion + output buffer + priming cost)
            const int64_t resumedAt = mResumedAtNs.load(std::memory_order_relaxed);
            if (resumedAt != 0 && !mResumeOutLogged.exchange(true, std::memory_order_relaxed)) {
                if (mLog) {
                    mLog->info("audio resume: first sound reached the output %.0fms after the "
                               "sender came back (%.0fms queued, %.0fms cushion)",
                               (now - resumedAt) / 1e6, backlogMs(), tunedMs());
                }
            }
            return;
        }
        if (mLastAudibleReadNs == 0 || mEndedAtNs == 0 || mPlayedOutLogged) return;
        mPlayedOutLogged = true;
        if (!mLog) return;
        const int64_t arrived = mLastAudibleAtNs.load(std::memory_order_relaxed);
        mLog->warn("audio end: output silent, last sound played %.0fms after it arrived "
                   "(%.0fms was queued at content end, %.0fms cushion, out=%s at that point), "
                   "%.0fms after content end",
                   (mLastAudibleReadNs - arrived) / 1e6, mBacklogAtEndMs, tunedMs(),
                   outLatencyText(mOutLatencyAtEndMs), (mLastAudibleReadNs - mEndedAtNs) / 1e6);
    }

    // "42ms" / "n/a": not every device exposes a presentation timestamp for its stream
    const char *outLatencyText(double ms) {
        static thread_local char buf[24];
        if (ms < 0) return "n/a";
        snprintf(buf, sizeof buf, "%.0fms", ms);
        return buf;
    }

    /*
     * Periodic level line. A "sound keeps going after I left the app" report needs one
     * number the buffer cannot supply: what the sender was actually sending. This prints
     * the received level every couple of seconds while audio flows, so a capture that
     * spans the moment the user leaves the app shows exactly when (and whether) the
     * sender's level dropped — and therefore whether the tail was ours or its.
     *
     * Consumer thread only.
     */
    void reportLevel(int64_t now) {
        if (!mLog) return;
        if (mLastLevelReportNs != 0 && now - mLastLevelReportNs < (int64_t)LEVEL_REPORT_NS_MS * 1000000LL) {
            return;
        }
        const int64_t first = mFirstWriteNs.load(std::memory_order_relaxed);
        if (first == 0) return;                          // stream never started
        mLastLevelReportNs = now;
        int32_t peak = 0;
        int64_t audible = 0, total = 0;
        levelSnapshot(&peak, &audible, &total);
        const double pct = total > 0 ? audible * 100.0 / (double)total : 0.0;
        // "queued" is only what WE hold; "out" is what the audio device still has to
        // play (measured from its presentation timestamp). A tail that survives a Wi-Fi
        // cut with 0ms queued has to show up in one of these two, or it never was local.
        mLog->info("audio level: peak %.0f dBFS, %.2f%% audible over %dms, %.0fms queued, out=%s",
                   peakToDbfs(peak), pct,
                   (int)(total / (double)mChannels * 1000.0 / mSampleRate), backlogMs(),
                   outLatencyText(mOutLatencyNs.load(std::memory_order_relaxed) / 1e6));
    }

    /*
     * Sliding level window over the packets the sender feeds us.
     *
     * Why not just "was the last packet audible": after the user leaves an app, AirPlay
     * keeps sending packets for a while, and those packets carry dither / a fade-out
     * tail / low-level room noise. Any single-sample threshold calls that "sound", so a
     * peak-based liveness test never says the content ended — which is exactly why the
     * reported "sound keeps going for a second" survived a peak-based cut.
     *
     * The window answers the question that matters: over the last ~200ms, what fraction
     * of samples carried signal? Real content is ~100%; a quiet sender is ~0%.
     *
     * Producer thread only for the array; the three atomics are what the consumer reads.
     */
    void noteLevel(int32_t audible, int32_t peak, int64_t samples) {
        mLevelAudibleSum += audible - mLevelRing[mLevelIdx].audible;
        mLevelSampleSum += samples - mLevelRing[mLevelIdx].samples;
        mLevelPeak = std::max(mLevelPeak, peak);
        mLevelRing[mLevelIdx] = {audible, (int32_t)samples};
        mLevelIdx = (mLevelIdx + 1) % LEVEL_WINDOW_PACKETS;
        mLevelAudibleSnap.store(mLevelAudibleSum, std::memory_order_relaxed);
        mLevelSampleSnap.store(mLevelSampleSum, std::memory_order_relaxed);
        mLevelPeakSnap.store(mLevelPeak, std::memory_order_relaxed);
    }

    // true when the sender has stopped carrying content. two ways that happens, and
    // both must be covered:
    //  1) no packet at all for a while — the window is frozen at its last value, so a
    //     window-only test would never fire (this is the plain teardown case)
    //  2) packets still arriving but carrying almost no signal — dither after a
    //     fade-out, or a quiet sender. here the window is the only thing that sees it,
    //     because every packet still crosses a per-sample threshold
    // safe from any thread (atomics only)
    bool contentEnded() const {
        const int64_t lastWrite = mLastWriteNs.load(std::memory_order_relaxed);
        if (lastWrite != 0 && monoNs() - lastWrite >= TAIL_IDLE_NS) return true;
        // a packet carrying real signal just landed: the window below may still be full of
        // the previous pause's silence, but content is demonstrably back. treating that as
        // "ended" would let the tail cut eat the first frames of the resumed sound
        const int64_t audibleAt = mLastAudibleAtNs.load(std::memory_order_relaxed);
        if (audibleAt != 0 && monoNs() - audibleAt < TAIL_IDLE_NS) return false;
        const int64_t total = mLevelSampleSnap.load(std::memory_order_relaxed);
        if (total <= 0) return false;                    // window not filled yet
        const int64_t audible = mLevelAudibleSnap.load(std::memory_order_relaxed);
        return audible * 100 < total * LEVEL_AUDIBLE_PCT;
    }

    // window snapshot for the periodic level line; safe from any thread
    void levelSnapshot(int32_t *peak, int64_t *audible, int64_t *total) const {
        *peak = mLevelPeakSnap.load(std::memory_order_relaxed);
        *audible = mLevelAudibleSnap.load(std::memory_order_relaxed);
        *total = mLevelSampleSnap.load(std::memory_order_relaxed);
    }

    // counts samples above the noise floor and reports the peak; one pass per packet,
    // same cost as the old boolean scan
    static int32_t countSignal(const int16_t *pcm, size_t samples, int32_t *peakOut) {
        int32_t audible = 0;
        int32_t peak = 0;
        for (size_t i = 0; i < samples; i++) {
            const int32_t v = pcm[i];
            const int32_t a = v < 0 ? -v : v;
            if (a > peak) peak = a;
            if (a > AUDIBLE_LEVEL) audible++;
        }
        *peakOut = peak;
        return audible;
    }

    static double peakToDbfs(int32_t peak) {
        if (peak <= 0) return LEVEL_DBFS_FLOOR;
        const double db = 20.0 * std::log10((double)peak / 32768.0);
        return db < LEVEL_DBFS_FLOOR ? LEVEL_DBFS_FLOOR : db;
    }

    // samples for ms of audio, interleaved
    size_t msToSamples(int ms) const {
        return (size_t)mSampleRate * (size_t)ms / 1000 * mChannels;
    }

    void writeSilenceFrames(size_t frames) {
        int16_t zeros[512] = {0};  // 256 stereo frames per chunk
        size_t samples = frames * mChannels;
        while (samples > 0) {
            const size_t n = std::min<size_t>(samples, 512);
            mRing.write(zeros, n);
            samples -= n;
        }
    }

    // consumer-thread only (published from read()); ptsNs == 0 means the sender's
    // clock is not NTP-synced yet, which cannot anchor a timeline
    void publishAnchor(int64_t ptsNs) {
        mAnchorSeq.fetch_add(1, std::memory_order_release);  // odd: writing
        mAnchorValid = ptsNs != 0;
        mAnchorPtsNs = ptsNs;
        mAnchorWallNs = monoNs();
        mAnchorEpoch++;
        mAnchorSeq.fetch_add(1, std::memory_order_release);  // even: stable
    }

    void invalidateAnchor() {
        mAnchorSeq.fetch_add(1, std::memory_order_release);
        mAnchorValid = false;
        mAnchorSeq.fetch_add(1, std::memory_order_release);
    }

    // milliseconds of audio currently buffered (producer or consumer thread)
    double backlogMs() const {
        return mRing.available() / (double)mChannels * 1000.0 / mSampleRate;
    }

    // current cushion target in ms (the depth the buffer tries to hold)
    double tunedMs() const {
        return mTracker.target() / (double)mChannels * 1000.0 / mSampleRate;
    }

    static constexpr int64_t SLACK_NS = 10'000'000LL;    // ignore <10ms gaps (NTP shift, rounding)
    static constexpr int64_t MAX_GAP_NS = 750'000'000LL; // >750ms = discontinuity, re-anchor
    static constexpr int64_t GAP_LOG_NS = 100'000'000LL; // log gaps larger than this (rare events)
    static constexpr int TRIM_FLOOR_MS = 30;             // min trim point even for tiny cushion
    static constexpr int TRIM_THROTTLE_MS = 2000;        // adaptive: min spacing between trims, and after underrun
    static constexpr int TRIM_SUSTAIN_MS = 2000;         // adaptive: backlog must exceed cap this long before trim
    static constexpr int TRIM_MARGIN_MS = 60;            // static: cap = cushion + this
    static constexpr int TRIM_THROTTLE_LOW_MS = 600;     // static: min spacing between trims
    // static: backlog must exceed cap this long before trim. 150ms is 7-15 packet
    // intervals (AAC-ELD 10ms .. AAC-LC 23ms), so anything that stays over the cap that
    // long is a burst the sender dumped, not jitter to absorb. this was 400ms, and that
    // 400ms was the reported "sound keeps going for a second": the sender's flush burst
    // sat in the ring for 400ms before being shed, on top of the cushion and the output
    // FIFO. the whole point of the static cap is that excess here is stale audio, so
    // waiting almost half a second to drop it defeated the design
    static constexpr int TRIM_SUSTAIN_LOW_MS = 150;      // static: backlog must exceed cap this long before trim

    // mirror sessions start playing on this much audio instead of the whole cushion (see
    // primeTargetSamples): 40ms is four AAC-ELD packets, well under the sender's own lead,
    // so a restart costs a few packet intervals of silence instead of the full cushion
    static constexpr int FAST_PRIME_MS = 40;
    // a second flush request inside this window belongs to a discontinuity that was already
    // handled by another watcher (see requestFlush)
    static constexpr int64_t FLUSH_COALESCE_NS = 500'000'000LL;

    // pause/resume: the sender going quiet for this long is a pause, not jitter. a flush
    // requested inside the grace window after it resumed is rejected — the queue holds
    // only new sound by then, and dropping it is the hiccup heard on resume
    static constexpr int64_t RESUME_SILENCE_NS = 250'000'000LL;
    static constexpr int64_t RESUME_FLUSH_GRACE_NS = 1'000'000'000LL;

    // tail cut: how long the sender must be quiet before the leftover is treated as
    // stale (much longer than any packet interval: 10-23ms for AAC-ELD/AAC-LC), how
    // much leftover is worth cutting, and how much to keep as a restart cushion
    static constexpr int64_t TAIL_IDLE_NS = 150'000'000LL;
    static constexpr int TAIL_CUT_MIN_MS = 60;
    static constexpr int TAIL_KEEP_MS = 40;
    static constexpr int TAIL_REPORT_MIN_MS = 100;       // log an idle episode only if it left audio behind
    static constexpr int AUDIBLE_LEVEL = 32;             // int16 amplitude that counts as sound
    static constexpr int64_t SETTLE_REPORT_NS = 5'000'000'000LL;  // one buffer report per session

    // level probe: a sliding window of what the sender is actually feeding us. the
    // tail cut needs "is there any content left", which a per-packet peak cannot
    // answer (dither after a fade-out keeps crossing AUDIBLE_LEVEL forever). the
    // window share also produces the one line of evidence a "sound keeps going"
    // report needs: what level the sender was sending, and for how long.
    static constexpr int LEVEL_WINDOW_PACKETS = 20;      // ~200ms at AAC-ELD 10ms packets
    static constexpr int LEVEL_REPORT_NS_MS = 2000;      // periodic level line while streaming
    static constexpr int LEVEL_AUDIBLE_PCT = 1;          // window share below this = no content
    static constexpr double LEVEL_DBFS_FLOOR = -90.0;    // reported for a silent window

    const int mSampleRate;
    const int mChannels;
    const bool mStaticCushion;           // fixed cushion: low-latency (mirroring) session
    DelayTracker mTracker;               // owns cushion policy; read live each read()
    SpscRing mRing;
    TimelineMetrics mMetrics;
    LogSink *mLog;                       // optional; logs rare transition events, any thread
    bool mPriming = true;                // stream start: buffer output to build cushion
    uint32_t mPrimeSilenceFrames = 0;    // consumer-only: silence frames output while priming with partial data
    int64_t mLastTrimBlockNs = 0;        // consumer-only: last trim/underrun time (trim throttle)
    int64_t mAboveCapSinceNs = 0;        // consumer-only: when backlog first exceeded cap (0 = under)
    std::atomic<bool> mUnderran{false};  // consumer->producer: underrun happened
    std::atomic<bool> mFlushRequested{false};  // producer->consumer: drop buffered audio + reprime
    std::atomic<int64_t> mLastFlushAtNs{0};    // producer: last honoured flush request
    std::atomic<int> mCoalescedFlushes{0};     // duplicate flush requests dropped (diagnostics)
    // pause/resume (see write/requestFlush/watchOutput)
    std::atomic<int64_t> mResumedAtNs{0};      // producer: last time the sender came back after a pause
    std::atomic<int> mResumeFlushes{0};        // flush requests rejected by the resume grace window
    std::atomic<bool> mResumeOutLogged{false}; // one resume-latency line per resume

    // write-head pts (producer writes under no lock; consumer snapshots it when
    // priming ends to derive the anchor). published by the same thread that calls
    // write(), so only that thread mutates it
    std::atomic<int64_t> mExpectedPtsNs{0};

    // playout anchor (consumer publishes, anyone reads via playoutAnchor())
    std::atomic<uint32_t> mAnchorSeq{0};   // even = fields stable, odd = publish in flight
    bool mAnchorValid = false;
    int64_t mAnchorPtsNs = 0;
    int64_t mAnchorWallNs = 0;
    uint32_t mAnchorEpoch = 0;

    // sender liveness (producer publishes, consumer uses it to cut a stale tail)
    std::atomic<int64_t> mLastWriteNs{0};       // mono of the last packet that reached us
    std::atomic<int64_t> mLastAudibleAtNs{0};   // mono of the last packet that carried sound
    std::atomic<int64_t> mFirstWriteNs{0};      // mono of the first packet, for the report
    bool mSettledLogged = false;                // consumer-only: cushion report emitted
    bool mIdle = false;                         // consumer-only: silence episode already evaluated

    // tail measurement (see watchOutput): all consumer-only, no atomics needed
    int64_t mEndedAtNs = 0;                     // when the level window declared content over
    int64_t mLastAudibleReadNs = 0;             // when audible samples last went to the device
    double mBacklogAtEndMs = 0;                 // queue depth at that moment, for the report
    double mOutLatencyAtEndMs = -1;              // device presentation latency at that moment
    bool mPlayedOutLogged = false;              // one report per content-end episode
    std::atomic<int64_t> mOutLatencyNs{-1};     // measured device-side presentation latency (n/a = -1)

    // sliding level window (see noteLevel): producer writes the array and publishes the
    // three atomics; the consumer only ever reads the atomics
    struct LevelEntry {
        int32_t audible;
        int32_t samples;
    };
    LevelEntry mLevelRing[LEVEL_WINDOW_PACKETS] = {};
    int mLevelIdx = 0;
    int64_t mLevelAudibleSum = 0;
    int64_t mLevelSampleSum = 0;
    int32_t mLevelPeak = 0;
    std::atomic<int64_t> mLevelAudibleSnap{0};
    std::atomic<int64_t> mLevelSampleSnap{0};
    std::atomic<int32_t> mLevelPeakSnap{0};
    int64_t mLastLevelReportNs = 0;             // consumer-only: throttle for reportLevel()
};

#endif  // TIMELINE_BUFFER_H
