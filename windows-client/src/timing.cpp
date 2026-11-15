// FlowCast Windows client - NTP timing client implementation.
#include "timing.h"
#include "bytebuf.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace fc {

uint64_t mono_ns() {
    // QueryUnbiasedInterruptTime returns 100ns units of time the system has
    // actually been running (sleep is excluded), which matches the semantics
    // iOS uses for its mirroring timestamps.
    ULONGLONG ticks = 0;
    QueryUnbiasedInterruptTime(&ticks);
    return (uint64_t)ticks * 100ull;
}

uint64_t wall_ns() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // FILETIME epoch is 1601-01-01, Unix epoch is 1970-01-01.
    return (u.QuadPart - 116444736000000000ull) * 100ull;
}

struct TimingClient::Impl {
    SOCKET sock = INVALID_SOCKET;
    bool wsaUp = false;
    sockaddr_in lastFrom{};
    bool haveFrom = false;
};

TimingClient::~TimingClient() {
    stop();
}

bool TimingClient::start() {
    stop();
    Impl* im = new Impl();
    m_impl = im;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    im->wsaUp = true;

    im->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (im->sock == INVALID_SOCKET) {
        LOGE("timing: socket() failed (%d)", WSAGetLastError());
        return false;
    }
    BOOL reuse = TRUE;
    setsockopt(im->sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = INADDR_ANY;
    any.sin_port = 0;
    if (bind(im->sock, (sockaddr*)&any, sizeof(any)) != 0) {
        LOGE("timing: bind() failed (%d)", WSAGetLastError());
        return false;
    }
    sockaddr_in bound{};
    int len = sizeof(bound);
    getsockname(im->sock, (sockaddr*)&bound, &len);
    m_port = ntohs(bound.sin_port);
    LOGI("timing: listening on UDP %u", (unsigned)m_port);
    return true;
}

void TimingClient::stop() {
    if (!m_impl) return;
    if (m_impl->sock != INVALID_SOCKET) closesocket(m_impl->sock);
    if (m_impl->wsaUp) WSACleanup();
    delete m_impl;
    m_impl = nullptr;
    m_port = 0;
}

bool TimingClient::waitRequest(int timeoutMs, uint64_t& serverSendNtp, uint32_t& seq) {
    if (!m_impl || m_impl->sock == INVALID_SOCKET) return false;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(m_impl->sock, &rfds);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    if (select(0, &rfds, nullptr, nullptr, &tv) <= 0) return false;

    uint8_t req[64];
    sockaddr_in from{};
    int fromLen = sizeof(from);
    int n = recvfrom(m_impl->sock, (char*)req, sizeof(req), 0, (sockaddr*)&from, &fromLen);
    if (n < 32) return false;

    // request[0:4] = 0x80 0xd2 0x00 0x07
    if (req[0] != 0x80 || req[1] != 0xd2) {
        LOGW("timing: unexpected packet type 0x%02x 0x%02x", req[0], req[1]);
        return false;
    }
    m_impl->lastFrom = from;
    m_impl->haveFrom = true;

    // Echo the server's own send timestamp straight back, as raop_ntp.c does.
    serverSendNtp = Buf::getU64BE(req + 24);
    seq = Buf::getU32BE(req + 8);
    LOGD("timing: request seq=%u serverSendNtp=%llx", (unsigned)seq,
         (unsigned long long)serverSendNtp);
    return true;
}

void TimingClient::reply(uint64_t serverSendNtp, uint64_t clientRecvNs, uint64_t clientSendNs) {
    if (!m_impl || !m_impl->haveFrom) return;

    uint8_t res[32];
    memset(res, 0, sizeof(res));
    res[0] = 0x80;
    res[1] = 0xd3; // response marker (0xd2 | 0x01)
    res[2] = 0x00;
    res[3] = 0x07;
    Buf::putU64BE(res, 8, serverSendNtp);                     // t0: echo
    Buf::putU64BE(res, 16, ns_to_ntp(clientRecvNs, true));    // t1: our recv time
    Buf::putU64BE(res, 24, ns_to_ntp(clientSendNs, true));    // t2: our send time

    int sent = sendto(m_impl->sock, (const char*)res, sizeof(res), 0,
                      (sockaddr*)&m_impl->lastFrom, sizeof(m_impl->lastFrom));
    if (sent != sizeof(res)) {
        LOGW("timing: reply send failed (%d)", WSAGetLastError());
    } else {
        LOGD("timing: replied t1=%.6f t2=%.6f", (double)clientRecvNs / 1e9,
             (double)clientSendNs / 1e9);
    }
}

} // namespace fc
