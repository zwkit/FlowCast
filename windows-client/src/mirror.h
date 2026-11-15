// FlowCast Windows client - AirPlay mirror video stream.
//
// Packet layout (verified against raop_rtp_mirror.c):
//   128-byte header, little-endian length + NTP-format timestamp
//     [0:4]   payload size            (LE u32)
//     [4]     0x00 = VCL, 0x01 = parameter set
//     [5]     0x00 = non-IDR, 0x10 = IDR
//     [6:8]   payload option          (0x16 0x01 for parameter sets)
//     [8:16]  NTP timestamp           (LE u64, seconds since boot + fraction)
//     [16:20] width, [20:24] height   (LE f32)
//     [40:48] source width/height     (LE f32)
//     [56:64] display width/height    (LE f32)
//   payload
//     VCL:    [4-byte big-endian NAL length][NAL] ...  (AES-128-CTR encrypted)
//     params: [6-byte header][u16 BE sps_size][SPS][n_pps][u16 BE pps_size][PPS]
//             (never encrypted)
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace fc {

class MirrorStream {
public:
    MirrorStream() = default;
    ~MirrorStream();

    // Derives the video key/IV exactly like mirror_buffer_init_aes():
    //   key = sha512("AirPlayStreamKey<connID>" || aeskeyAudio)[0:16]
    //   iv  = sha512("AirPlayStreamIV<connID>"  || aeskeyAudio)[0:16]
    //
    // The derived state lives on the object, not in the connection, so
    // connect() never wipes it. A reconnect re-inits the keystream from the
    // same IV, which matches the receiver re-initialising AES on its side.
    void setKeys(uint64_t streamConnectionID, const uint8_t aeskeyAudio[16]);

    // Connects the mirror TCP socket to the receiver's dataPort.
    bool connect(const char* host, uint16_t dataPort);
    void close();
    bool connected() const { return m_connected; }

    // Sends the unencrypted parameter-set packet (SPS + PPS).
    bool sendParameterSets(const uint8_t* sps, size_t spsLen, const uint8_t* pps, size_t ppsLen,
                           uint64_t timestampNs);

    // HEVC variant: VPS + SPS + PPS, in the layout the receiver's hvc1 branch
    // reads. This packet is also what tells the receiver the stream is H.265 at
    // all, so it must precede the first frame.
    bool sendHevcParameterSets(const uint8_t* vps, size_t vpsLen, const uint8_t* sps, size_t spsLen,
                               const uint8_t* pps, size_t ppsLen, uint64_t timestampNs);

    // Sends one encoded frame. `nals` is the [4-byte BE length][NAL]... buffer
    // produced by H264Encoder. `idr` selects the header flags.
    bool sendFrame(const std::vector<uint8_t>& nals, bool idr, uint64_t timestampNs);

    // Statistics
    uint64_t packetsSent() const { return m_packets; }
    uint64_t bytesSent() const { return m_bytes; }

    // Encoded frame size, for the 128-byte header. Set by the capture loop.
    void setSize(uint16_t width, uint16_t height) { m_width = width; m_height = height; }

private:
    bool sendPacket(uint8_t type4, uint8_t type5, uint8_t opt6, uint8_t opt7,
                    const uint8_t* payload, size_t payloadLen, uint64_t timestampNs,
                    bool encrypt);

    // Re-derives the keystream from the stored inputs. Returns false if
    // setKeys() has not been called yet.
    bool deriveKeys();

    struct Impl;
    Impl* m_impl = nullptr;
    bool m_connected = false;
    uint64_t m_packets = 0;
    uint64_t m_bytes = 0;
    uint16_t m_width = 0;
    uint16_t m_height = 0;

    uint64_t m_connId = 0;
    uint8_t m_aesKey[16] = {0};
    bool m_haveKeys = false;
};

} // namespace fc
