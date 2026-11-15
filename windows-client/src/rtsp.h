// FlowCast Windows client - RTSP/1.0 request-response client over TCP.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <map>
#include <string>

namespace fc {

struct RtspResponse {
    int status = 0;
    std::string reason;
    std::string protocol;
    std::map<std::string, std::string> headers; // keys lower-cased
    std::string body;
    std::string rawHeaderLine; // status line + headers, CRLF separated

    // Why there is no status line, when there is none. 0 would otherwise mean
    // both "the receiver never answered" and "we never asked", and the two need
    // different advice. kPeerClosed is our own code for a clean close (recv
    // returned 0), which WSAGetLastError() reports as whatever happened last.
    int err = 0;
    static constexpr int kPeerClosed = -1;

    const std::string* header(const char* name) const {
        for (const auto& kv : headers) {
            if (kv.first == name) return &kv.second;
        }
        return nullptr;
    }
    long contentLength() const {
        const std::string* v = header("content-length");
        return v ? strtol(v->c_str(), nullptr, 10) : 0;
    }
};

class RtspClient {
public:
    RtspClient() = default;
    ~RtspClient();
    RtspClient(const RtspClient&) = delete;
    RtspClient& operator=(const RtspClient&) = delete;

    // `host` may be a dotted IPv4 or a hostname. Returns false with a log line.
    bool connect(const char* host, uint16_t port, int timeoutMs = 4000);
    void close();
    bool connected() const { return m_impl != nullptr; }

    // Drops the socket and dials the same peer again. A receiver that hangs up
    // mid-handshake leaves the connection unusable, and every later request on
    // it fails with a socket error that looks like a different bug. Returns
    // false when there was no peer to redial.
    bool reconnect(int timeoutMs = 4000);

    // Sends one request and waits for the matching response. `contentType` may
    // be null for an empty body.
    //
    // `userAgent` and `extraHeaders` exist for the capability probe only: some
    // receivers answer a request differently depending on who they think is
    // calling, and telling "the endpoint is missing" apart from "the endpoint
    // disliked this caller" is the difference between a real fix and a rewrite.
    // `extraHeaders` is raw CRLF-terminated lines.
    bool request(const char* method, const char* url, const char* contentType,
                 const void* body, size_t bodyLen, RtspResponse& out, int timeoutMs = 5000,
                 const char* userAgent = nullptr, const char* extraHeaders = nullptr);

    // Convenience: body as a std::string.
    bool request(const char* method, const char* url, const char* contentType,
                 const std::string& body, RtspResponse& out, int timeoutMs = 5000,
                 const char* userAgent = nullptr, const char* extraHeaders = nullptr);

    // Default identity for every request that does not pass its own. Some
    // receivers decide whether to answer by who they think is calling, so the
    // ability to look like an Apple sender belongs here rather than being
    // sprinkled through the client. An explicit argument to request() still
    // wins, which is what the probe relies on.
    void set_identity(const char* userAgent, const char* extraHeaders);

    const std::string& peerAddress() const { return m_peerIp; }

private:
    struct Impl;
    Impl* m_impl = nullptr;
    std::string m_peerIp;
    // Remembered so reconnect() can redial without the caller repeating itself.
    std::string m_host;
    uint16_t m_port = 0;
    std::string m_ua;
    std::string m_extra;
    // Filled from the first response that carries a Session header, then sent
    // on every later request. A receiver that keeps a session table uses this
    // to match SETUP #2 / RECORD to the session SETUP #1 created; without it
    // those requests arrive as orphans. Cleared on connect(), because a session
    // id belongs to one RTSP connection.
    std::string m_session;
    int m_cseq = 0;
};

// Resolves a hostname or dotted quad to an IPv4 sockaddr. Returns false if the
// name cannot be resolved (".local" names usually need the mDNS answer).
bool net_resolve_ipv4(const char* host, uint16_t port, void* sockaddrOut, int timeoutMs);

// One word for a socket-level failure: "no reply" (timed out), "dropped" (the
// peer reset or closed), "refused" (nothing listening), "send failed". Used
// wherever a status code would be, because "(0)" says nothing to a reader.
const char* net_error_text(int err);

} // namespace fc
