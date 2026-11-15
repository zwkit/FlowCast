// FlowCast Windows client - orchestration implementation.
#include "client.h"

#include "aacenc.h"
#include "audio_stream.h"
#include "audiocap.h"
#include "capture.h"
#include "crypto.h"
#include "fairplay.h"
#include "h264enc.h"
#include "log.h"
#include "mdns.h"
#include "mirror.h"
#include "plist.h"
#include "rtsp.h"
#include "timing.h"
#include "bytebuf.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace fc {
namespace {

constexpr uint8_t kStreamTypeMirror = 110;
constexpr uint8_t kStreamTypeAudio = 96;
constexpr int kAudioCtAacLc = 4;   // AAC-LC, what FlowCast decodes in hardware
constexpr int kAudioSpf = 1024;    // samples per AAC frame
constexpr int kAudioRate = 44100;  // the one rate every AirPlay format uses
constexpr int kAudioChannels = 2;   // AirPlay mirror audio is always stereo

// Mirror of ClientConfig::bitsPerPixel, used when a caller passes 0. Keep the
// two in sync -- the GUI builds a ClientConfig, the CLI fills one in by hand.
constexpr double kDefaultBpp = 0.35;

// What the log says about a request that did not come back 200: the status code
// when the receiver actually answered, and a word for the socket-level reasons
// when it did not. Every failure used to print a bare "0" there, which is what
// the status card then showed the user - "SETUP #1 · 0" explains nothing.
std::string fail_text(const RtspResponse& r) {
    if (r.status > 0) return std::to_string(r.status);
    return net_error_text(r.err);
}

// Scans a [4-byte BE length][NAL]... buffer for an IDR (type 5) NAL.
bool containsIdr(const std::vector<uint8_t>& nals, bool hevc) {
    size_t off = 0;
    while (off + 4 <= nals.size()) {
        const size_t len = Buf::getU32BE(nals.data() + off);
        off += 4;
        if (len == 0 || off + len > nals.size()) break;
        // 19/20 are IDR_W_RADL / IDR_N_LP, 21 is CRA -- all of them start a
        // decodable picture, which is what the receiver needs before it can
        // join. Getting this wrong only costs a delayed first frame, but the
        // parameter sets go out on exactly this edge.
        if (hevc) {
            if (len < 2) break;
            const int t = (nals[off] >> 1) & 0x3f;
            if (t == 19 || t == 20 || t == 21) return true;
        } else if ((nals[off] & 0x1f) == 5) {
            return true;
        }
        off += len;
    }
    return false;
}

struct Discovered {
    std::string host;
    uint16_t port = 0;
    bool ok = false;
    std::string match;              // lowercased instance substring, empty = any
    std::vector<std::string> names; // everything seen, so a wrong pick is diagnosable
};

std::string toLower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return s;
}

void onServiceFound(const MdnsServiceInfo& info, void* user) {
    auto* d = static_cast<Discovered*>(user);
    d->names.push_back(info.instance);
    LOGI("discovery: candidate %s -> %s:%u (%s)", info.instance.c_str(), info.host.c_str(),
         (unsigned)info.port, info.address.empty() ? "name only" : info.address.c_str());
    if (d->ok) return; // first acceptable one wins
    if (!d->match.empty() && toLower(info.instance).find(d->match) == std::string::npos) return;
    const std::string host = info.address.empty() ? info.host : info.address;
    if (host.empty() || info.port == 0) return;
    d->host = host;
    d->port = info.port;
    d->ok = true;
}

} // namespace

struct FlowCastClient::Impl {
    ClientConfig cfg;
    std::atomic<bool> running{false};

    RtspClient rtsp;
    TimingClient timing;
    MirrorStream mirror;
    AudioStream audio;

    std::string host;
    uint16_t port = 0;
    uint16_t mirrorDataPort = 0;
    uint16_t audioDataPort = 0;
    uint16_t audioControlPort = 0;
    uint16_t timingPort = 0;
    uint16_t localAudioControlPort = 0;

    uint64_t streamConnectionID = 0;
    uint8_t aeskeyAudio[16] = {0};
    uint8_t aesivAudio[16] = {0};

    std::thread videoThread;
    std::thread audioThread;
    std::thread timingThread;

    // A stream thread that cannot start fails the whole session. Previously it
    // only logged and returned, which left the RTSP session and the audio
    // stream running: the GUI kept saying "casting" and the receiver sat on
    // "connected, waiting for the sender to start" with no hint of a problem.
    std::mutex failMutex;
    std::wstring fatalError;

    void failSession(const wchar_t* msg) {
        {
            std::lock_guard<std::mutex> g(failMutex);
            if (fatalError.empty()) fatalError = msg;
        }
        LOGW("client: session ended by a stream failure");
        running = false;
    }

    std::wstring failure() const {
        std::lock_guard<std::mutex> g(const_cast<std::mutex&>(failMutex));
        return fatalError;
    }


    // Written by the three stream threads, read by stats()/onProgress from any
    // other thread.
    std::mutex statsMutex;
    Stats stats;
    std::chrono::steady_clock::time_point t0;

    template <typename F>
    void withStats(F&& fn) {
        std::lock_guard<std::mutex> g(statsMutex);
        fn(stats);
    }

    Stats snapshot() const {
        Stats out;
        {
            // const method: the mutex has to be reachable through a const Impl,
            // which is fine because the object itself is never const in use.
            std::lock_guard<std::mutex> g(const_cast<std::mutex&>(statsMutex));
            out = stats;
            out.elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
        return out;
    }
};

FlowCastClient::~FlowCastClient() {
    stop();
}

FlowCastClient::Stats FlowCastClient::stats() const {
    std::lock_guard<std::mutex> g(m_statsMutex);
    return m_stats;
}

std::wstring FlowCastClient::error() const {
    return m_impl ? m_impl->failure() : std::wstring();
}

// Same shape as fail_text, minus the "we did not even get a reply" case: a
// status code is the receiver saying something, a socket error is it saying
// nothing, and only the first one is worth preferring when several attempts
// failed in different places.
bool is_status(const std::string& s) { return !s.empty() && s[0] >= '0' && s[0] <= '9'; }

// How far a handshake attempt got before it failed. Ordered, because start()
// reports the deepest failure: the step that stopped us is the diagnosis, and
// a variant that never got past OPTIONS must not overwrite it.
enum Depth {
    kDepthOptions = 0,
    kDepthFp1 = 1,
    kDepthFp2 = 2,
    kDepthSetup1 = 3,
    kDepthSetup2 = 4,
    kDepthRecord = 5,
};

bool FlowCastClient::start(const ClientConfig& cfg) {
    m_rank = 0;
    m_depth = 0;
    m_deepStep.clear();
    m_deepCode.clear();
    m_sawStatus = false;
    m_fp2Silent = false;
    m_host = cfg.host;
    m_hostPort = cfg.port;

    // Every handshake variant worth trying, ordered by how much protocol
    // correctness each one gives up. A receiver that behaves finishes on the
    // first and never sees the rest, so the ladder costs nothing on the happy
    // path; on one that says no, a single run maps out which part of the
    // handshake it objects to instead of needing one round trip per guess.
    struct Step {
        FairplayMode fp;
        SetupStyle setup;
        bool apple;
        const char* what;
    };
    // Ordered so the cheapest decisive variant comes second: on a receiver that
    // refuses, the shape of SETUP is the part that has never been varied, and
    // "no FairPlay + Apple-shaped SETUP" tests it without paying for a key
    // message the receiver may simply ignore.
    static const Step kSteps[] = {
        {FairplayMode::kFull, SetupStyle::kTwoStage, false, "FairPlay both rounds"},
        {FairplayMode::kSkip, SetupStyle::kSingle, false, "no FairPlay, single SETUP"},
        {FairplayMode::kFull, SetupStyle::kSingle, false, "FairPlay both rounds, single SETUP"},
        {FairplayMode::kSkip, SetupStyle::kTwoStage, false, "no FairPlay at all"},
        {FairplayMode::kHelloOnly, SetupStyle::kTwoStage, false, "FairPlay greeting only"},
        {FairplayMode::kSkip, SetupStyle::kTwoStage, true, "no FairPlay, Apple identity"},
    };
    // A variant the caller named on the command line is not a guess to be
    // improved on: run it alone. This used to be overwritten by the first
    // ladder entry, which made --skip-fairplay a silent no-op.
    const bool pinned = cfg.fairplayPinned || cfg.setupPinned;
    Step pinnedStep{cfg.fairplay, cfg.setup, cfg.appleUa, "the variant named on the command line"};
    const Step* steps = pinned ? &pinnedStep : kSteps;
    const int kTotal = pinned ? 1 : (int)(sizeof(kSteps) / sizeof(kSteps[0]));

    ClientConfig attempt = cfg;
    int tried = 0;
    for (int i = 0; i < kTotal; i++) {
        const Step& s = steps[i];
        // The Apple identity only matters when the receiver answered with a
        // status. Rebroadcasting the same silence as an iPhone is just a second
        // way to be ignored. (Measured on MAXHUB: /fp-setup #1 comes back
        // byte-identical for both identities, so it is not a header gate.)
        if (s.apple && !m_sawStatus) continue;
        // Nothing is listening: no later variant changes that, and four more
        // seconds of "connecting" would only delay the "nothing there" card.
        if (tried > 0 && m_depth == 0 && !m_sawStatus) {
            LOGI("client: the receiver is not answering at all - skipping the remaining variants");
            break;
        }

        attempt.fairplay = s.fp;
        attempt.setup = s.setup;
        attempt.appleUa = s.apple;
        attempt.host = m_host;
        attempt.port = m_hostPort;
        tried++;
        LOGI("client: attempt %d/%d - %s", i + 1, kTotal, s.what);
        if (start_once(attempt)) return true;
        LOGI("client: attempt %d/%d failed", i + 1, kTotal);
        if (!cfg.retryVariants) break;
        // A receiver that resets the connection mid-handshake needs a moment
        // before its RTSP service answers again: on MAXHUB the next variant was
        // met with a reset on OPTIONS itself, which burns a whole variant on a
        // service that is still coming back up. One and a half seconds is
        // enough for that restart and invisible on a receiver that never fails.
        Sleep(1500);
    }

    if (m_fp2Silent) {
        LOGI("client: /fp-setup #1 was answered and #2 was not - the greeting is all this receiver "
             "implemented, so no key was ever agreed");
    }
    if (!m_deepStep.empty()) {
        // Re-emit the deepest failure last so the status card quotes the
        // attempt that was actually informative rather than the noise from the
        // one that happened to be tried last. Same "step (code)" shape the card
        // parses, so nothing downstream changes.
        LOGE("client: %s failed (%s) - deepest of %d handshake variant(s)", m_deepStep.c_str(),
             m_deepCode.c_str(), tried);
    }
    return false;
}

void FlowCastClient::note_depth(int depth, const char* step, const std::string& code) {
    const bool status = is_status(code);
    if (status) m_sawStatus = true;
    if (depth > m_depth) m_depth = depth;
    const int rank = depth * 2 + (status ? 1 : 0);
    if (rank < m_rank) return;
    m_rank = rank;
    m_deepStep = step;
    m_deepCode = code;
}

bool FlowCastClient::start_once(const ClientConfig& cfg) {
    stop();
    Impl* im = new Impl();
    m_impl = im;
    im->cfg = cfg;
    // Level only: the host process owns the sink. The CLI leaves it unset (the
    // logger falls back to the console) and the GUI installs one that feeds the
    // error line into its status card.
    fc::log_set_level(cfg.verbose ? LogLevel::Debug : LogLevel::Info);

    // --- 1. discovery -------------------------------------------------------
    if (!cfg.host.empty()) {
        im->host = cfg.host;
        im->port = cfg.port;
        LOGI("client: using explicit host %s:%u", im->host.c_str(), (unsigned)im->port);
    } else {
        LOGI("client: browsing mDNS for _airplay._tcp (%d ms)...", cfg.discoverMs);
        Discovered found;
        found.match = toLower(cfg.receiverMatch);
        MdnsBrowser browser;
        browser.start("_airplay._tcp", onServiceFound, &found, cfg.discoverMs);
        if (!found.ok) {
            if (found.names.empty()) {
                LOGE("client: no FlowCast receiver found on this network");
            } else {
                LOGE("client: %zu receiver(s) found but none matched '%s'", found.names.size(),
                     cfg.receiverMatch.c_str());
                for (const std::string& n : found.names) LOGI("  - %s", n.c_str());
            }
            return false;
        }
        im->host = found.host;
        im->port = found.port;
    }
    // Remembered so the next attempt in the ladder dials the same address: an
    // mDNS browse per variant would add seconds to a path that is already the
    // slow one.
    m_host = im->host;
    m_hostPort = im->port;

    // --- 2. RTSP handshake --------------------------------------------------
    if (!im->rtsp.connect(im->host.c_str(), im->port)) return false;

    // Address the receiver by its own address, the way every genuine sender
    // does: `SETUP rtsp://192.168.1.202:7386/1`. UxPlay keys its handlers on the
    // *path* and ignores the host, which is why "localhost" survived three
    // receivers unnoticed - but a receiver that keeps a session table keyed on
    // the request URL sees an address that was never dialled. MAXHUB hung up on
    // every SETUP shape at the same ~2.0 s, body irrelevant, which is what a
    // request that never matched anything looks like.
    auto url_for = [&](const char* path) {
        char buf[256];
        snprintf(buf, sizeof(buf), "rtsp://%s:%u%s", im->rtsp.peerAddress().c_str(),
                 (unsigned)im->port, path);
        return std::string(buf);
    };

    if (cfg.appleUa) {
        // Same three headers the probe sends, now for the whole session. The
        // panel on the other end has to be told who is calling before it decides
        // whether /fp-setup exists, so the identity has to be on OPTIONS too -
        // not only on the request that is being refused.
        LOGI("client: announcing as AirPlay/670.99.20 (X-Apple-* headers on)");
        im->rtsp.set_identity("AirPlay/670.99.20",
                              "X-Apple-ProtocolVersion: 1\r\n"
                              "X-Apple-Device-ID: 0x000000000000\r\n"
                              "X-Apple-Session-ID: 6C1B4E4A-5D2F-4E4B-9B5F-6D8A0C1E2F30\r\n");
    }

    RtspResponse r;
    if (!im->rtsp.request("OPTIONS", "rtsp://localhost/", nullptr, "", r) || r.status != 200) {
        LOGE("client: OPTIONS failed (%s)", fail_text(r).c_str());
        note_depth(kDepthOptions, "OPTIONS", fail_text(r));
        return false;
    }
    // The Public header lists the RTSP methods the receiver implements, which is
    // the cheapest way to tell a full AirPlay mirroring sink from a video-only
    // one. Third-party units (MAXHUB, AirServer, assorted TVs) differ a lot here,
    // and "it works from an iPhone" says nothing about what this client needs.
    if (const std::string* pub = r.header("public")) {
        LOGI("client: receiver Public: %s", pub->c_str());
    } else {
        // Say so explicitly: too many receivers omit it, and a silent absence
        // looks exactly like a bug in our header parsing.
        std::string names;
        for (const auto& kv : r.headers) {
            if (!names.empty()) names += ",";
            names += kv.first;
        }
        LOGI("client: receiver sent no Public header (has: %s)", names.c_str());
    }
    if (const std::string* srv = r.header("server")) LOGI("client: receiver Server: %s", srv->c_str());

    // --- 2. FairPlay ---------------------------------------------------------
    // Two /fp-setup round trips, so the receiver can derive the AES key it
    // would use to decrypt the mirror stream -- except our stream is plaintext
    // (SETUP #1 sends no `et`), so nothing downstream depends on the result.
    // Well-behaved receivers insist on the exchange anyway, so it stays first.
    //
    // Third-party panels are the exception. MAXHUB advertises legacy pairing in
    // its `features` mask and then silently drops /fp-setup for any non-Apple
    // sender: no 4xx, no close, just no answer until the socket times out.
    // Aborting there threw away a session that would have worked, so a failure
    // now degrades to "carry on without it" and lets SETUP have the final say --
    // and if the receiver really does require pairing, SETUP #1 reports it by
    // number instead of leaving a vague "refused".
    uint8_t keymsg[164];
    bool fairplayOk = false;
    // Set when the receiver stopped answering in the middle of the exchange.
    // Such a socket is dead even though recv() only reported a timeout, and the
    // failure then lands on whichever request happens to be next - which is how
    // "SETUP #1 failed (dropped)" gets blamed on SETUP when /fp-setup was the
    // request the receiver actually walked away from.
    bool fairplayBroke = false;
    if (cfg.fairplay == FairplayMode::kSkip) {
        LOGI("client: skipping FairPlay; the mirror stream is plaintext anyway");
    } else {
        // fp-setup #1: 16 bytes, the receiver answers with a 142-byte reply
        // whose byte 4 must be 0x03 (FairPlay version). The canonical Apple M1
        // lives in fairplay.cpp so the probe sends the same bytes - it used to
        // send "FPLY" + zeros and reported the endpoint as missing.
        Buf req;
        uint8_t m1[16];
        fairplay_m1(m1);
        req.append(m1, sizeof(m1));
        // 3 s, not the 5 s default: a receiver that implements this answers in
        // microseconds, and on one that does not the wait is pure startup cost.
        if (!im->rtsp.request("POST", "/fp-setup", "application/octet-stream", req.data(),
                              req.size(), r, 3000) ||
            r.body.size() < 16) {
            LOGI("client: fp-setup #1 got no answer (%s, %zu bytes) - no legacy FairPlay "
                 "here, continuing without it", fail_text(r).c_str(), r.body.size());
            fairplayBroke = r.status == 0;
            note_depth(kDepthFp1, "fp-setup #1", fail_text(r));
        } else if ((uint8_t)r.body[4] != 0x03) {
            LOGI("client: fp-setup #1 answered FairPlay version 0x%02x - continuing without it",
                 (uint8_t)r.body[4]);
        } else {
            LOGI("client: fp-setup #1 ok");
            if (cfg.fairplay == FairplayMode::kHelloOnly) {
                LOGI("client: stopping after the greeting - this variant does not send the key "
                     "message");
            } else {
                // fp-setup #2: the 164-byte key message. A receiver that runs
                // the real exchange echoes bytes [144:164] and stores the rest
                // as the FairPlay session key.
                fairplay_build_keymsg(keymsg);
                if (!im->rtsp.request("POST", "/fp-setup", "application/octet-stream", keymsg,
                                      sizeof(keymsg), r, cfg.fp2TimeoutMs) ||
                    r.body.size() < 32) {
                    LOGI("client: fp-setup #2 got no usable answer (%s, %zu bytes) - continuing",
                         fail_text(r).c_str(), r.body.size());
                    fairplayBroke = r.status == 0;
                    note_depth(kDepthFp2, "fp-setup #2", fail_text(r));
                    if (r.status == 0) m_fp2Silent = true;
                } else {
                    LOGI("client: fp-setup #2 ok");
                    fairplayOk = true;
                }
            }
        }
    }
    if (!fairplayOk) {
        // ekey still has to carry 72 bytes of something. A receiver that never
        // completed the exchange has no key to compare against, so random bytes
        // are indistinguishable from a real derivation.
        fc::random_bytes(keymsg, sizeof(keymsg));
    }
    if (fairplayBroke) {
        // Redial rather than reuse a socket the receiver has already walked
        // away from, and re-announce with OPTIONS so the new connection is in
        // the same state the first one was before the exchange started.
        LOGI("client: the FairPlay exchange broke the connection - reopening it before SETUP");
        if (!im->rtsp.reconnect()) {
            // reconnect() already logged the socket reason.
            LOGE("client: could not reopen the connection");
            return false;
        }
        RtspResponse again;
        if (!im->rtsp.request("OPTIONS", "rtsp://localhost/", nullptr, "", again) ||
            again.status != 200) {
            LOGE("client: OPTIONS failed after reconnecting (%s)", fail_text(again).c_str());
            note_depth(kDepthFp2, "fp-setup #2", fail_text(again));
            return false;
        }
    }

    // --- 3. our own keys ----------------------------------------------------
    fc::random_bytes(im->aeskeyAudio, 16);
    fc::random_bytes(im->aesivAudio, 16);
    fc::random_bytes(reinterpret_cast<uint8_t*>(&im->streamConnectionID), 8);

    uint8_t ekey[72];
    fairplay_make_ekey(keymsg, im->aeskeyAudio, ekey);

    // --- 4. timing port must exist before SETUP #1 -------------------------
    if (!im->timing.start()) {
        LOGE("client: could not open the timing port");
        return false;
    }
    im->timingPort = im->timing.localPort();

    // --- 5. audio sockets must exist before SETUP ---------------------------
    // The audio stream dict carries the control port we listen on, so in both
    // shapes the sockets have to be open before the request goes out. setKeys()
    // allocates the stream state, so it must come after openSockets (which
    // starts by releasing it).
    if (cfg.audio) {
        if (!im->audio.openSockets(im->localAudioControlPort)) {
            LOGE("client: could not open the audio sockets");
            return false;
        }
        im->audio.setKeys(im->aeskeyAudio, im->aesivAudio);
    }

    // Both shapes end in the same place - the receiver's ports - so the
    // response handling is shared and only the request differs.
    auto read_ports = [&](const PlistValue& res, const char* step) {
        const PlistValue* rs = res.find("streams");
        if (!rs || rs->type != PlistType::Array) {
            LOGI("client: %s response has no streams", step);
            return;
        }
        for (const auto& s : rs->arr) {
            const PlistValue* t = s.find("type");
            if (!t) continue;
            if (t->asInt() == kStreamTypeMirror) {
                const PlistValue* dp = s.find("dataPort");
                if (dp) im->mirrorDataPort = (uint16_t)dp->asInt();
            } else if (t->asInt() == kStreamTypeAudio) {
                const PlistValue* dp = s.find("dataPort");
                const PlistValue* cp = s.find("controlPort");
                if (dp) im->audioDataPort = (uint16_t)dp->asInt();
                if (cp) im->audioControlPort = (uint16_t)cp->asInt();
            }
        }
    };
    auto have_ports = [&]() {
        if (cfg.video && im->mirrorDataPort == 0) {
            LOGE("client: receiver gave no mirror dataPort");
            return false;
        }
        if (cfg.audio && (im->audioDataPort == 0 || im->audioControlPort == 0)) {
            LOGE("client: receiver gave no audio ports");
            return false;
        }
        return true;
    };
    // The stream list. `apple` adds what the mirror stream says about itself in
    // the single request shape: the encryption story and the sender's own
    // ports, which is the part a receiver written against iOS actually reads.
    auto build_streams = [&](bool apple) {
        std::vector<PlistValue> streams;
        if (cfg.video) {
            if (apple) {
                streams.push_back(PlistValue::dictOf({
                    {"type", PlistValue::integer(kStreamTypeMirror)},
                    {"streamConnectionID", PlistValue::integer((long long)im->streamConnectionID)},
                    // Zeros are what iOS sends: it is asking the receiver which
                    // port to connect to, not announcing its own listener.
                    {"dataPort", PlistValue::integer(0)},
                    {"ctlPort", PlistValue::integer(0)},
                    {"eventPort", PlistValue::integer(0)},
                    {"headerCompression", PlistValue::boolean(true)},
                    {"encryptionType", PlistValue::string("FairPlay")},
                }));
            } else {
                streams.push_back(PlistValue::dictOf({
                    {"type", PlistValue::integer(kStreamTypeMirror)},
                    {"streamConnectionID", PlistValue::integer((long long)im->streamConnectionID)},
                }));
            }
        }
        if (cfg.audio) {
            streams.push_back(PlistValue::dictOf({
                {"type", PlistValue::integer(kStreamTypeAudio)},
                {"controlPort", PlistValue::integer(im->localAudioControlPort)},
                {"ct", PlistValue::integer(kAudioCtAacLc)},
                {"spf", PlistValue::integer(kAudioSpf)},
                {"usingScreen", PlistValue::boolean(cfg.video)},
                {"isMedia", PlistValue::boolean(false)},
            }));
        }
        return streams;
    };

    if (cfg.setup == SetupStyle::kSingle) {
        // --- 6. one SETUP, in the shape an Apple sender uses ----------------
        // Keys, timing, `isScreenMirroring` and the whole stream list in the
        // same request, and no second SETUP at all. iOS repeats the encryption
        // story both at the top level and inside the mirror stream, so a
        // receiver written against iOS finds it wherever it looks. Receivers
        // that only ever handled the split form understand this too, which is
        // what makes it worth trying on anything that refuses.
        std::vector<PlistValue> streams = build_streams(true);
        if (streams.empty()) {
            LOGE("client: nothing to stream (video and audio both disabled)");
            return false;
        }
        PlistValue body = PlistValue::dictOf({
            {"ekey", PlistValue::data(std::string((const char*)ekey, 72))},
            {"eiv", PlistValue::data(std::string((const char*)im->aesivAudio, 16))},
            {"timingPort", PlistValue::integer(im->timingPort)},
            {"timingProtocol", PlistValue::string("NTP")},
            {"deviceID", PlistValue::string("00:11:22:33:44:55")},
            {"model", PlistValue::string("WindowsPC")},
            {"name", PlistValue::string(cfg.name)},
            {"isScreenMirroring", PlistValue::boolean(true)},
            {"streams", PlistValue::array(std::move(streams))},
        });
        std::string wire = plist_dump_bin(body);
        const std::string url = url_for("/1");
        LOGI("client: SETUP %s", url.c_str());
        if (!im->rtsp.request("SETUP", url.c_str(), "application/x-apple-binary-plist",
                              wire, r, 3000) ||
            r.status != 200) {
            LOGE("client: SETUP (single) failed (%s)", fail_text(r).c_str());
            note_depth(kDepthSetup1, "SETUP single", fail_text(r));
            return false;
        }
        PlistValue res;
        if (!plist_parse(r.body, res)) {
            LOGE("client: SETUP (single) response is not a plist");
            return false;
        }
        read_ports(res, "SETUP (single)");
        if (!have_ports()) return false;
        LOGI("client: SETUP (single) ok (mirror data %u, audio data %u control %u)",
             (unsigned)im->mirrorDataPort, (unsigned)im->audioDataPort,
             (unsigned)im->audioControlPort);
    } else {
        // --- 6. SETUP #1: ekey/eiv + timing --------------------------------
        {
            PlistValue body = PlistValue::dictOf({
                {"ekey", PlistValue::data(std::string((const char*)ekey, 72))},
                {"eiv", PlistValue::data(std::string((const char*)im->aesivAudio, 16))},
                {"timingPort", PlistValue::integer(im->timingPort)},
                {"timingProtocol", PlistValue::string("NTP")},
                {"deviceID", PlistValue::string("00:11:22:33:44:55")},
                {"model", PlistValue::string("WindowsPC")},
                {"name", PlistValue::string(cfg.name)},
                {"isRemoteControlOnly", PlistValue::boolean(false)},
            });
            std::string wire = plist_dump_bin(body);
            const std::string url = url_for("/1");
            LOGI("client: SETUP %s", url.c_str());
            if (!im->rtsp.request("SETUP", url.c_str(),
                                  "application/x-apple-binary-plist", wire, r, 3000) ||
                r.status != 200) {
                LOGE("client: SETUP #1 failed (%s)", fail_text(r).c_str());
                note_depth(kDepthSetup1, "SETUP #1", fail_text(r));
                return false;
            }
            PlistValue res;
            if (!plist_parse(r.body, res)) {
                LOGE("client: SETUP #1 response is not a plist");
                return false;
            }
            const PlistValue* tp = res.find("timingPort");
            if (!tp || tp->asInt() == 0) {
                LOGE("client: SETUP #1 did not return a timingPort");
                return false;
            }
            LOGI("client: SETUP #1 ok (receiver timingPort %lld)", tp->asInt());
        }

        // --- 7. SETUP #2: the streams --------------------------------------
        {
            std::vector<PlistValue> streams = build_streams(false);
            if (streams.empty()) {
                LOGE("client: nothing to stream (video and audio both disabled)");
                return false;
            }
            PlistValue body = PlistValue::dictOf({
                {"streams", PlistValue::array(std::move(streams))},
            });
            std::string wire = plist_dump_bin(body);
            const std::string url = url_for("/2");
            if (!im->rtsp.request("SETUP", url.c_str(),
                                  "application/x-apple-binary-plist", wire, r, 3000) ||
                r.status != 200) {
                LOGE("client: SETUP #2 failed (%s)", fail_text(r).c_str());
                note_depth(kDepthSetup2, "SETUP #2", fail_text(r));
                return false;
            }
            PlistValue res;
            if (!plist_parse(r.body, res)) {
                LOGE("client: SETUP #2 response is not a plist");
                return false;
            }
            read_ports(res, "SETUP #2");
            if (!have_ports()) return false;
            LOGI("client: SETUP #2 ok (mirror data %u, audio data %u control %u)",
                 (unsigned)im->mirrorDataPort, (unsigned)im->audioDataPort,
                 (unsigned)im->audioControlPort);
        }
    }
    // --- 8. open the media streams -----------------------------------------
    if (cfg.video) {
        im->mirror.setKeys(im->streamConnectionID, im->aeskeyAudio);
        if (!im->mirror.connect(im->rtsp.peerAddress().c_str(), im->mirrorDataPort)) return false;
    }
    if (cfg.audio) {
        if (!im->audio.setRemote(im->rtsp.peerAddress().c_str(), im->audioDataPort,
                                 im->audioControlPort))
            return false;
    }

    // --- 9. RECORD ----------------------------------------------------------
    {
        PlistValue body = PlistValue::dictOf({
            {"audioSampleRate", PlistValue::integer(kAudioRate)},
            {"audioCompression", PlistValue::integer(kAudioCtAacLc)},
            {"volume", PlistValue::real(0.0)},
        });
        std::string wire = plist_dump_bin(body);
        const std::string url = url_for("/3");
        if (!im->rtsp.request("RECORD", url.c_str(), "application/x-apple-binary-plist", wire, r,
                           3000) ||
            r.status != 200) {
            LOGE("client: RECORD failed (%s)", fail_text(r).c_str());
            note_depth(kDepthRecord, "RECORD", fail_text(r));
            return false;
        }
        LOGI("client: RECORD ok - mirroring %s to %s:%u", cfg.audio ? "video+audio" : "video",
             im->host.c_str(), (unsigned)im->port);
    }

    im->running = true;
    m_active = true;
    return true;
}

// Runs the three stream loops on their own threads until stop() is called.
void FlowCastClient::run(const std::atomic<bool>* externalStop) {
    if (!m_impl) return;
    Impl* im = m_impl;
    im->t0 = std::chrono::steady_clock::now();

    // --- timing responder -------------------------------------------------
    // The receiver polls roughly every 3 s and needs an answer for its clock
    // offset to converge. t1/t2 are wall-clock NTP (raop_ntp.c subtracts the
    // 1900 epoch on both), so they must come from wall_ns(), not mono_ns().
    // This thread also doubles as the progress ticker.
    if (im->timingPort != 0) {
        im->timingThread = std::thread([this, externalStop]() {
            Impl* im = m_impl;
            uint64_t lastReport = 0;
            while (im && im->running.load()) {
                uint64_t serverSendNtp = 0;
                uint32_t seq = 0;
                if (im->timing.waitRequest(200, serverSendNtp, seq)) {
                    im->timing.reply(serverSendNtp, wall_ns(), wall_ns());
                } else if (externalStop && externalStop->load()) {
                    im->running = false;
                }
                // Ticking here rather than inside the request branch keeps the
                // progress report independent of how often the receiver asks for
                // the time (it polls every few seconds, and not at all while idle).
                if (im->cfg.onProgress) {
                    const uint64_t now = mono_ns();
                    if (now - lastReport > 2000000000ull) { // ~2 s
                        lastReport = now;
                        im->cfg.onProgress(im->snapshot());
                    }
                }
            }
        });
    }

    // --- audio ------------------------------------------------------------
    if (im->cfg.audio && im->audio.connected()) {
        im->audioThread = std::thread([this, externalStop]() {
            Impl* im = m_impl;
            AudioCapture cap;
            AacEncoder enc;
            if (!cap.start(kAudioRate, kAudioChannels) || !enc.start(kAudioRate, kAudioChannels, im->cfg.audioBitrate)) {
                LOGE("client: audio pipeline failed to start");
                return;
            }
            std::vector<float> pcm;
            std::vector<uint8_t> aac;
            uint32_t rtpTs = 0;
            uint64_t lastSync = 0;
            while (im->running.load() && !(externalStop && externalStop->load())) {
                uint32_t frames = 0;
                if (!cap.readPacket(pcm, frames) || frames == 0) {
                    Sleep(2);
                    continue;
                }
                aac.clear();
                const int n = enc.encode(pcm.data(), pcm.size() / kAudioChannels, aac);
                if (n <= 0 || aac.empty()) continue;
                if (!im->audio.sendAudio(aac.data(), aac.size(), rtpTs)) {
                    LOGE("client: audio send failed, stopping audio");
                    break;
                }
                rtpTs += kAudioSpf;
                // Sync packets let the receiver align our RTP clock with ours.
                // The RTP field names the access unit just sent, so take it back
                // one frame. The NTP field is wall clock:
                // raop_rtp.c runs it through
                // raop_remote_timestamp_to_nano_seconds(), which strips the
                // 1900 epoch, so it has to be wall clock plus that epoch.
                const uint64_t now = mono_ns();
                if (now - lastSync > 300000000ull) { // ~0.3 s
                    lastSync = now;
                    im->audio.sendSync(rtpTs - kAudioSpf, ns_to_ntp(wall_ns(), true), rtpTs);
                }
                im->withStats([&](Stats& s) {
                    s.audioPackets = im->audio.packetsSent();
                    s.audioBytes = im->audio.bytesSent();
                });
            }
        });
    }

    // --- video ------------------------------------------------------------
    // The video stream not being connected at this point means SETUP never gave
    // us a data port, which used to skip the video thread entirely and leave the
    // same "connected but no picture" state as a failed capture.
    if (im->cfg.video && !im->mirror.connected()) {
        im->failSession(
            L"client: the receiver gave no video stream - SETUP #2 returned no mirror dataPort");
    }
    if (im->cfg.video && im->mirror.connected()) {
        im->videoThread = std::thread([this, externalStop]() {
            Impl* im = m_impl;
            ScreenCapture cap;
            const bool capOk =
                im->cfg.testPattern
                    ? cap.startTestPattern(im->cfg.maxWidth > 0 ? (size_t)im->cfg.maxWidth : 1920,
                                           (size_t)((im->cfg.maxWidth > 0 ? im->cfg.maxWidth : 1920) *
                                                    9 / 16))
                    : cap.start((size_t)im->cfg.maxWidth, im->cfg.display);
            if (!capOk) {
                // The three capture APIs fail for different reasons, and which
                // one is left standing is the diagnosis. Put every code that
                // was tried into the message so a machine nobody can sit at
                // still reports something actionable.
                const std::string& why = cap.startDetail();
                LOGE("client: no picture from the desktop - capture could not start (%s)",
                     why.empty() ? "no detail" : why.c_str());
                wchar_t msg[400];
                std::wstring wide;
                for (char c : why) wide.push_back((wchar_t)(unsigned char)c);
                _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                             L"client: no picture from the desktop - capture could not start "
                             L"(%s)",
                             wide.c_str());
                im->failSession(msg);
                return;
            }
            H264Encoder enc;
            // Bitrate heuristic: bits per pixel per frame.
            // 0.12 was sized for camera video; desktop text has hard edges and
            // no motion to hide artefacts behind, so it went soft long before
            // the number looked "generous". 0.35 is the shipped default: on a
            // mostly idle desktop the rate controller never spends it (measured
            // 1.1 Mbps of a 22 Mbps budget), so it costs nothing until there is
            // actually detail to carry; on a moving window it is the difference
            // between readable and mushy text. The 120 Mbps ceiling only exists
            // so a 4K or high-refresh desktop is not clipped; --bitrate still
            // overrides the whole heuristic.
            int bitrate = im->cfg.videoBitrate;
            if (bitrate <= 0) {
                const double pixels = (double)cap.width() * (double)cap.height();
                double bpp = im->cfg.bitsPerPixel > 0 ? im->cfg.bitsPerPixel : kDefaultBpp;
                bitrate = (int)(pixels * im->cfg.fps * bpp);
                if (bitrate < 4000000) bitrate = 4000000;
                if (bitrate > 120000000) bitrate = 120000000;
            }
            if (!enc.start(cap.width(), cap.height(), im->cfg.fps, bitrate, true, im->cfg.maxQp,
                           im->cfg.hevc)) {
                // Capture worked here; the encoder is a different problem with
                // a different fix (driver / preset), so it gets its own text.
                // The detail says which candidate refused at which step, which
                // is the only way to tell "no H.264 encoder on this machine"
                // from "the one it has will not take this resolution".
                const std::string& why = enc.startDetail();
                LOGE("client: the H.264 encoder would not start %zux%zu (%s)", cap.width(),
                     cap.height(), why.empty() ? "no detail" : why.c_str());
                std::wstring wide;
                for (char c : why) wide.push_back((wchar_t)(unsigned char)c);
                wchar_t msg[400];
                _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                             L"client: the H.264 encoder would not start (%s)", wide.c_str());
                im->failSession(msg);
                return;
            }
            im->withStats([&](Stats& s) {
                s.width = (int)cap.width();
                s.height = (int)cap.height();
                s.fps = im->cfg.fps;
            });
            // The 128-byte mirror header carries the frame size in three
            // places; the receiver uses them to size its surface, so they must
            // be the *encoded* size, not the desktop size.
            im->mirror.setSize((uint16_t)cap.width(), (uint16_t)cap.height());
            std::vector<uint8_t> vps, sps, pps, nals;
            bool haveParams = false;
            uint64_t frames = 0;
            // Rolling capture+encode cost. Without it a 4K stream that quietly
            // drops to 12 fps looks exactly like a healthy one.
            uint64_t busyFrames = 0, busyNs = 0, perfMark = mono_ns(), framesAtMark = 0;
            const auto framePeriod =
                std::chrono::microseconds(1000000 / (im->cfg.fps > 0 ? im->cfg.fps : 30));
            auto nextFrame = std::chrono::steady_clock::now();
            // Windows can hand back "nothing new" forever without that being an
            // error (a static desktop), so only a repeated *error* counts here.
            int captureFailStreak = 0;

            while (im->running.load() && !(externalStop && externalStop->load())) {
                nextFrame += framePeriod;
                std::this_thread::sleep_until(nextFrame);

                const uint64_t frameStartNs = mono_ns();
                CapturedFrame f;
                if (!cap.acquireFrame(f) || !f.valid) {
                    // A locked screen or a remote session makes every capture
                    // call fail, which used to leave the receiver connected with
                    // a black picture and the GUI cheerfully showing "casting".
                    // Say it once, loudly, and end the session instead.
                    const unsigned long err = cap.lastError();
                    if (err == 0) {
                        captureFailStreak = 0;
                    } else if (frames == 0 && ++captureFailStreak == 60) { // ~2 s at 30 fps
                        // Only when nothing ever arrived: a screen that was
                        // captured fine and then locked is a temporary state
                        // recover() deals with, but one that never produced a
                        // single frame will not fix itself.
                        LOGE("client: no picture from the desktop (backend %s, Windows error %lu) - "
                             "the screen may be locked, or this is a remote session",
                             ScreenCapture::backend_name(cap.backend()), err);
                        wchar_t msg[240];
                        std::wstring wide;
                        for (const char* p = ScreenCapture::backend_name(cap.backend()); *p; ++p) {
                            wide.push_back((wchar_t)(unsigned char)*p);
                        }
                        _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                                     L"client: no picture from the desktop (backend %s, Windows "
                                     L"error %lu) - the screen may be locked, or this is a remote "
                                     L"session",
                                     wide.c_str(), err);
                        im->failSession(msg);
                        break;
                    }
                    continue;
                }
                captureFailStreak = 0;
                frames++;

                nals.clear();
                const int n = (f.format == PixelFormat::Nv12)
                                  ? enc.encodeNv12(f.data, f.width, f.height, f.timestampNs, nals)
                                  : enc.encodeFrame(f.data, f.width, f.height, f.stride,
                                                    f.timestampNs, nals);
                busyNs += mono_ns() - frameStartNs;
                busyFrames++;
                if (n <= 0 || nals.empty()) continue;

                // Parameter sets go out unencrypted and must carry the same
                // timestamp as the IDR they precede, because the receiver only
                // prepends them when the two timestamps match exactly.
                const bool paramsReady =
                    im->cfg.hevc ? enc.hevcParameterSets(vps, sps, pps) : enc.parameterSets(sps, pps);
                if (paramsReady) {
                    haveParams = true;
                    if (containsIdr(nals, im->cfg.hevc)) {
                        // The parameter-set packet is also what declares the
                        // codec, so it has to carry the IDR's timestamp.
                        if (im->cfg.hevc) {
                            im->mirror.sendHevcParameterSets(vps.data(), vps.size(), sps.data(),
                                                             sps.size(), pps.data(), pps.size(),
                                                             f.timestampNs);
                        } else {
                            im->mirror.sendParameterSets(sps.data(), sps.size(), pps.data(),
                                                         pps.size(), f.timestampNs);
                        }
                    }
                }
                const bool idr = containsIdr(nals, im->cfg.hevc);
                if (!haveParams && !idr) continue; // wait for the first IDR
                if (!im->mirror.sendFrame(nals, idr, f.timestampNs)) {
                    LOGE("client: mirror send failed, stopping video");
                    // A dead socket means the receiver is gone (it was closed, or
                    // the app was quit). Reporting it ends the session instead of
                    // leaving the GUI showing a cast that no longer exists. When
                    // the user asked to stop, the failure is expected: stay quiet.
                    if (!(externalStop && externalStop->load())) {
                        im->failSession(
                            L"client: the receiver closed the video connection");
                    }
                    break;
                }
                im->withStats([&](Stats& s) {
                    s.frames = frames;
                    s.videoPackets = im->mirror.packetsSent();
                    s.videoBytes = im->mirror.bytesSent();
                });

                const uint64_t nowNs = mono_ns();
                if (nowNs - perfMark > 5000000000ull) {
                    const double secs = (double)(nowNs - perfMark) / 1e9;
                    LOGI("video: %.1f fps sent, %.1f ms/frame capture+encode",
                         (double)(frames - framesAtMark) / secs,
                         busyFrames ? (double)busyNs / 1e6 / (double)busyFrames : 0.0);
                    perfMark = nowNs;
                    framesAtMark = frames;
                    busyFrames = 0;
                    busyNs = 0;
                }
            }
        });
    }

    // Park here until something asks us to finish; stop() does the teardown.
    while (im->running.load() && !(externalStop && externalStop->load())) Sleep(50);
    LOGI("client: stream loops finished, shutting the session down");

    if (im->timingThread.joinable()) im->timingThread.join();
    if (im->videoThread.joinable()) im->videoThread.join();
    if (im->audioThread.joinable()) im->audioThread.join();
    LOGI("client: stream threads joined");
}

void FlowCastClient::stop() {
    if (!m_impl) return;
    Impl* im = m_impl;
    // Always join, even when running is already false: a stream thread that
    // ended the session itself leaves running false while its std::thread is
    // still joinable, and tearing the session down under a live thread would be
    // a use-after-free.
    im->running = false;
    if (im->timingThread.joinable()) im->timingThread.join();
    if (im->videoThread.joinable()) im->videoThread.join();
    if (im->audioThread.joinable()) im->audioThread.join();

    // Tell the receiver we are done so it frees the session immediately.
    if (im->rtsp.connected()) {
        RtspResponse r;
        PlistValue body = PlistValue::dictOf({
            {"streams", PlistValue::array({
                           PlistValue::dictOf({{"type", PlistValue::integer(kStreamTypeAudio)}}),
                           PlistValue::dictOf({{"type", PlistValue::integer(kStreamTypeMirror)}}),
                       })},
        });
        std::string wire = plist_dump_bin(body);
        char turl[256];
        snprintf(turl, sizeof(turl), "rtsp://%s:%u/4", im->rtsp.peerAddress().c_str(),
                 (unsigned)im->port);
        im->rtsp.request("TEARDOWN", turl, "application/x-apple-binary-plist", wire, r, 2000);
        im->rtsp.close();
    }
    im->mirror.close();
    im->audio.close();
    im->timing.stop();

    {
        std::lock_guard<std::mutex> g(m_statsMutex);
        m_stats = im->snapshot();
    }
    m_active = false;
    delete im;
    m_impl = nullptr;
}

} // namespace fc
