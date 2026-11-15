// FlowCast Windows client - NTP timing client (AirPlay clock sync).
//
// The receiver sends 32-byte timing requests to our timingPort; we answer with
// a matching response so it can compute the offset between our clock and its
// own. All timestamps use the iOS convention of "seconds since boot plus the
// 1900->1970 epoch offset", which is what raop_ntp.c expects.
#pragma once
#include <cstdint>
#include <ctime>

namespace fc {

// Seconds between the NTP (1900) and Unix (1970) epochs.
constexpr uint64_t kNtpEpochOffset = 2208988800ULL;

// Monotonic-ish uptime in nanoseconds (excludes sleep).
uint64_t mono_ns();

// Wall clock in nanoseconds since the Unix epoch.
uint64_t wall_ns();

// Converts a (seconds << 32 | fraction) NTP pair into nanoseconds, optionally
// subtracting the 1900 epoch offset (iOS-style boot-relative clocks).
inline uint64_t ntp_to_ns(uint64_t ntp, bool bootRelative) {
    uint64_t seconds = (ntp >> 32) & 0xffffffffu;
    if (bootRelative) seconds -= kNtpEpochOffset;
    uint64_t fraction = ntp & 0xffffffffu;
    return seconds * 1000000000ull + ((fraction * 1000000000ull) >> 32);
}

// Packs nanoseconds into the (seconds << 32 | fraction) NTP form, adding the
// 1900 epoch offset so the receiver sees an iOS-style boot-relative clock.
inline uint64_t ns_to_ntp(uint64_t ns, bool bootRelative) {
    uint64_t seconds = ns / 1000000000ull;
    uint64_t fraction = ((ns % 1000000000ull) << 32) / 1000000000ull;
    if (bootRelative) seconds += kNtpEpochOffset;
    return (seconds << 32) | fraction;
}

class TimingClient {
public:
    TimingClient() = default;
    ~TimingClient();

    // Opens an ephemeral UDP port and returns it. `onRequest` is called on this
    // thread for every timing request; answer inside the callback.
    bool start();
    void stop();
    uint16_t localPort() const { return m_port; }

    // Blocks until the next timing request arrives (or the timeout expires).
    // Returns true when a request was received; the response is then sent with
    // `reply()`. `serverSendNtp` is the raw NTP value from the request that
    // must be echoed back verbatim.
    bool waitRequest(int timeoutMs, uint64_t& serverSendNtp, uint32_t& seq);

    // `serverSendNtp` is echoed as-is; `clientRecvNs`/`clientSendNs` are our
    // boot-relative clock in nanoseconds and get the 1900 epoch added.
    void reply(uint64_t serverSendNtp, uint64_t clientRecvNs, uint64_t clientSendNs);

private:
    struct Impl;
    Impl* m_impl = nullptr;
    uint16_t m_port = 0;
};

} // namespace fc
