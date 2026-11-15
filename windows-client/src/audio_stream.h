// FlowCast Windows client - RAOP audio stream (RTP data + timing sync).
//
// Packet shapes verified against raop_rtp.c:
//   data:   0x80 0x60 <seq BE16> <rtp ts BE32> 0x00000000 <payload>
//   sync:   0x90 0xd4 0x00 0x04 <sync_rtp BE32> <client ntp BE64> <next_rtp BE32>
// The payload is AES-128-CBC encrypted (IV reset per packet) over the 16-byte
// aligned prefix only; any tail stays plaintext, which is exactly what
// raop_buffer.c expects.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace fc {

class AudioStream {
public:
    AudioStream() = default;
    ~AudioStream();

    // `aeskey` and `aesiv` come straight from the FairPlay exchange in the
    // SETUP handshake.
    void setKeys(const uint8_t aeskey[16], const uint8_t aesiv[16]);

    // Binds the two UDP sockets and reports our local control port, which must
    // be advertised in the SETUP stream description.
    bool openSockets(uint16_t& localControlPort);

    // Points the sockets at the receiver once SETUP has told us its ports.
    bool setRemote(const char* host, uint16_t dataPort, uint16_t controlPort);

    void close();
    bool connected() const { return m_connected; }

    // Sends one AAC access unit as an encrypted RTP packet. `rtpTimestamp` is
    // in 1/44100 units and must advance by 1024 per frame.
    bool sendAudio(const uint8_t* aac, size_t len, uint32_t rtpTimestamp);

    // Sends a timing sync packet. `clientNtp` is our clock in NTP form with the
    // 1900 epoch offset added (the iOS convention).
    bool sendSync(uint32_t syncRtp, uint64_t clientNtp, uint32_t nextRtp);

    uint16_t localDataPort() const { return m_localDataPort; }
    uint16_t localControlPort() const { return m_localControlPort; }

    uint32_t packetsSent() const { return m_packets; }
    uint64_t bytesSent() const { return m_bytes; }

private:
    struct Impl;
    Impl* m_impl = nullptr;
    bool m_connected = false;
    uint16_t m_localDataPort = 0;
    uint16_t m_localControlPort = 0;
    uint16_t m_seq = 0;
    uint32_t m_packets = 0;
    uint64_t m_bytes = 0;
};

} // namespace fc
