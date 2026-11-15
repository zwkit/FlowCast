// FlowCast Windows client - BGRA -> NV12 conversion and scaling on the GPU.
//
// The scalar loop this replaces costs ~5 ms per frame at 1080p and ~19 ms at
// 4K on a modern Ryzen, which by itself rules out a 4K stream at 30 fps. The
// D3D11 video processor does the same job plus the downscale in well under a
// millisecond, and only 1.5 bytes per pixel have to come back to the CPU
// instead of 4 -- at 4K that is 12 MB per frame rather than 33 MB.
#pragma once
#include <cstddef>
#include <cstdint>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

namespace fc {

class Nv12Converter {
public:
    Nv12Converter() = default;
    ~Nv12Converter();
    Nv12Converter(const Nv12Converter&) = delete;
    Nv12Converter& operator=(const Nv12Converter&) = delete;

    // `device`/`context` must be the ones that own the source texture: DXGI
    // hands desktop frames out on the device that created the duplication, and
    // a second device would turn every frame into a cross-device copy.
    // `dstWidth`/`dstHeight` are the encoder's frame size; the video processor
    // scales when they differ from the source size.
    bool init(ID3D11Device* device, ID3D11DeviceContext* context, size_t srcWidth,
              size_t srcHeight, size_t dstWidth, size_t dstHeight);
    void reset();

    // GPU -> GPU, into the internal NV12 texture.
    bool convert(ID3D11Texture2D* bgra);

    // Copies the last converted picture into `dst` as tightly packed Y plane
    // followed by the interleaved UV plane (width * height * 3 / 2 bytes).
    bool readback(uint8_t* dst);

    bool ready() const { return m_impl != nullptr; }
    size_t width() const { return m_dstW; }
    size_t height() const { return m_dstH; }
    size_t bufferSize() const { return m_dstW * m_dstH * 3 / 2; }

    // Why init() failed, for the log line.
    const char* error() const { return m_error; }

private:
    struct Impl;
    Impl* m_impl = nullptr;
    size_t m_srcW = 0, m_srcH = 0;
    size_t m_dstW = 0, m_dstH = 0;
    const char* m_error = "";
};

} // namespace fc
