// FlowCast Windows client - top-level orchestration.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace fc {

// Live counters for the running session. Aliased as FlowCastClient::Stats so
// callers can keep using either spelling.
struct ClientStats {
    uint64_t videoPackets = 0;
    uint64_t videoBytes = 0;
    uint64_t audioPackets = 0;
    uint64_t audioBytes = 0;
    uint64_t frames = 0;
    double elapsedSec = 0.0;
    int width = 0;
    int height = 0;
    int fps = 0;
};

// See ClientConfig::fairplay. kHelloOnly is the interesting one: it keeps the
// 16-byte greeting (some receivers refuse everything until they have seen it)
// and drops the 164-byte key message, which is the step that makes strict
// third-party units stop answering altogether.
enum class FairplayMode {
    kFull,      // greeting + key message, i.e. the whole legacy exchange
    kHelloOnly, // greeting only
    kSkip,      // nothing at all
};

// How the session is described to the receiver.
//
// kTwoStage is what this client used to speak: SETUP /1 carries ekey + timing,
// SETUP /2 carries the streams. UxPlay and the Sony panels accept it because
// their handler takes both halves, but it is not the shape an Apple sender
// sends: iOS puts `isScreenMirroring` and the whole `streams` array (with
// `encryptionType` and the keys inside the stream dict) in the FIRST SETUP and
// never sends a second one. A receiver written against the Apple shape reads
// SETUP /1, finds no stream of type 110, has nothing to set up - and closes,
// which is exactly the silent hang-up seen on MAXHUB.
enum class SetupStyle {
    kTwoStage,
    kSingle, // one SETUP /1 in the shape iOS uses
};

struct ClientConfig {
    std::string host;          // empty = discover over mDNS
    // Only accept a discovered receiver whose service instance name contains
    // this substring (case-insensitive). Empty = the first one found.
    std::string receiverMatch;
    uint16_t port = 7000;      // AirPlay port advertised in mDNS
    std::string name = "Windows PC";
    int fps = 30;
    int videoBitrate = 0;      // 0 = pick from resolution and bitsPerPixel
    // Bits per pixel per frame for the automatic bitrate. Screen content needs
    // noticeably more than camera video at the same resolution: text has hard
    // edges and no motion masking to hide quantisation behind. 0.12 (the
    // original) starved 1080p text, 0.24 was better but still soft on a moving
    // window, 0.35 is where the encoder stops being the visible bottleneck.
    // An idle desktop does not spend it -- the rate controller only takes what
    // the content needs -- so raising this costs bandwidth only when there is
    // detail on screen.
    double bitsPerPixel = 0.35;
    // Quantiser ceiling (H.264 QP, 0-51). Guards small text against a rate
    // controller that would rather raise QP than spend bits on a static frame.
    // 0 = leave the encoder's own ceiling alone.
    int maxQp = 30;
    // Encode H.265 instead of H.264. The receiver learns the codec from the
    // parameter-set packet (an "hvc1" marker), so this needs no other signalling.
    bool hevc = false;
    int audioBitrate = 128000;
    int maxWidth = 1920;       // 0 = native desktop size
    // Screen to capture, as a "\\.\DISPLAYn" device name; empty = the primary
    // display. Needed to mirror a 4K TV set up as an extended desktop: the
    // primary panel here is only 1080p, so "native" would still be 1080p.
    std::wstring display;
    bool video = true;
    bool audio = true;
    bool verbose = false;
    int discoverMs = 3000;
    // Send a synthetic colour-bar picture instead of capturing a desktop. A
    // troubleshooting switch: a remote session cannot be captured at all, and
    // this is the only way to exercise encode + send from one.
    bool testPattern = false;
    // How much of /fp-setup to send before SETUP.
    //
    // Our mirror stream is plaintext: SETUP #1 carries no `et` key, so no
    // receiver decrypts the video with the FairPlay session key anyway. Some
    // third-party receivers therefore never implement /fp-setup and simply
    // drop it - MAXHUB answers the greeting and then goes silent on the key
    // message. On those, sending less of the handshake is the difference
    // between "refused" and a working mirror; on a receiver that does check it,
    // SETUP #1 comes back non-200 and nothing is lost but one run.
    FairplayMode fairplay = FairplayMode::kFull;
    // Which SETUP shape to speak. The ladder varies this too; see SetupStyle.
    SetupStyle setup = SetupStyle::kTwoStage;
    // Set when the caller pinned `setup` by hand; pins the ladder to one run.
    bool setupPinned = false;
    // Set when the caller picked `fairplay` by hand (command line). The ladder
    // must then run exactly that variant: overriding a deliberate choice is how
    // --skip-fairplay silently turned into "FairPlay both rounds".
    bool fairplayPinned = false;
    // Milliseconds to wait for /fp-setup #2. A receiver that implements the
    // exchange answers in microseconds, so this wait is only ever spent on one
    // that silently drops the request. 6 s rather than 3 s because "answers
    // late" and "does not answer" have to be told apart exactly once, and the
    // cost only appears in the failing case.
    int fp2TimeoutMs = 6000;
    // Try the fallback handshake variants when the first one fails. On by
    // default: for a third-party panel it is the only second chance it gets.
    // Off for scripted runs, where a failure should fail fast instead of
    // exploring four ways to fail.
    bool retryVariants = true;
    // Announce ourselves the way an Apple sender does - User-Agent plus the
    // X-Apple-* headers - on every request of the session. Panels that gate on
    // the caller rather than on the endpoint answer this and ignore the other;
    // nothing in the stream depends on it, so it costs one flag to test.
    bool appleUa = false;

    // Called from the timing thread about every two seconds with a consistent
    // snapshot of the counters. May be empty.
    std::function<void(const ClientStats&)> onProgress;
};

class FlowCastClient {
public:
    FlowCastClient() = default;
    ~FlowCastClient();

    // Performs discovery, the RTSP/FairPlay handshake and opens every stream.
    // Returns false with a log line explaining where it stopped.
    bool start(const ClientConfig& cfg);

    // Runs the capture/encode/send loops. Returns when stop() is called or when
    // `externalStop` (may be null) becomes true. The session stays open until
    // stop() is called afterwards.
    void run(const std::atomic<bool>* externalStop = nullptr);

    // Signals the loops to exit and tears the session down.
    void stop();

    // Why the session ended by itself, or empty when the caller stopped it. A
    // stream thread that cannot start (no capturable desktop, an encoder this
    // machine does not have) now ends the whole session and reports here,
    // instead of leaving a connection up that carries no picture.
    std::wstring error() const;

    // True while mirroring is active.
    bool active() const { return m_active; }

    using Stats = ClientStats;
    Stats stats() const;

private:
    // One attempt, no fallback. start() wraps this in a ladder of attempts that
    // each give up a little more protocol correctness.
    bool start_once(const ClientConfig& cfg);
    // Records how far an attempt got. Only the deepest one is worth reporting:
    // "the receiver hung up on SETUP #1" says something, the noise from a
    // variant that never got past OPTIONS says nothing.
    void note_depth(int depth, const char* step, const std::string& code);

    int m_rank = 0;          // depth * 2 + (the receiver answered with a status)
    int m_depth = 0;         // deepest step number any attempt reached
    std::string m_deepStep;  // step name of the deepest failure, e.g. "SETUP #1"
    std::string m_deepCode;  // "(...)": a status code, or dropped/no reply
    bool m_sawStatus = false;
    // /fp-setup #1 answered and #2 stayed silent. Worth its own flag: it is the
    // shape of a receiver that ships the greeting as a stub, and the summary
    // says so instead of leaving the reader with "SETUP was dropped".
    bool m_fp2Silent = false;
    // Resolved once, so the ladder does not run mDNS five times.
    std::string m_host;
    uint16_t m_hostPort = 0;

    struct Impl;
    Impl* m_impl = nullptr;
    bool m_active = false;
    mutable std::mutex m_statsMutex;
    Stats m_stats;
};

} // namespace fc
