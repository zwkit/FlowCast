// FlowCast Windows client - Windows Graphics Capture backend.
//
// Desktop Duplication reads frames out of the display output, which is why it
// is fast and why it can be refused outright: a laptop whose screen is driven
// by a different adapter than the one D3D picked, an indirect display (a USB-C
// dock, a DisplayLink adapter), a machine where another application already
// holds the output's single duplication object, or an HDR desktop whose frames
// are FP16 instead of BGRA. Windows Graphics Capture instead asks DWM for the
// composited desktop, the same way the Snipping Tool and Teams do, so it keeps
// working in all of those cases. Its cost on Windows 10 is the capture border
// the OS draws around the screen and a copy through DWM, so it stays a
// fallback rather than the default.
#pragma once
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

namespace fc {

class WgcCapture {
public:
    WgcCapture() = default;
    ~WgcCapture();
    WgcCapture(const WgcCapture&) = delete;
    WgcCapture& operator=(const WgcCapture&) = delete;

    // Is the API present and usable on this machine (Windows 10 1809+ and a
    // runtime that answers GraphicsCaptureSession::IsSupported)?
    static bool supported();

    // Captures one monitor. `device` is a display name from list_displays();
    // empty (or a screen that is gone) means the primary one. On failure `hr`
    // carries the reason.
    bool start(const std::wstring& device, HRESULT* hr);
    void stop();

    // Frames live on this device, so the caller's NV12 converter has to be
    // built against it: a second device would turn every frame into a
    // cross-device copy.
    ID3D11Device* device() const;
    ID3D11DeviceContext* context() const;

    // The newest frame, or nullptr. *hr is S_OK for "nothing new" (a static
    // screen) and a failure code when capture itself is broken. The texture is
    // borrowed and stays valid until release().
    ID3D11Texture2D* acquire(HRESULT* hr);
    void release();

    int width() const;
    int height() const;

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace fc
