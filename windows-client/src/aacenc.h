// FlowCast Windows client - AAC-LC encoder over Media Foundation.
//
// ct=4 (AAC-LC) / spf=1024 / 44100 Hz stereo is the combination FlowCast's
// audio engine supports natively, so that is what we ask the MFT for.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace fc {

class AacEncoder {
public:
    AacEncoder() = default;
    ~AacEncoder();

    bool start(int sampleRate, int channels, int bitrate = 128000);
    void stop();

    // Feeds interleaved float samples. Encoded frames are appended to `out` as
    // raw AAC access units (no ADTS). Returns the number of frames produced.
    int encode(const float* pcm, size_t frames, std::vector<uint8_t>& out);

    // AudioSpecificConfig for the stream, valid after the first frame.
    bool asc(std::vector<uint8_t>& out) const;

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace fc
