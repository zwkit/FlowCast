// FlowCast Windows client - H.264 encoder over Media Foundation.
//
// Uses the hardware H.264 encoder MFT when one is installed (every Windows 10
// box has one for Intel/AMD/NV), falling back to the software transform.
// The output is raw Annex-B NAL units with no start codes, which is exactly
// what the mirror stream wants.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace fc {

class H264Encoder {
public:
    H264Encoder() = default;
    ~H264Encoder();

    // `fps` drives the average bitrate/frame-rate key we advertise; `bitrate`
    // is in bits per second. `maxQp` caps the quantiser (H.264 scale, 0-51);
    // 0 leaves the encoder's own ceiling in place. A cap is what keeps static
    // desktop text legible: without it the rate controller happily spends
    // spare bits elsewhere and lets text edges quantise away.
    // `hevc` selects H.265 instead of H.264. Screen content gains the most from
    // it: SAO and 32x32+ transforms keep small text from turning into 8x8 blocks,
    // which is exactly the artefact H.264 leaves on a mostly-white desktop.
    bool start(size_t width, size_t height, int fps, int bitrate, bool lowLatency = true,
               int maxQp = 0, bool hevc = false);
    void stop();

    // Feeds one BGRA frame. Encoded NAL units are appended to `out` as
    // [4-byte big-endian length][NAL bytes] pairs, which is the mirror payload
    // layout. Returns the number of NAL units produced.
    int encodeFrame(const uint8_t* bgra, size_t width, size_t height, size_t stride,
                    uint64_t timestampNs, std::vector<uint8_t>& out);

    // Same, for a frame the GPU already converted: tightly packed NV12, Y plane
    // followed by the interleaved UV plane. Skips the scalar conversion, which
    // is the difference between 30 fps and a slideshow at 4K.
    int encodeNv12(const uint8_t* nv12, size_t width, size_t height, uint64_t timestampNs,
                   std::vector<uint8_t>& out);

    // SPS/PPS as raw NAL bytes (no start codes), filled after the first frame.
    bool parameterSets(std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) const;

    // Same for HEVC, which carries a VPS in front of SPS/PPS. The receiver only
    // learns the codec from these, so they must go out before the first frame.
    bool hevcParameterSets(std::vector<uint8_t>& vps, std::vector<uint8_t>& sps,
                           std::vector<uint8_t>& pps) const;

    bool isHevc() const { return m_hevc; }

    // Why the last start() failed, as "candidate:stage/hresult" entries. Empty
    // after a successful start. "Encoder would not start" covers a resolution
    // no MFT accepts, a driver that is not installed, and an MFT cache that is
    // filtered -- this is what tells those apart.
    const std::string& startDetail() const { return m_detail; }

    // Which encoder the last successful start() ended up using, e.g.
    // "software: H264 Encoder MFT". Empty before start().
    const std::string& backend() const { return m_backend; }

    size_t width() const { return m_width; }
    size_t height() const { return m_height; }

private:
    struct Impl;
    Impl* m_impl = nullptr;
    std::string m_detail;
    std::string m_backend;
    size_t m_width = 0;
    size_t m_height = 0;
    bool m_hevc = false;
};

// Diagnostics for --probe-encoders: prints the encoder MFTs installed for
// H.264 and H.265, split by the pass start() would use.
void list_video_encoders();

} // namespace fc
