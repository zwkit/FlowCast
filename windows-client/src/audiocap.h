// FlowCast Windows client - system audio capture via WASAPI loopback.
//
// Loopback mode gives us exactly what is playing on the machine, which is what
// "mirror this PC" should send. Frames come out as 32-bit float at the mix
// format's rate, which we resample to 44100 Hz stereo for AAC-LC.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace fc {

class AudioCapture {
public:
    AudioCapture() = default;
    ~AudioCapture();

    // Opens the default render endpoint in loopback mode.
    bool start(int sampleRate = 44100, int channels = 2);
    void stop();

    // Pulls one packet. Returns false when nothing was captured. `out` is
    // interleaved float samples in [-1, 1] at the requested rate/channels.
    bool readPacket(std::vector<float>& out, uint32_t& frames);

    int sampleRate() const { return m_rate; }
    int channels() const { return m_channels; }

private:
    struct Impl;
    Impl* m_impl = nullptr;
    int m_rate = 44100;
    int m_channels = 2;
};

} // namespace fc
