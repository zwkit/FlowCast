// FlowCast Windows client - RAOP audio stream implementation.
#include "audio_stream.h"
#include "crypto.h"
#include "log.h"
#include "rtsp.h"
#include "bytebuf.h"
#include "timing.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace fc {

struct AudioStream::Impl {
    SOCKET dataSock = INVALID_SOCKET;
    SOCKET ctrlSock = INVALID_SOCKET;
    bool wsaUp = false;
    sockaddr_in dataTo{};
    sockaddr_in ctrlTo{};
    fc::AesCbc cbc;
    bool keysReady = false;
    uint8_t aesiv[16] = {0};
};

AudioStream::~AudioStream() {
    close();
}

void AudioStream::setKeys(const uint8_t aeskey[16], const uint8_t aesiv[16]) {
    if (!m_impl) m_impl = new Impl();
    Impl* im = m_impl;
    im->cbc.init(aeskey, aesiv);
    memcpy(im->aesiv, aesiv, 16);
    im->keysReady = true;
    LOGI("audio: stream keys set");
}

bool AudioStream::openSockets(uint16_t& localControlPort) {
    close();
    Impl* im = new Impl();
    m_impl = im;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    im->wsaUp = true;

    im->dataSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    im->ctrlSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (im->dataSock == INVALID_SOCKET || im->ctrlSock == INVALID_SOCKET) {
        LOGE("audio: socket() failed (%d)", WSAGetLastError());
        return false;
    }

    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = INADDR_ANY;
    any.sin_port = 0;
    if (bind(im->dataSock, (sockaddr*)&any, sizeof(any)) != 0 ||
        bind(im->ctrlSock, (sockaddr*)&any, sizeof(any)) != 0) {
        LOGE("audio: bind() failed (%d)", WSAGetLastError());
        return false;
    }
    sockaddr_in bound{};
    int len = sizeof(bound);
    getsockname(im->dataSock, (sockaddr*)&bound, &len);
    m_localDataPort = ntohs(bound.sin_port);
    getsockname(im->ctrlSock, (sockaddr*)&bound, &len);
    m_localControlPort = ntohs(bound.sin_port);
    m_seq = 0;
    localControlPort = m_localControlPort;
    LOGI("audio: local UDP data %u control %u", (unsigned)m_localDataPort,
         (unsigned)m_localControlPort);
    return true;
}

bool AudioStream::setRemote(const char* host, uint16_t dataPort, uint16_t controlPort) {
    if (!m_impl || m_impl->dataSock == INVALID_SOCKET) return false;
    Impl* im = m_impl;
    if (!net_resolve_ipv4(host, dataPort, &im->dataTo, 4000)) return false;
    if (!net_resolve_ipv4(host, controlPort, &im->ctrlTo, 4000)) return false;
    m_connected = true;
    LOGI("audio: remote %s data:%u ctrl:%u", host, (unsigned)dataPort, (unsigned)controlPort);
    return true;
}

void AudioStream::close() {
    if (!m_impl) return;
    if (m_impl->dataSock != INVALID_SOCKET) closesocket(m_impl->dataSock);
    if (m_impl->ctrlSock != INVALID_SOCKET) closesocket(m_impl->ctrlSock);
    if (m_impl->wsaUp) WSACleanup();
    delete m_impl;
    m_impl = nullptr;
    m_connected = false;
}

bool AudioStream::sendAudio(const uint8_t* aac, size_t len, uint32_t rtpTimestamp) {
    if (!m_impl || !m_connected || !aac || len == 0) return false;
    Impl* im = m_impl;

    // Header + payload; the CBC prefix covers whole 16-byte blocks only.
    const size_t encBlocks = (len / 16) * 16;
    Buf pkt;
    pkt.resize(12 + len);
    pkt.data()[0] = 0x80;
    pkt.data()[1] = 0x60; // payload type 96
    pkt.putU16BE(2, m_seq);
    pkt.putU32BE(4, rtpTimestamp);
    // [8:12] stays zero (SSRC / marker field as the receiver expects)
    memcpy(pkt.data() + 12, aac, len);
    if (encBlocks > 0) {
        if (!im->keysReady) {
            LOGE("audio: encrypting without keys");
            return false;
        }
        im->cbc.encrypt_reset(pkt.data() + 12, pkt.data() + 12, encBlocks / 16);
    }
    m_seq++;

    int n = sendto(im->dataSock, (const char*)pkt.data(), (int)pkt.size(), 0,
                   (sockaddr*)&im->dataTo, sizeof(im->dataTo));
    if (n != (int)pkt.size()) {
        LOGW("audio: data send failed (%d)", WSAGetLastError());
        return false;
    }
    m_packets++;
    m_bytes += pkt.size();
    return true;
}

bool AudioStream::sendSync(uint32_t syncRtp, uint64_t clientNtp, uint32_t nextRtp) {
    if (!m_impl || !m_connected) return false;
    Impl* im = m_impl;

    Buf pkt;
    pkt.resize(20);
    // The first sync uses 0x90, later ones 0x80; the receiver only masks the
    // high bit, so 0x90 is always safe and avoids tracking "first" state.
    pkt.data()[0] = 0x90;
    pkt.data()[1] = 0xd4;
    pkt.data()[2] = 0x00;
    pkt.data()[3] = 0x04;
    pkt.putU32BE(4, syncRtp);
    pkt.putU64BE(8, clientNtp);
    pkt.putU32BE(16, nextRtp);

    int n = sendto(im->ctrlSock, (const char*)pkt.data(), (int)pkt.size(), 0,
                   (sockaddr*)&im->ctrlTo, sizeof(im->ctrlTo));
    if (n != (int)pkt.size()) {
        LOGW("audio: sync send failed (%d)", WSAGetLastError());
        return false;
    }
    return true;
}

} // namespace fc
