// FlowCast Windows client - RTSP client implementation.
#include "rtsp.h"
#include "log.h"
#include "bytebuf.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cctype>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace fc {
namespace {

std::string lower(const std::string& s) {
    std::string o = s;
    for (char& c : o) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return o;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

bool setSocketTimeout(SOCKET s, int ms) {
    DWORD tv = (DWORD)ms;
    return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)) == 0 &&
           setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv)) == 0;
}

} // namespace

const char* net_error_text(int err) {
    switch (err) {
    case 0: return "no answer";
    case WSAETIMEDOUT: return "no reply";
    case WSAECONNRESET:
    case WSAECONNABORTED:
    case RtspResponse::kPeerClosed: return "dropped";
    case WSAECONNREFUSED: return "refused";
    case WSAENOTCONN: return "closed";
    case WSAENETUNREACH:
    case WSAEHOSTUNREACH: return "unreachable";
    default: return "socket error";
    }
}

struct RtspClient::Impl {
    SOCKET sock = INVALID_SOCKET;
    bool wsaUp = false;
};

RtspClient::~RtspClient() {
    close();
}

void RtspClient::close() {
    if (m_impl) {
        if (m_impl->sock != INVALID_SOCKET) closesocket(m_impl->sock);
        if (m_impl->wsaUp) WSACleanup();
        delete m_impl;
        m_impl = nullptr;
    }
}

bool net_resolve_ipv4(const char* host, uint16_t port, void* sockaddrOut, int timeoutMs) {
    (void)timeoutMs;
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);

    // A dotted quad is the common case (mDNS already resolved the name).
    if (inet_pton(AF_INET, host, &sin.sin_addr) == 1) {
        memcpy(sockaddrOut, &sin, sizeof(sin));
        return true;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) {
        LOGE("rtsp: cannot resolve '%s' (%d)", host, WSAGetLastError());
        return false;
    }
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET && ai->ai_addrlen >= sizeof(sockaddr_in)) {
            memcpy(&sin.sin_addr, &((sockaddr_in*)ai->ai_addr)->sin_addr, 4);
            break;
        }
    }
    freeaddrinfo(res);
    memcpy(sockaddrOut, &sin, sizeof(sin));
    return true;
}

bool RtspClient::connect(const char* host, uint16_t port, int timeoutMs) {
    close();
    Impl* im = new Impl();
    m_impl = im;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOGE("rtsp: WSAStartup failed");
        return false;
    }
    im->wsaUp = true;

    sockaddr_in sin{};
    if (!net_resolve_ipv4(host, port, &sin, timeoutMs)) return false;

    im->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (im->sock == INVALID_SOCKET) {
        LOGE("rtsp: socket() failed (%d)", WSAGetLastError());
        return false;
    }
    setSocketTimeout(im->sock, timeoutMs);
    if (::connect(im->sock, (sockaddr*)&sin, sizeof(sin)) != 0) {
        LOGE("rtsp: connect to %s:%u failed (%d)", host, (unsigned)port, WSAGetLastError());
        return false;
    }
    setSocketTimeout(im->sock, 5000);

    char ip[64] = {0};
    inet_ntop(AF_INET, &sin.sin_addr, ip, sizeof(ip));
    m_peerIp = ip;
    m_host = host;
    m_port = port;
    m_cseq = 0;
    // A session id is scoped to the connection that created it; carrying it
    // across a redial would address a session that no longer exists.
    m_session.clear();
    LOGI("rtsp: connected to %s:%u", m_peerIp.c_str(), (unsigned)port);
    return true;
}

bool RtspClient::reconnect(int timeoutMs) {
    if (m_host.empty() || m_port == 0) {
        LOGE("rtsp: reconnect without a previous peer");
        return false;
    }
    // The identity survives: it is a property of who we claim to be, not of the
    // socket, and a receiver that wanted the Apple identity still wants it.
    LOGI("rtsp: reopening the connection to %s:%u", m_host.c_str(), (unsigned)m_port);
    return connect(m_host.c_str(), m_port, timeoutMs);
}

void RtspClient::set_identity(const char* userAgent, const char* extraHeaders) {
    m_ua = userAgent ? userAgent : "";
    m_extra = extraHeaders ? extraHeaders : "";
}

bool RtspClient::request(const char* method, const char* url, const char* contentType,
                         const void* body, size_t bodyLen, RtspResponse& out, int timeoutMs,
                         const char* userAgent, const char* extraHeaders) {
    if (!m_impl || m_impl->sock == INVALID_SOCKET) {
        LOGE("rtsp: request on a closed connection");
        out.err = WSAENOTCONN;
        return false;
    }
    out = RtspResponse();
    m_cseq++;

    // Per-request, not per-connection: the socket timeout set by connect() is a
    // 5 s default, and callers that pass 3000 for a request a receiver either
    // answers instantly or not at all were being silently ignored - which is
    // why a dropped /fp-setup always cost a full 5 s.
    setSocketTimeout(m_impl->sock, timeoutMs);

    Buf req;
    char line[512];
    snprintf(line, sizeof(line), "%s %s RTSP/1.0\r\n", method, url);
    req.append(line, strlen(line));
    snprintf(line, sizeof(line), "CSeq: %d\r\n", m_cseq);
    req.append(line, strlen(line));
    // NOTE: always size these with strlen(). A hardcoded byte count that drops
    // the final "\n" makes the receiver's header parser fail and it closes the
    // connection instead of answering.
    const char* ua = (userAgent && *userAgent) ? userAgent : (m_ua.empty() ? "FlowCast/1.0" : m_ua.c_str());
    snprintf(line, sizeof(line), "User-Agent: %s\r\n", ua);
    req.append(line, strlen(line));
    if (extraHeaders && *extraHeaders) {
        req.append(extraHeaders, strlen(extraHeaders));
    } else if (m_extra.size()) {
        req.append(m_extra.data(), m_extra.size());
    }
    // Echo the session id once the receiver has assigned one. iOS does this on
    // every request after SETUP #1, and a receiver with a session table looks up
    // the id before it will touch the request body - an orphan request is
    // closed rather than answered.
    if (!m_session.empty() && strcmp(method, "OPTIONS") != 0) {
        snprintf(line, sizeof(line), "Session: %s\r\n", m_session.c_str());
        req.append(line, strlen(line));
    }
    if (bodyLen > 0 && contentType) {
        snprintf(line, sizeof(line), "Content-Type: %s\r\n", contentType);
        req.append(line, strlen(line));
        snprintf(line, sizeof(line), "Content-Length: %zu\r\n", bodyLen);
        req.append(line, strlen(line));
    }
    req.append("\r\n", 2);
    if (bodyLen > 0 && body) req.append(body, bodyLen);

    int sent = ::send(m_impl->sock, (const char*)req.data(), (int)req.size(), 0);
    if (sent != (int)req.size()) {
        out.err = WSAGetLastError();
        LOGE("rtsp: send failed while writing %s %s (%d, %s)", method, url, out.err,
             net_error_text(out.err));
        return false;
    }
    LOGD("rtsp: --> %s %s (CSeq %d, %zu body bytes)", method, url, m_cseq, bodyLen);

    // --- read the header block ---------------------------------------------
    std::string hdr;
    for (;;) {
        size_t eol = hdr.find("\r\n\r\n");
        if (eol != std::string::npos) {
            out.rawHeaderLine = hdr.substr(0, eol + 2);
            hdr.erase(0, eol + 4);
            break;
        }
        char tmp[2048];
        int n = ::recv(m_impl->sock, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            // n == 0 is a clean close, and WSAGetLastError() then reports
            // whatever the previous call left behind - quoting it would blame a
            // timeout that never happened.
            out.err = n == 0 ? RtspResponse::kPeerClosed : WSAGetLastError();
            LOGE("rtsp: recv failed while reading %s %s (%d, %s)", method, url, out.err,
                 net_error_text(out.err));
            return false;
        }
        hdr.append(tmp, (size_t)n);
        if (hdr.size() > 65536) {
            LOGE("rtsp: response header too large");
            return false;
        }
    }

    // status line
    size_t p = out.rawHeaderLine.find("\r\n");
    std::string statusLine =
        p == std::string::npos ? out.rawHeaderLine : out.rawHeaderLine.substr(0, p);
    {
        size_t s1 = statusLine.find(' ');
        size_t s2 = statusLine.find(' ', s1 == std::string::npos ? 0 : s1 + 1);
        if (s1 == std::string::npos) {
            LOGE("rtsp: malformed status line '%s'", statusLine.c_str());
            return false;
        }
        out.protocol = statusLine.substr(0, s1);
        out.status = atoi(statusLine.substr(s1 + 1, s2 - s1 - 1).c_str());
        out.reason = s2 == std::string::npos ? "" : trim(statusLine.substr(s2 + 1));
    }

    // headers
    size_t pos = p == std::string::npos ? out.rawHeaderLine.size() : p + 2;
    while (pos < out.rawHeaderLine.size()) {
        size_t e = out.rawHeaderLine.find("\r\n", pos);
        if (e == std::string::npos) e = out.rawHeaderLine.size();
        std::string l = out.rawHeaderLine.substr(pos, e - pos);
        pos = e + 2;
        size_t c = l.find(':');
        if (c == std::string::npos) continue;
        out.headers[lower(trim(l.substr(0, c)))] = trim(l.substr(c + 1));
    }

    // Remember the session id the receiver hands out, so the requests that
    // follow are addressed to the session they belong to instead of arriving as
    // orphans. Kept verbatim: some receivers append ";timeout=..." and compare
    // the whole string.
    if (m_session.empty()) {
        if (const std::string* sid = out.header("session")) {
            if (!sid->empty()) {
                m_session = *sid;
                LOGI("rtsp: session id %s", m_session.c_str());
            }
        }
    }

    // --- read the body ------------------------------------------------------
    long len = out.contentLength();
    if (len > 0) {
        out.body.append(hdr);
        while ((long)out.body.size() < len) {
            char tmp[8192];
            int n = ::recv(m_impl->sock, tmp, (int)sizeof(tmp), 0);
            if (n <= 0) {
                out.err = n == 0 ? RtspResponse::kPeerClosed : WSAGetLastError();
                LOGE("rtsp: recv failed while reading body (%d, %s)", out.err,
                     net_error_text(out.err));
                return false;
            }
            out.body.append(tmp, (size_t)n);
        }
        if ((long)out.body.size() > len) {
            // Only happens if the server sent more than Content-Length bytes.
            out.body.resize((size_t)len);
        }
    } else if (!hdr.empty()) {
        out.body = hdr;
    }

    LOGD("rtsp: <-- %d %s (%zu body bytes)", out.status, out.reason.c_str(), out.body.size());
    return out.status > 0;
}

bool RtspClient::request(const char* method, const char* url, const char* contentType,
                         const std::string& body, RtspResponse& out, int timeoutMs,
                         const char* userAgent, const char* extraHeaders) {
    return request(method, url, contentType, body.data(), body.size(), out, timeoutMs, userAgent,
                   extraHeaders);
}

} // namespace fc
