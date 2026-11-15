// FlowCast Windows client - mirror video stream implementation.
#include "mirror.h"
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

struct MirrorStream::Impl {
    SOCKET sock = INVALID_SOCKET;
    bool wsaUp = false;
    fc::AesCtr ctr;
    bool keysReady = false;
};

MirrorStream::~MirrorStream() {
    close();
}

bool MirrorStream::deriveKeys() {
    if (!m_haveKeys) return false;
    if (!m_impl) m_impl = new Impl();
    Impl* im = m_impl;

    // SHA-512, matching the receiver's mirror_buffer_init_aes(). The digest is
    // truncated to its first 16 bytes for both the AES key and the CTR IV.
    uint8_t digest[64];
    uint8_t key[16];
    uint8_t iv[16];

    auto derive = [&](const char* label, uint8_t out[16]) {
        Buf in;
        char tag[64];
        snprintf(tag, sizeof(tag), "%s%llu", label, (unsigned long long)m_connId);
        in.append(tag, strlen(tag));
        in.append(m_aesKey, 16);
        fc::sha512(in.data(), in.size(), digest);
        memcpy(out, digest, 16);
    };
    derive("AirPlayStreamKey", key);
    derive("AirPlayStreamIV", iv);

    im->ctr.init(key, iv);
    im->keysReady = true;
    return true;
}

void MirrorStream::setKeys(uint64_t streamConnectionID, const uint8_t aeskeyAudio[16]) {
    m_connId = streamConnectionID;
    memcpy(m_aesKey, aeskeyAudio, 16);
    m_haveKeys = true;
    deriveKeys();
}

bool MirrorStream::connect(const char* host, uint16_t dataPort) {
    // close() drops the Impl, which used to take the derived keystream with it
    // and made every frame fail with "encrypting without derived keys". The key
    // inputs now live on the object, so re-derive right after the swap.
    close();
    if (!deriveKeys()) {
        LOGE("mirror: connect() before setKeys()");
        return false;
    }
    Impl* im = m_impl;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    im->wsaUp = true;

    sockaddr_in sin{};
    if (!net_resolve_ipv4(host, dataPort, &sin, 4000)) return false;

    im->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (im->sock == INVALID_SOCKET) {
        LOGE("mirror: socket() failed (%d)", WSAGetLastError());
        return false;
    }
    // Nagle off: mirroring is a steady small-packet stream where latency
    // matters far more than bandwidth efficiency.
    BOOL nodelay = TRUE;
    setsockopt(im->sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
    DWORD rcvto = 5000, sndto = 5000;
    setsockopt(im->sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcvto, sizeof(rcvto));
    setsockopt(im->sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndto, sizeof(sndto));

    if (::connect(im->sock, (sockaddr*)&sin, sizeof(sin)) != 0) {
        LOGE("mirror: connect to %s:%u failed (%d)", host, (unsigned)dataPort, WSAGetLastError());
        return false;
    }
    m_connected = true;
    LOGI("mirror: connected to %s:%u", host, (unsigned)dataPort);
    return true;
}

void MirrorStream::close() {
    if (!m_impl) return;
    if (m_impl->sock != INVALID_SOCKET) closesocket(m_impl->sock);
    if (m_impl->wsaUp) WSACleanup();
    delete m_impl;
    m_impl = nullptr;
    m_connected = false;
}

bool MirrorStream::sendPacket(uint8_t type4, uint8_t type5, uint8_t opt6, uint8_t opt7,
                              const uint8_t* payload, size_t payloadLen, uint64_t timestampNs,
                              bool encrypt) {
    if (!m_impl || !m_connected) return false;
    Impl* im = m_impl;

    Buf pkt;
    pkt.resize(128);
    pkt.putU32LE(0, (uint32_t)payloadLen);
    pkt.data()[4] = type4;
    pkt.data()[5] = type5;
    pkt.data()[6] = opt6;
    pkt.data()[7] = opt7;
    // NTP-format timestamp: seconds since boot in the high 32 bits. No 1900
    // epoch offset here, unlike the timing and audio sync packets.
    pkt.putU64LE(8, ns_to_ntp(timestampNs, false));

    const float w = (float)m_width;
    const float h = (float)m_height;
    pkt.putF32LE(16, w);
    pkt.putF32LE(20, h);
    pkt.putF32LE(40, w);
    pkt.putF32LE(44, h);
    pkt.putF32LE(56, w);
    pkt.putF32LE(60, h);

    if (payloadLen > 0) {
        pkt.append(payload, payloadLen);
        if (encrypt) {
            if (!im->keysReady) {
                LOGE("mirror: encrypting without derived keys");
                return false;
            }
            im->ctr.process(pkt.data() + 128, pkt.data() + 128, payloadLen);
        }
    }

    size_t off = 0;
    const uint8_t* p = pkt.data();
    const size_t total = pkt.size();
    while (off < total) {
        int n = ::send(im->sock, (const char*)p + off, (int)(total - off), 0);
        if (n <= 0) {
            LOGE("mirror: send failed (%d)", WSAGetLastError());
            m_connected = false;
            return false;
        }
        off += (size_t)n;
    }
    m_packets++;
    m_bytes += total;
    return true;
}

bool MirrorStream::sendParameterSets(const uint8_t* sps, size_t spsLen, const uint8_t* pps,
                                     size_t ppsLen, uint64_t timestampNs) {
    if (!sps || !pps || spsLen == 0 || ppsLen == 0 || spsLen > 0xffff || ppsLen > 0xffff) {
        LOGE("mirror: invalid parameter sets (%zu/%zu)", spsLen, ppsLen);
        return false;
    }
    // Payload layout the receiver's h264 branch expects:
    //   [0:6]   header (ignored; [4:8] is probed for "hvc1" and must not match)
    //   [6:8]   sps_size, big-endian
    //   [8:..]  SPS
    //   [..]    1 byte: number of PPS
    //   [..:+2] pps_size, big-endian
    //   [..]    PPS
    Buf payload;
    payload.appendZeros(6);
    payload.appendU16BE((uint16_t)spsLen);
    payload.append(sps, spsLen);
    payload.appendByte(0x01); // number of PPS
    payload.appendU16BE((uint16_t)ppsLen);
    payload.append(pps, ppsLen);

    LOGD("mirror: parameter sets sps=%zu pps=%zu, payload=%zu bytes", spsLen, ppsLen,
         payload.size());

    // 0x16 0x01 is the option pair real senders use for h264 parameter sets.
    return sendPacket(0x01, 0x00, 0x16, 0x01, payload.data(), payload.size(), timestampNs, false);
}

bool MirrorStream::sendHevcParameterSets(const uint8_t* vps, size_t vpsLen, const uint8_t* sps,
                                         size_t spsLen, const uint8_t* pps, size_t ppsLen,
                                         uint64_t timestampNs) {
    if (!vps || !sps || !pps || vpsLen == 0 || spsLen == 0 || ppsLen == 0 || vpsLen > 0xff ||
        spsLen > 0xff || ppsLen > 0xff) {
        // Each length is stored in one meaningful byte: the prefix bytes occupy
        // the high half of the 16-bit field the receiver reads.
        LOGE("mirror: invalid HEVC parameter sets (%zu/%zu/%zu)", vpsLen, spsLen, ppsLen);
        return false;
    }
    // The receiver's hvc1 branch is written against the layout a real sender
    // produces: it probes [4..8) for "hvc1" purely to learn the codec, then
    // starts parsing at the fixed offset 0x75. Everything between is ignored, so
    // it is padded rather than guessed at. Each unit is prefixed with a 3-byte
    // tag plus a 16-bit length whose high byte is always 0 at these sizes.
    Buf payload;
    payload.appendZeros(4);
    payload.append("hvc1", 4);
    payload.appendZeros(0x75 - 8);
    auto put = [&](uint8_t tag, const uint8_t* d, size_t n) {
        const uint8_t pre[3] = {tag, 0x00, 0x01};
        payload.append(pre, sizeof(pre));
        payload.appendU16BE((uint16_t)n);
        payload.append(d, n);
    };
    put(0xa0, vps, vpsLen);
    put(0xa1, sps, spsLen);
    put(0xa2, pps, ppsLen);

    LOGD("mirror: hevc parameter sets vps=%zu sps=%zu pps=%zu, payload=%zu bytes", vpsLen, spsLen,
         ppsLen, payload.size());
    return sendPacket(0x01, 0x00, 0x16, 0x01, payload.data(), payload.size(), timestampNs, false);
}

bool MirrorStream::sendFrame(const std::vector<uint8_t>& nals, bool idr, uint64_t timestampNs) {
    if (nals.empty()) return false;
    // The receiver prepends the stored SPS/PPS to the next encrypted packet
    // only when the timestamps match, so the parameter-set packet must carry
    // the same timestamp as the IDR that follows it.
    return sendPacket(0x00, idr ? 0x10 : 0x00, 0x00, 0x00, nals.data(), nals.size(), timestampNs,
                      true);
}

} // namespace fc
