/*
 * Behavioural checks for TimelineBuffer's audio timing, runnable on a device/emulator
 * (needs android/log.h + bionic). Covers the two things that decide how long sound
 * keeps playing after the sender stops:
 *
 *   A) stale-tail cut  — the sender goes quiet while audio is still queued: the
 *      leftover must be dropped instead of played out as a one-second tail
 *   B) cushion stability — a stream whose pts runs slightly faster than our arrival
 *      rate must NOT inflate the cushion (the old per-packet base creep converged to
 *      ~1024x the drift, i.e. hundreds of ms of buffer, which is the tail itself)
 *   C) jitter absorption — 0-80ms of arrival jitter must still be covered by the
 *      cushion without underruns
 *
 * Build & run:
 *   $NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++ \
 *       --target=x86_64-linux-android24 -std=c++17 -O1 -static-libstdc++ \
 *       -I app/src/main/cpp tools/timeline_tail_test.cpp -o /tmp/tail_test -llog
 *   adb push /tmp/tail_test /data/local/tmp/ && adb shell "chmod 755 /data/local/tmp/tail_test; /data/local/tmp/tail_test"
 *
 * Drives the buffer like the engine does: a producer writes PCM with pts advancing by
 * the packet duration, a consumer pulls 192-frame blocks like Oboe's data callback.
 */

#include <atomic>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

#include "timeline_buffer.h"

namespace {

constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr int kFramesPerPacket = 480;   // 10ms AAC-ELD
constexpr int kConsumeFrames = 192;     // 4ms Oboe callback
constexpr int AUDIBLE_TEST_LEVEL = 30;  // just under the 32 sample threshold, like dither
constexpr int64_t kMs = 1'000'000LL;

int failures = 0;

void check(const char *what, bool ok, double detail) {
    printf("[%s] %s (%.0f)\n", ok ? "ok  " : "FAIL", what, detail);
    if (!ok) failures++;
}

int backlogMs(TimelineBuffer &b) { return b.debugInfo().backlogMs; }

// a consumer thread draining the buffer at exactly the device rate (like an Oboe
// callback clocked by the audio hardware, not by sleep()). records when it last
// *output* a non-silent sample, so a scenario can measure the audible tail: how
// long sound keeps coming out after the sender's last audible packet. that is the
// only number the "sound keeps going for a second" complaint is about
struct Playout {
    TimelineBuffer &buf;
    std::atomic<bool> run{true};
    std::thread thread;
    std::vector<int16_t> out;
    std::atomic<int64_t> lastAudibleReadNs{0};
    // for restart measurements: when the next audible output happens after armAtNs
    std::atomic<int64_t> armAtNs{0};
    std::atomic<int64_t> firstAudibleAfter{0};
    // longest stretch of silent output (measured per read), for "did the resumed sound run
    // without a drop-out" checks: a flush landing mid-sound shows up here as a gap
    std::atomic<int64_t> silentSinceNs{0};
    std::atomic<int64_t> longestSilentNs{0};

    void resetSilence() {
        silentSinceNs.store(0, std::memory_order_relaxed);
        longestSilentNs.store(0, std::memory_order_relaxed);
    }

    void arm(int64_t atNs) {
        firstAudibleAfter.store(0, std::memory_order_relaxed);
        armAtNs.store(atNs, std::memory_order_relaxed);
    }

    explicit Playout(TimelineBuffer &b) : buf(b), out(kConsumeFrames * kChannels) {
        thread = std::thread([this] {
            const int64_t start = monoNs();
            int64_t consumed = 0;  // frames
            while (run.load()) {
                const int64_t due = (monoNs() - start) * kRate / 1'000'000'000LL;
                while (consumed < due) {
                    const int n = (int)std::min<int64_t>(kConsumeFrames, due - consumed);
                    buf.read(out.data(), n);
                    consumed += n;
                    bool audible = false;
                    for (int i = 0; i < n * kChannels; i++) {
                        if (out[i] > 32 || out[i] < -32) {
                            const int64_t now = monoNs();
                            lastAudibleReadNs.store(now, std::memory_order_relaxed);
                            const int64_t armedAt = armAtNs.load(std::memory_order_relaxed);
                            if (armedAt != 0 && now > armedAt &&
                                firstAudibleAfter.load(std::memory_order_relaxed) == 0) {
                                firstAudibleAfter.store(now, std::memory_order_relaxed);
                            }
                            audible = true;
                            break;
                        }
                    }
                    const int64_t now = monoNs();
                    if (audible) {
                        const int64_t since = silentSinceNs.exchange(0, std::memory_order_relaxed);
                        if (since != 0 && now - since > longestSilentNs.load(std::memory_order_relaxed)) {
                            longestSilentNs.store(now - since, std::memory_order_relaxed);
                        }
                    } else if (silentSinceNs.load(std::memory_order_relaxed) == 0) {
                        silentSinceNs.store(now, std::memory_order_relaxed);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    }
    void stop() {
        if (!run.exchange(false)) return;
        if (thread.joinable()) thread.join();
    }
    ~Playout() { stop(); }
};

std::vector<int16_t> makeAudio() {
    std::vector<int16_t> pcm(kFramesPerPacket * kChannels);
    for (size_t i = 0; i < pcm.size(); i++) pcm[i] = (int16_t)(2000 - (int)(i % 120) * 10);
    return pcm;
}

std::vector<int16_t> makeSilence() {
    // dither, not zeros: iOS keeps the mirror audio stream alive while a video is
    // paused, and those packets carry low-level noise, not literal silence
    std::vector<int16_t> s(kFramesPerPacket * kChannels);
    for (size_t i = 0; i < s.size(); i++) s[i] = (int16_t)(AUDIBLE_TEST_LEVEL - (int)(i % 7));
    return s;
}

const int64_t kPacketNs = (int64_t)kFramesPerPacket * kMs * 1000 / kRate;

}  // namespace

int main() {
    LogSink log;  // unbound: logcat only
    const std::vector<int16_t> pcm = makeAudio();
    const std::vector<int16_t> quiet = makeSilence();

    // ---- A) stale tail cut (fixed 120ms cushion: deterministic, no adaptive trims) ----
    {
        TimelineBuffer buf(kRate, kChannels, /*staticCushionMs=*/120, /*percentile=*/95, &log);
        Playout playout(buf);
        int64_t pts = monoNs() + 60 * kMs;  // sender leads by 60ms, like AirPlay
        const int64_t t0 = monoNs();
        // paced exactly against the pts cadence: no drift, so the trim path stays idle
        // and anything that drops the backlog below is the tail cut
        for (int i = 0; i < 80; i++) {
            const int64_t sendAt = t0 + (int64_t)i * 10 * kMs;
            while (monoNs() < sendAt) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            buf.write(pcm.data(), pcm.size(), pts);
            pts += kPacketNs;
        }
        check("A: steady backlog sits at the cushion", backlogMs(buf) < 260, backlogMs(buf));

        // 800ms burst into the buffer, then silence: the shape of the reported bug.
        // the sender dumps what it had buffered (AirPlay holds ~1s) and then stops
        for (int i = 0; i < 80; i++) {
            buf.write(pcm.data(), pcm.size(), pts);
            pts += kPacketNs;
        }
        check("A: burst leaves a deep backlog to cut", backlogMs(buf) > 600, backlogMs(buf));
        const int64_t lastAudibleWrite = monoNs();

        std::this_thread::sleep_for(std::chrono::milliseconds(900));
        const int audibleTailMs = (int)((playout.lastAudibleReadNs.load() - lastAudibleWrite) / kMs);
        printf("A: audible tail %dms, %d cut(s)\n", audibleTailMs, (int)buf.tailCuts());
        // the bound the design targets: trim sustain (150ms) + cushion (120ms) + one
        // Oboe burst (43ms) + slack. the old numbers gave ~520ms, which is what the
        // user heard as "a second"
        check("A: audible tail stays well under half a second",
              audibleTailMs >= 0 && audibleTailMs <= 450, audibleTailMs);
        // either the tail cut or the trim may bound this; one cut at most, and never a
        // second cut for the same silence episode (that would mean we re-armed wrongly)
        check("A: at most one cut for one silence episode", buf.tailCuts() <= 1, buf.tailCuts());

        pts += 600 * kMs;  // its timeline advanced while paused
        const int64_t resumeAt = monoNs();
        playout.arm(resumeAt);
        for (int i = 0; i < 30; i++) {
            buf.write(pcm.data(), pcm.size(), pts);
            pts += kPacketNs;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // "resumed" means sound actually reached the output again, not that the ring is
        // deep: a fast-prime restart deliberately runs on far less than the cushion
        const int64_t resumed = playout.firstAudibleAfter.load();
        check("A: stream resumes after a tail cut", resumed > 0, backlogMs(buf));
    }

    // ---- B) cushion must track the link, not the clock ----
    // B1: perfectly paced stream (no drift, no jitter) on a *static* cushion stays at
    // that cushion — the tail-cut path is what bounds a low-latency session, and a
    // clean link must not inflate it
    {
        TimelineBuffer buf(kRate, kChannels, /*staticCushionMs=*/120, /*percentile=*/95, &log);
        Playout playout(buf);
        int64_t pts = monoNs() + 60 * kMs;
        const int64_t t0 = monoNs();
        for (int i = 0; i < 400; i++) {  // 4s
            const int64_t sendAt = t0 + (int64_t)i * 10 * kMs;
            while (monoNs() < sendAt) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            buf.write(pcm.data(), pcm.size(), pts);
            pts += kPacketNs;
        }
        const TimelineBuffer::Debug d = buf.debugInfo();
        printf("B1: cushion %dms, backlog %dms\n", d.tunedCushionMs, d.backlogMs);
        check("B1: clean link holds a static cushion steady", d.tunedCushionMs >= 100
              && d.tunedCushionMs < 200, d.tunedCushionMs);
    }

    // B2: pts running ~4% fast (far worse than any real sender) must not run away: the
    // per-packet base creep this replaced settled at ~10s x drift, i.e. hundreds of ms
    {
        TimelineBuffer buf(kRate, kChannels, /*staticCushionMs=*/0, /*percentile=*/95, &log);
        Playout playout(buf);
        int64_t pts = monoNs() + 60 * kMs;
        int64_t t0 = monoNs();
        while (monoNs() - t0 < 5000 * kMs) {
            buf.write(pcm.data(), pcm.size(), pts);
            pts += kPacketNs;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const TimelineBuffer::Debug d = buf.debugInfo();
        printf("B2: cushion %dms, backlog %dms, trims %u\n", d.tunedCushionMs, d.backlogMs,
               d.metrics.trims);
        check("B2: drift does not inflate the cushion without bound",
              d.tunedCushionMs < 400, d.tunedCushionMs);
    }

    // ---- D) sender keeps feeding LOW-LEVEL packets (the AirPlay "leave app" case) ----
    // the real-world report: after leaving the app, iOS keeps sending packets, and those
    // packets carry dither / a fade-out tail / low-level noise. a single-sample peak
    // threshold calls that "sound", so a peak-based cut never fires and the stale tail
    // plays out in full. the level window must see through it: 200ms of almost-silent
    // packets means the content is over, and whatever is still queued is stale.
    //
    // The trim alone would also shed this burst, so the scenario first spends the trim's
    // throttle and *then* bursts while it is still cooling down — and crucially keeps
    // packets flowing the whole time, so the "no packet for 150ms" rule cannot be what
    // ends the tail. the level window is the only thing that can.
    {
        TimelineBuffer buf(kRate, kChannels, /*staticCushionMs=*/120, /*percentile=*/95, &log);
        Playout playout(buf);
        int64_t pts = monoNs() + 60 * kMs;
        const int64_t t0 = monoNs();
        // warm up with real audio, paced against pts so the trim path stays idle
        for (int i = 0; i < 80; i++) {
            const int64_t sendAt = t0 + (int64_t)i * 10 * kMs;
            while (monoNs() < sendAt) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            buf.write(pcm.data(), pcm.size(), pts);
            pts += kPacketNs;
        }
        // spend the trim throttle: one burst, then 200ms of *continuous* real audio at
        // packet pace (so the "no packet" rule never fires and only the trim acts), which
        // leaves the trim cooling down for another 600ms
        for (int i = 0; i < 80; i++) { buf.write(pcm.data(), pcm.size(), pts); pts += kPacketNs; }
        for (int i = 0; i < 20; i++) {
            buf.write(pcm.data(), pcm.size(), pts);
            pts += kPacketNs;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        for (int i = 0; i < 80; i++) { buf.write(pcm.data(), pcm.size(), pts); pts += kPacketNs; }
        check("D: second burst leaves a deep backlog the trim cannot shed", backlogMs(buf) > 600,
              backlogMs(buf));

        // sender "leaves the app": keeps sending packets whose samples sit just under a
        // per-sample threshold (dither-level). the old peak test read this as content
        // and never cut; the window share must not
        std::vector<int16_t> dither(kFramesPerPacket * kChannels);
        std::mt19937 drng(99);
        std::uniform_int_distribution<int> dj(-AUDIBLE_TEST_LEVEL, AUDIBLE_TEST_LEVEL);
        for (auto &s : dither) s = (int16_t)dj(drng);
        // sanity: this packet must read as "no sound" to any per-sample peak test —
        // that is precisely the case the old cut could not handle
        int over = 0;
        for (int16_t s : dither) if (s > 32 || s < -32) over++;
        check("D: dither stays under the per-sample threshold", over == 0, over);

        // packets never stop: only the level can end this tail
        const int64_t quietFrom = monoNs();
        int cutAtMs = -1;
        while (monoNs() - quietFrom < 900 * kMs) {
            buf.write(dither.data(), dither.size(), pts);
            pts += kPacketNs;
            if (cutAtMs < 0 && buf.tailCuts() > 0)
                cutAtMs = (int)((monoNs() - quietFrom) / kMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const int tailMs = (int)((playout.lastAudibleReadNs.load() - quietFrom) / kMs);
        printf("D: level-window cut at %dms despite continuous dither packets, %d cut(s), "
               "audible tail %dms\n", cutAtMs, (int)buf.tailCuts(), tailMs);
        check("D: dither packets do not defer the cut", cutAtMs > 0 && cutAtMs <= 500, cutAtMs);
        check("D: exactly one cut for one quiet episode", buf.tailCuts() == 1,
              buf.tailCuts());
        check("D: audible tail bounded even with the trim blocked", tailMs <= 500, tailMs);
    }

    // ---- E) the real device shape: mirroring's 80ms static cushion ----
    // This is the configuration the "sound keeps going" report came from
    // (AirPlayService._videoSyncCushionMs = 80). With the old absolute 400ms cut
    // threshold the ring here can never hold 520ms while the trim is doing its job,
    // so the cut was unreachable and the burst played out in full. Measure the
    // audible tail, which is what the user hears.
    {
        TimelineBuffer buf(kRate, kChannels, /*staticCushionMs=*/80, /*percentile=*/95, &log);
        Playout playout(buf);
        int64_t pts = monoNs() + 60 * kMs;
        const int64_t t0 = monoNs();
        for (int i = 0; i < 80; i++) {
            const int64_t sendAt = t0 + (int64_t)i * 10 * kMs;
            while (monoNs() < sendAt) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            buf.write(pcm.data(), pcm.size(), pts);
            pts += kPacketNs;
        }
        // sender dumps its ~1s AirPlay buffer, then the app is gone
        for (int i = 0; i < 100; i++) {
            buf.write(pcm.data(), pcm.size(), pts);
            pts += kPacketNs;
        }
        const int64_t lastAudibleWrite = monoNs();
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
        const int tailMs = (int)((playout.lastAudibleReadNs.load() - lastAudibleWrite) / kMs);
        printf("E: 80ms cushion, audible tail %dms, %d cut(s)\n", tailMs, (int)buf.tailCuts());
        check("E: burst tail is bounded by trim sustain + cushion + output fifo",
              tailMs >= 0 && tailMs <= 400, tailMs);
    }

    // ---- F) resume after a pause (the reported hiccup on the phone) ----
    // the pause empties the ring; when playback resumes the picture is already moving, so
    // the only thing the ear notices is how long the sound is still silent. a static
    // (mirror) session must restart on a fast-prime target instead of the whole cushion,
    // and a second flush request landing right after the restart (the video watcher
    // reporting the same discontinuity the audio side already handled) must not throw the
    // restart away for another full cushion of silence.
    {
        TimelineBuffer buf(kRate, kChannels, /*staticCushionMs=*/80, /*percentile=*/95, &log);
        Playout playout(buf);
        int64_t pts = monoNs() + 60 * kMs;
        const int64_t t0 = monoNs();
        for (int i = 0; i < 80; i++) {
            const int64_t sendAt = t0 + (int64_t)i * 10 * kMs;
            while (monoNs() < sendAt) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            buf.write(pcm.data(), pcm.size(), pts);
            pts += kPacketNs;
        }
        // pause: no packets for 600ms, the ring drains and the output falls silent
        std::this_thread::sleep_for(std::chrono::milliseconds(600));

        const int64_t resumeAt = monoNs();
        playout.arm(resumeAt);
        pts += 2000 * kMs;  // the sender's timeline advanced while paused
        buf.write(pcm.data(), pcm.size(), pts);
        pts += kPacketNs;
        // the video watcher reports the same discontinuity a moment later
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        buf.requestFlush();
        for (int i = 0; i < 25; i++) {
            buf.write(pcm.data(), pcm.size(), pts);
            pts += kPacketNs;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const int64_t first = playout.firstAudibleAfter.load();
        const int restartMs = first > 0 ? (int)((first - resumeAt) / kMs) : -1;
        printf("F: sound back %dms after resume on an 80ms cushion, %d underrun(s)\n",
               restartMs, (int)buf.debugInfo().metrics.underruns);
        check("F: mirror resume starts on the fast-prime target, not the cushion",
              restartMs >= 0 && restartMs <= 100, restartMs);
    }

    // ---- G) the resumed sound must not be cut in half by the video watcher ----
    // pause -> resume is a pts jump, so the video side reports a discontinuity and asks for
    // an audio flush ~100-200ms after the sound came back. by then the ring holds only *new*
    // audio (the pause drained it), so honouring the request chopped the first frames of the
    // resumed sound: heard as "the picture resumes instantly, the sound stutters once".
    // the resume grace window must swallow that request.
    {
        TimelineBuffer buf(kRate, kChannels, /*staticCushionMs=*/80, /*percentile=*/95, &log);
        Playout playout(buf);
        int64_t pts = monoNs() + 60 * kMs;
        const int64_t t0 = monoNs();
        for (int i = 0; i < 80; i++) {
            const int64_t sendAt = t0 + (int64_t)i * 10 * kMs;
            while (monoNs() < sendAt) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            buf.write(pcm.data(), pcm.size(), pts);
            pts += kPacketNs;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(600));  // pause

        playout.resetSilence();
        pts += 3000 * kMs;  // the sender's timeline advanced while paused
        for (int i = 0; i < 100; i++) {  // 1s of resumed sound
            buf.write(pcm.data(), pcm.size(), pts);
            pts += kPacketNs;
            // the video watcher's discontinuity flush arrives mid-sound, twice
            if (i == 15 || i == 30) buf.requestFlush();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const int64_t gap = playout.longestSilentNs.load();
        const int gapMs = (int)(gap / kMs);
        printf("G: longest drop-out after resume %dms, %d flush(es) rejected by the grace window\n",
               gapMs, buf.resumeFlushes());
        check("G: resumed sound runs without a drop-out", gapMs >= 0 && gapMs <= 60, gapMs);
        check("G: both mid-sound flushes were rejected", buf.resumeFlushes() >= 2,
              buf.resumeFlushes());
    }

    // ---- H) a paused sender that keeps the stream alive with silence ----
    // iOS does not tear the mirror audio stream down while the video is paused: packets
    // keep arriving, they just carry no sound. the resume detector must therefore key on
    // the last packet that *carried sound*; a packet-gap test never fires, the grace
    // window never arms, and the video side's pts-jump flush lands on audio that arrived
    // a few ms earlier — heard as "the picture resumes instantly, the sound hitches".
    {
        TimelineBuffer buf(kRate, kChannels, /*staticCushionMs=*/80, /*percentile=*/95, &log);
        Playout playout(buf);
        int64_t pts = monoNs() + 60 * kMs;
        const int64_t t0 = monoNs();
        for (int i = 0; i < 80; i++) {              // 800ms of sound
            const int64_t sendAt = t0 + (int64_t)i * 10 * kMs;
            while (monoNs() < sendAt) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            buf.write(pcm.data(), pcm.size(), pts);
            pts += kPacketNs;
        }
        for (int i = 0; i < 60; i++) {              // paused: 600ms of silent packets
            buf.write(quiet.data(), quiet.size(), pts);
            pts += kPacketNs;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        playout.resetSilence();
        for (int i = 0; i < 100; i++) {             // 1s of resumed sound
            buf.write(pcm.data(), pcm.size(), pts);
            pts += kPacketNs;
            if (i == 15 || i == 30) buf.requestFlush();   // the video pts jump
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const int64_t gap = playout.longestSilentNs.load();
        const int gapMs = (int)(gap / kMs);
        printf("H: longest drop-out after resume %dms, %d flush(es) rejected by the grace window\n",
               gapMs, buf.resumeFlushes());
        check("H: resumed sound runs without a drop-out", gapMs >= 0 && gapMs <= 60, gapMs);
        check("H: both mid-sound flushes were rejected", buf.resumeFlushes() >= 2,
              buf.resumeFlushes());
    }

    // ---- C) jitter absorption ----
    {
        TimelineBuffer buf(kRate, kChannels, /*staticCushionMs=*/0, /*percentile=*/95, &log);
        Playout playout(buf);
        const int64_t start = monoNs();
        int64_t pts = start + 60 * kMs;
        std::mt19937 rng(1234);
        std::uniform_int_distribution<int> jitterMs(0, 80);
        uint32_t afterWarmup = 0;
        for (int i = 0; i < 600; i++) {  // 6s of audio
            const int64_t sendAt = start + (int64_t)i * 10 * kMs + (int64_t)jitterMs(rng) * kMs;
            while (monoNs() < sendAt) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            buf.write(pcm.data(), pcm.size(), pts);
            pts += kPacketNs;
            if (i == 50) afterWarmup = buf.debugInfo().metrics.underruns;
        }
        const TimelineBuffer::Debug d = buf.debugInfo();
        printf("C: cushion %dms, backlog %dms, underruns %u (after warmup %u)\n",
               d.tunedCushionMs, d.backlogMs, d.metrics.underruns, afterWarmup);
        check("C: cushion grows to cover 0-80ms of jitter", d.tunedCushionMs >= 50,
              d.tunedCushionMs);
        // one gap in 6s under 80ms of jitter is the accepted price of a bounded buffer;
        // anything more means the cushion is no longer covering the link
        check("C: at most one underrun once warmed up", d.metrics.underruns <= afterWarmup + 1,
              (double)(d.metrics.underruns - afterWarmup));
    }

    printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
