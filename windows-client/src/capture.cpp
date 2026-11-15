// FlowCast Windows client - DXGI Desktop Duplication capture implementation.
#include "capture.h"
#include "log.h"
#include "nv12conv.h"
#include "timing.h"
#include "wgc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

namespace fc {

struct ScreenCapture::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> dup;
    ComPtr<ID3D11Texture2D> staging;
    D3D11_TEXTURE2D_DESC stagingDesc{};
    // CPU-side copy: DXGI gives us a mapped pointer that only stays valid
    // until Unmap, and CopyResource refuses a mapped destination, so the
    // frame is copied row by row into a tightly packed BGRA buffer.
    // Only allocated when the GPU converter is unavailable.
    std::vector<uint8_t> cpu;
    // Only allocated when --max-width actually shrinks the desktop.
    std::vector<uint8_t> scaled;
    // Converted picture when the GPU path is active (tightly packed NV12).
    std::vector<uint8_t> nv12;

    // GDI fallback, used when the desktop cannot be duplicated at all.
    bool gdi = false;
    HDC screenDc = nullptr;
    HDC memDc = nullptr;
    HBITMAP bitmap = nullptr;
    void* bits = nullptr;
};

ScreenCapture::~ScreenCapture() {
    stop();
}

namespace {

// EnumDisplayMonitors is what the Windows display settings page uses, and it
// hands back the same "\\.\DISPLAYn" name DXGI puts in DXGI_OUTPUT_DESC. Using
// it for both the list and the capture choice keeps the two in step even when
// the direct3D device was created on a different adapter.
BOOL CALLBACK monitor_proc(HMONITOR mon, HDC, LPRECT, LPARAM data) {
    auto* out = reinterpret_cast<std::vector<DisplayInfo>*>(data);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(mon, &mi)) return TRUE;
    DisplayInfo d;
    d.device = mi.szDevice;
    d.left = mi.rcMonitor.left;
    d.top = mi.rcMonitor.top;
    d.width = mi.rcMonitor.right - mi.rcMonitor.left;
    d.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
    d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    if (d.width <= 0 || d.height <= 0) return TRUE;
    out->push_back(d);
    return TRUE;
}

bool same_name(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

} // namespace

std::vector<DisplayInfo> list_displays() {
    std::vector<DisplayInfo> out;
    ::EnumDisplayMonitors(nullptr, nullptr, monitor_proc, (LPARAM)&out);
    // Primary first, then left to right: the list has to be stable enough for a
    // saved choice and a dropdown position to mean the same thing twice.
    std::sort(out.begin(), out.end(), [](const DisplayInfo& a, const DisplayInfo& b) {
        if (a.primary != b.primary) return a.primary;
        if (a.left != b.left) return a.left < b.left;
        return a.top < b.top;
    });
    for (size_t i = 0; i < out.size(); i++) out[i].index = (int)i + 1;
    return out;
}

DisplayInfo find_display(const std::wstring& device) {
    const std::vector<DisplayInfo> all = list_displays();
    if (!device.empty()) {
        for (const DisplayInfo& d : all) {
            if (same_name(d.device, device)) return d;
        }
    }
    for (const DisplayInfo& d : all) {
        if (d.primary) return d;
    }
    if (!all.empty()) return all.front();
    return DisplayInfo{};
}

namespace {

// Area-average (box) downscale of a tightly packed BGRA image. Nearest-neighbour
// turns desktop text into aliased mush at 2:1, and a separable resampler is
// overkill for a screen that is usually being halved.
void downscaleBgra(const uint8_t* src, size_t sw, size_t sh, size_t srcStride, uint8_t* dst,
                   size_t dw, size_t dh, size_t dstStride) {
    for (size_t dy = 0; dy < dh; dy++) {
        const size_t sy0 = dy * sh / dh;
        size_t sy1 = (dy + 1) * sh / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        uint8_t* drow = dst + dy * dstStride;
        for (size_t dx = 0; dx < dw; dx++) {
            const size_t sx0 = dx * sw / dw;
            size_t sx1 = (dx + 1) * sw / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint32_t acc0 = 0, acc1 = 0, acc2 = 0, n = 0;
            for (size_t sy = sy0; sy < sy1; sy++) {
                const uint8_t* srow = src + sy * srcStride + sx0 * 4;
                for (size_t sx = sx0; sx < sx1; sx++, srow += 4) {
                    acc0 += srow[0];
                    acc1 += srow[1];
                    acc2 += srow[2];
                    n++;
                }
            }
            drow[dx * 4 + 0] = (uint8_t)(acc0 / n);
            drow[dx * 4 + 1] = (uint8_t)(acc1 / n);
            drow[dx * 4 + 2] = (uint8_t)(acc2 / n);
            drow[dx * 4 + 3] = 255;
        }
    }
}

} // namespace

void ScreenCapture::applyScaleLimit(size_t maxWidth) {
    m_width = m_srcWidth;
    m_height = m_srcHeight;
    if (maxWidth == 0 || m_srcWidth == 0 || m_srcHeight == 0 || m_width <= maxWidth) return;
    // Even dimensions only: NV12 chroma is subsampled 2x2 and the H.264
    // encoders reject an odd frame size outright.
    const size_t w = maxWidth & ~(size_t)1;
    const size_t h = (m_srcHeight * w / m_srcWidth) & ~(size_t)1;
    if (w < 64 || h < 64) return; // nonsense cap, keep the native size
    m_width = w;
    m_height = h;
    LOGI("capture: downscaling %zux%zu -> %zux%zu", m_srcWidth, m_srcHeight, m_width, m_height);
}

// Builds the GPU BGRA -> NV12 converter for the current geometry. The CPU path
// stays available: a handful of drivers (and every WARP device) cannot do video
// processing at all, and encoding must still work there.
bool ScreenCapture::startConverter() {
    if (!m_impl || m_impl->gdi || !m_impl->device || !m_impl->context) return false;
    Nv12Converter* c = new Nv12Converter();
    if (!c->init(m_impl->device.Get(), m_impl->context.Get(), m_srcWidth, m_srcHeight, m_width,
                 m_height)) {
        LOGW("capture: GPU BGRA->NV12 unavailable (%s), converting on the CPU", c->error());
        delete c;
        return false;
    }
    m_impl->nv12.assign(c->bufferSize(), 0);
    m_conv = c;
    LOGI("capture: GPU conversion %zux%zu -> %zux%zu NV12", m_srcWidth, m_srcHeight, m_width,
         m_height);
    return true;
}

// One attempt at bringing up device + duplication + staging texture.
// Returns false with the failing HRESULT in `hr`.
bool ScreenCapture::tryStart(Impl* im, const std::wstring& device, size_t* width, size_t* height,
                             HRESULT* hr) {
    ComPtr<IDXGIFactory1> factory;
    *hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(*hr)) return false;

    // Pick the output by device name, never by enumeration index: on a laptop
    // with a TV on HDMI, output 0 is whichever screen the driver brought up
    // first, and capturing the wrong desktop is indistinguishable from
    // "mirroring is broken". The adapter is resolved together with the output
    // because DuplicateOutput only works on the adapter that owns the screen.
    ComPtr<IDXGIOutput> output;
    ComPtr<IDXGIAdapter1> owner;
    ComPtr<IDXGIOutput> firstAttached, primary;
    ComPtr<IDXGIAdapter1> firstOwner, primaryOwner;
    for (UINT a = 0;; a++) {
        ComPtr<IDXGIAdapter1> ad;
        if (factory->EnumAdapters1(a, &ad) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT o = 0;; o++) {
            ComPtr<IDXGIOutput> cand;
            if (ad->EnumOutputs(o, &cand) == DXGI_ERROR_NOT_FOUND) break;
            if (!cand) continue;
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(cand->GetDesc(&desc)) || !desc.AttachedToDesktop) continue;
            if (!firstAttached) {
                firstAttached = cand;
                firstOwner = ad;
            }
            // The screen that owns the desktop origin is the primary one.
            const RECT& r = desc.DesktopCoordinates;
            if (!primary && r.left <= 0 && r.right > 0 && r.top <= 0 && r.bottom > 0) {
                primary = cand;
                primaryOwner = ad;
            }
            if (!device.empty() && same_name(desc.DeviceName, device)) {
                output = cand;
                owner = ad;
                break;
            }
        }
        if (output) break;
    }
    if (!output) {
        if (primary) {
            output = primary;
            owner = primaryOwner;
            if (!device.empty()) {
                LOGW("capture: %ls is not attached, capturing the primary screen instead",
                     device.c_str());
            }
        } else if (firstAttached) {
            output = firstAttached;
            owner = firstOwner;
        }
    }
    if (!output || !owner) { *hr = E_NOINTERFACE; return false; }

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                  D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    // D3D_DRIVER_TYPE_UNKNOWN is mandatory as soon as an adapter is passed in.
    *hr = D3D11CreateDevice(owner.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels,
                            ARRAYSIZE(levels), D3D11_SDK_VERSION, &im->device, &got,
                            &im->context);
    if (FAILED(*hr)) {
        // A few drivers refuse an explicit adapter. The default-device path is
        // what earlier builds used, so keep it as a safety net rather than
        // losing hardware capture (and 4K with it) to a driver quirk.
        LOGW("capture: device on the adapter failed (0x%08lx), using the default one",
             (unsigned long)*hr);
        *hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                                ARRAYSIZE(levels), D3D11_SDK_VERSION, &im->device, &got,
                                &im->context);
        if (FAILED(*hr)) return false;
    }

    DXGI_ADAPTER_DESC adDesc{};
    if (SUCCEEDED(owner->GetDesc(&adDesc))) {
        LOGI("capture: adapter %ls", adDesc.Description);
    }

    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output.As(&output1))) { *hr = E_NOINTERFACE; return false; }

    *hr = output1->DuplicateOutput(im->device.Get(), &im->dup);
    if (FAILED(*hr)) return false;

    DXGI_OUTPUT_DESC outDesc{};
    output->GetDesc(&outDesc);
    const size_t deskW = (size_t)(outDesc.DesktopCoordinates.right - outDesc.DesktopCoordinates.left);
    const size_t deskH = (size_t)(outDesc.DesktopCoordinates.bottom - outDesc.DesktopCoordinates.top);
    LOGI("capture: duplicating %ls at (%ld,%ld) %zux%zu", outDesc.DeviceName,
         outDesc.DesktopCoordinates.left, outDesc.DesktopCoordinates.top, deskW, deskH);

    // Two different numbers, and only one of them is the frame we actually get:
    // DesktopCoordinates is what Windows thinks the desktop is, ModeDesc is the
    // surface the duplicator is bound to (and the size the staging texture has
    // to match). Under Virtual Super Resolution they disagree - the desktop
    // composites at 4K while the output stays at the panel's 1080p - and when
    // they disagree the 4K is downscaled by Windows before it ever reaches us.
    // Log both so "is the TV really getting 4K?" is one line of output instead
    // of a visual judgement call.
    DXGI_OUTDUPL_DESC dupDesc{};
    im->dup->GetDesc(&dupDesc);
    LOGI("capture: duplication surface %ux%u (desktop %zux%zu)", dupDesc.ModeDesc.Width,
         dupDesc.ModeDesc.Height, deskW, deskH);
    if (dupDesc.ModeDesc.Width != deskW || dupDesc.ModeDesc.Height != deskH) {
        // Seen with Virtual Super Resolution on a 1080p panel: the driver keeps
        // the output at 4K while Windows composites a 1080p desktop, so
        // capturing the surface would mean sending an upscaled picture at four
        // times the bitrate for no extra detail. The desktop is the honest size.
        LOGW("capture: output mode is %ux%u but the desktop is %zux%zu - capturing the desktop, "
             "the extra output pixels carry no detail",
             dupDesc.ModeDesc.Width, dupDesc.ModeDesc.Height, deskW, deskH);
    }
    *width = deskW;
    *height = deskH;

    // HDR desktops hand out FP16 frames. Everything downstream -- the staging
    // texture, the NV12 converter, the CPU fallback -- is BGRA8, so rather than
    // grow a second format path, hand the machine to the DWM capture below,
    // which tone-maps to SDR on the way out.
    if (dupDesc.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        LOGW("capture: duplication surface is format %u, not BGRA8 - using DWM capture",
             (unsigned)dupDesc.ModeDesc.Format);
        *hr = (HRESULT)DXGI_ERROR_UNSUPPORTED;
        return false;
    }

    im->stagingDesc.Width = (UINT)*width;
    im->stagingDesc.Height = (UINT)*height;
    im->stagingDesc.MipLevels = 1;
    im->stagingDesc.ArraySize = 1;
    im->stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    im->stagingDesc.SampleDesc.Count = 1;
    im->stagingDesc.Usage = D3D11_USAGE_STAGING;
    im->stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    im->stagingDesc.BindFlags = 0;
    *hr = im->device->CreateTexture2D(&im->stagingDesc, nullptr, &im->staging);
    if (FAILED(*hr)) return false;
    return true;
}

// Last-resort capture path. Desktop Duplication is unavailable in a locked
// session, over some RDP configurations, and while another duplicator holds
// the desktop, but plain BitBlt still works in all of those. Slower and it
// cannot skip idle frames, but mirroring beats "no video at all".
bool ScreenCapture::startGdiFallback(size_t maxWidth) {
    Impl* im = new Impl();
    im->gdi = true;

    // Only the screen that was asked for, not the whole virtual desktop: a
    // BitBlt over two screens at different resolutions produces a frame with
    // the wrong aspect and a seam down the middle.
    const DisplayInfo target = find_display(m_device);
    int x = target.width > 0 ? target.left : GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = target.width > 0 ? target.top : GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = target.width > 0 ? target.width : GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = target.width > 0 ? target.height : GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0) {
        LOGW("capture: GDI fallback found no screen (%dx%d)", w, h);
        delete im;
        return false;
    }

    im->screenDc = GetDC(nullptr);
    if (!im->screenDc) {
        LOGW("capture: GetDC failed (%lu)", (unsigned long)GetLastError());
        delete im;
        return false;
    }
    im->memDc = CreateCompatibleDC(im->screenDc);
    if (!im->memDc) {
        LOGW("capture: CreateCompatibleDC failed (%lu)", (unsigned long)GetLastError());
        ReleaseDC(nullptr, im->screenDc);
        delete im;
        return false;
    }

    // Negative height makes the DIB top-down, matching the DXGI path's row
    // order, so downstream code sees the same layout either way.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    im->bitmap = CreateDIBSection(im->memDc, &bi, DIB_RGB_COLORS, &im->bits, nullptr, 0);
    if (!im->bitmap || !im->bits) {
        LOGW("capture: CreateDIBSection failed (%lu)", (unsigned long)GetLastError());
        if (im->bitmap) DeleteObject(im->bitmap);
        DeleteDC(im->memDc);
        ReleaseDC(nullptr, im->screenDc);
        delete im;
        return false;
    }
    SelectObject(im->memDc, im->bitmap);

    m_impl = im;
    m_gdiOriginX = x;
    m_gdiOriginY = y;
    m_srcWidth = (size_t)w;
    m_srcHeight = (size_t)h;
    applyScaleLimit(maxWidth);
    LOGW("capture: desktop duplication unavailable, falling back to GDI (%zux%zu)", m_srcWidth,
         m_srcHeight);
    if (m_width != m_srcWidth) im->scaled.assign(m_width * m_height * 4, 0);
    return true;
}

const char* ScreenCapture::backend_name(CaptureBackend b) {
    switch (b) {
    case CaptureBackend::Duplication: return "dxgi";
    case CaptureBackend::Graphics:    return "wgc";
    case CaptureBackend::Gdi:         return "gdi";
    case CaptureBackend::Synthetic:   return "test-pattern";
    case CaptureBackend::None:
    default:                          return "none";
    }
}

void ScreenCapture::noteFailure(const char* stage, long code) {
    if (!m_startDetail.empty()) m_startDetail += ", ";
    m_startDetail += stage;
    char buf[24];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "=0x%08lx", (unsigned long)code);
    m_startDetail += buf;
}

// DWM-composition path. Desktop Duplication can be refused for reasons that
// have nothing to do with permissions -- a hybrid-GPU laptop, an indirect
// display, another application already duplicating the output, an HDR desktop
// whose frames are FP16 rather than BGRA -- and in every one of those cases the
// composited desktop itself is still perfectly readable.
bool ScreenCapture::startWgc(size_t maxWidth, const std::wstring& device) {
    if (!WgcCapture::supported()) {
        noteFailure("wgc", E_NOTIMPL);
        LOGW("capture: Windows Graphics Capture is not available here");
        return false;
    }
    HRESULT hr = S_OK;
    WgcCapture* w = new WgcCapture();
    if (!w->start(device, &hr)) {
        noteFailure("wgc", (long)hr);
        delete w;
        return false;
    }

    Impl* im = new Impl();
    // The frames live on this device, so the converter has to be built on it
    // too: a second device would copy every frame across the bus.
    im->device = w->device();
    im->context = w->context();
    m_srcWidth = (size_t)w->width();
    m_srcHeight = (size_t)w->height();
    if (m_srcWidth < 2 || m_srcHeight < 2 || !im->device || !im->context) {
        noteFailure("wgc-geometry", E_FAIL);
        w->stop();
        delete w;
        delete im;
        return false;
    }
    applyScaleLimit(maxWidth);

    im->stagingDesc.Width = (UINT)m_srcWidth;
    im->stagingDesc.Height = (UINT)m_srcHeight;
    im->stagingDesc.MipLevels = 1;
    im->stagingDesc.ArraySize = 1;
    im->stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    im->stagingDesc.SampleDesc.Count = 1;
    im->stagingDesc.Usage = D3D11_USAGE_STAGING;
    im->stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    im->stagingDesc.BindFlags = 0;
    hr = im->device->CreateTexture2D(&im->stagingDesc, nullptr, &im->staging);
    if (FAILED(hr)) {
        noteFailure("wgc-staging", (long)hr);
        w->stop();
        delete w;
        delete im;
        return false;
    }

    m_impl = im;
    m_wgc = w;
    m_backend = CaptureBackend::Graphics;
    LOGI("capture: screen %zux%zu -> %zux%zu (DWM composition)", m_srcWidth, m_srcHeight, m_width,
         m_height);
    if (!startConverter()) {
        im->cpu.assign(m_srcWidth * m_srcHeight * 4, 0);
        if (m_width != m_srcWidth) im->scaled.assign(m_width * m_height * 4, 0);
    }
    return true;
}

// One GPU frame either converted into NV12 (1) or staged as BGRA (0). Anything
// else means the frame was lost and the caller must not use it.
int ScreenCapture::stageTexture(ID3D11Texture2D* desktop) {
    Impl* im = m_impl;
    if (!im || !desktop) return -1;

    if (m_conv) {
        if (m_conv->convert(desktop) && m_conv->readback(im->nv12.data())) {
            m_convFailures = 0;
            return 1;
        }
        // A driver reset (TDR) invalidates the processor's textures for good.
        // Losing every later frame silently would look like a frozen picture,
        // so give up on the GPU after a few tries and keep mirroring through
        // the CPU path.
        if (++m_convFailures >= 5) {
            LOGW("capture: GPU conversion keeps failing, falling back to the CPU path");
            delete m_conv;
            m_conv = nullptr;
            m_convFailures = 0;
            im->cpu.assign(m_srcWidth * m_srcHeight * 4, 0);
            if (m_width != m_srcWidth) im->scaled.assign(m_width * m_height * 4, 0);
        }
        return -1;
    }

    if (im->cpu.empty()) im->cpu.assign(m_srcWidth * m_srcHeight * 4, 0);
    im->context->CopyResource(im->staging.Get(), desktop);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(im->context->Map(im->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return -1;
    const size_t rowBytes = m_srcWidth * 4;
    for (size_t y = 0; y < m_srcHeight; y++) {
        memcpy(im->cpu.data() + y * rowBytes,
               reinterpret_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch, rowBytes);
    }
    im->context->Unmap(im->staging.Get(), 0);
    return 0;
}

bool ScreenCapture::start(size_t maxWidth, const std::wstring& device) {
    stop();
    m_maxWidth = maxWidth;
    m_device = device;
    // m_lastRecoverMs is deliberately kept: recover() calls back into start(),
    // and resetting the throttle there would make it rebuild on every frame.
    m_sizeFrozen = false;
    m_startDetail.clear();

    // Every attempt leaves a line here, so "the TV showed nothing" on a machine
    // nobody can look at still comes with an explanation.
    LOGI("capture: desktop %ldx%ld, %d monitor(s), remote session: %s",
         (long)::GetSystemMetrics(SM_CXSCREEN), (long)::GetSystemMetrics(SM_CYSCREEN),
         (int)list_displays().size(), ::GetSystemMetrics(SM_REMOTESESSION) ? "yes" : "no");

    // Support switch: FLOWCAST_CAPTURE=dxgi|wgc|gdi pins one path. A fallback
    // that is never exercised is a fallback that does not work, and the only
    // way to exercise it on a machine whose default path is healthy is to ask
    // for it by name.
    std::wstring forced;
    {
        wchar_t buf[16]{};
        if (::GetEnvironmentVariableW(L"FLOWCAST_CAPTURE", buf, 16) > 0) forced = buf;
    }
    const bool allowDxgi = forced.empty() || _wcsicmp(forced.c_str(), L"dxgi") == 0;
    const bool allowWgc = forced.empty() || _wcsicmp(forced.c_str(), L"wgc") == 0;
    const bool allowGdi = forced.empty() || _wcsicmp(forced.c_str(), L"gdi") == 0;
    if (!forced.empty()) LOGW("capture: backend forced to %ls", forced.c_str());

    // DuplicateOutput returns E_ACCESSDENIED while the desktop is unavailable
    // -- locked session, UAC secure desktop, a display mode switch, or another
    // duplicator still winding down. Only one duplication can exist at a time,
    // so retry rather than failing the whole session on a transient state.
    static const int kAttempts = 6;
    for (int attempt = 0; allowDxgi && attempt < kAttempts; attempt++) {
        Impl* im = new Impl();
        HRESULT hr = S_OK;
        size_t w = 0, h = 0;
        if (tryStart(im, device, &w, &h, &hr)) {
            m_impl = im;
            m_backend = CaptureBackend::Duplication;
            m_srcWidth = w;
            m_srcHeight = h;
            applyScaleLimit(maxWidth);
            LOGI("capture: screen %zux%zu -> %zux%zu", m_srcWidth, m_srcHeight, m_width,
                 m_height);
            if (!startConverter()) {
                im->cpu.assign(m_srcWidth * m_srcHeight * 4, 0);
                if (m_width != m_srcWidth) im->scaled.assign(m_width * m_height * 4, 0);
            }
            return true;
        }
        delete im;
        const bool retryable = (hr == E_ACCESSDENIED) ||
                               (hr == (HRESULT)DXGI_ERROR_MODE_CHANGE_IN_PROGRESS) ||
                               (hr == (HRESULT)DXGI_ERROR_NOT_CURRENTLY_AVAILABLE);
        if (!retryable || attempt == kAttempts - 1) {
            LOGW("capture: desktop duplication failed (0x%08lx)", (unsigned long)hr);
            noteFailure("dxgi", (long)hr);
            break;
        }
        if (attempt == 0) {
            LOGW("capture: desktop not available yet (0x%08lx), retrying...", (unsigned long)hr);
        }
        Sleep(300);
    }

    if (allowWgc && startWgc(maxWidth, device)) return true;
    if (allowGdi && startGdiFallback(maxWidth)) {
        m_backend = CaptureBackend::Gdi;
        return true;
    }
    noteFailure("gdi", (long)::GetLastError());
    LOGE("capture: no capture path works here (%s)", m_startDetail.c_str());
    return false;
}

void ScreenCapture::stop() {
    delete m_conv;
    m_conv = nullptr;
    if (m_wgc) {
        m_wgc->stop();
        delete m_wgc;
        m_wgc = nullptr;
    }
    m_backend = CaptureBackend::None;
    m_testPattern = false;
    m_patternFrame = 0;
    std::vector<uint8_t>().swap(m_pattern);
    if (!m_impl) return;
    Impl* im = m_impl;
    m_impl = nullptr;
    if (im->gdi) {
        if (im->bitmap) DeleteObject(im->bitmap);
        if (im->memDc) DeleteDC(im->memDc);
        if (im->screenDc) ReleaseDC(nullptr, im->screenDc);
    }
    delete im;
    m_width = m_height = 0;
    m_srcWidth = m_srcHeight = 0;
}

// ---------------------------------------------------------------------------
// Synthetic source. A remote-desktop session hands out a desktop that no capture
// API will read (DXGI says ACCESS_DENIED, GDI says error 5, Windows Graphics
// Capture returns black frames), which makes "the TV shows nothing" impossible
// to attribute. Feeding the encoder a known-good picture separates the two
// halves: if the TV shows bars, the encode/send/handshake path is fine and the
// desktop was the problem; if it stays black, the bug is downstream.
// ---------------------------------------------------------------------------
bool ScreenCapture::startTestPattern(size_t width, size_t height) {
    stop();
    if (width < 64) width = 64;
    if (height < 64) height = 64;
    m_width = width & ~(size_t)1; // NV12 needs even dimensions
    m_height = height & ~(size_t)1;
    m_srcWidth = m_width;
    m_srcHeight = m_height;
    m_maxWidth = m_width;
    m_sizeFrozen = false;
    m_convFailures = 0;
    m_lastError = 0;
    m_testPattern = true;
    m_patternFrame = 0;
    m_backend = CaptureBackend::Synthetic;
    m_impl = new Impl();
    m_pattern.assign(m_width * m_height * 3 / 2, 0);
    fillTestPattern();
    LOGI("capture: synthetic test pattern %zux%zu NV12 (desktop not used)",
         m_width, m_height);
    return true;
}

void ScreenCapture::fillTestPattern() {
    const size_t w = m_width, h = m_height;
    if (w == 0 || h == 0 || m_pattern.size() < w * h * 3 / 2) return;

    // Eight vertical bars in descending luma, the classic test card.
    static const uint8_t kLuma[8] = {235, 210, 170, 145, 106, 81, 41, 16};
    // A paired chroma per bar (U, V) so the colours are obvious on a TV.
    static const uint8_t kU[8] = {128, 16, 166, 54, 202, 90, 240, 128};
    static const uint8_t kV[8] = {128, 146, 16, 34, 222, 240, 110, 128};

    uint8_t* y = m_pattern.data();
    uint8_t* uv = y + w * h;

    const size_t barW = (w + 7) / 8;
    // A white column sweeping left to right once per second: a frozen picture
    // is then unmistakable, and the encoder is forced to emit real frames
    // instead of repeating the previous one.
    const size_t sweep = (size_t)((m_patternFrame * w) / 30) % w;

    for (size_t row = 0; row < h; ++row) {
        uint8_t* line = y + row * w;
        for (size_t x = 0; x < w; ++x) {
            size_t bi = x / barW;
            if (bi > 7) bi = 7;
            line[x] = kLuma[bi];
        }
    }
    for (size_t row = 0; row < h; ++row) {
        const size_t x0 = sweep;
        size_t x1 = sweep + w / 24 + 2;
        if (x1 > w) x1 = w;
        if (x0 >= x1) continue;
        uint8_t* line = y + row * w;
        for (size_t x = x0; x < x1; ++x) line[x] = 235;
    }

    for (size_t row = 0; row < h / 2; ++row) {
        uint8_t* line = uv + row * w;
        for (size_t col = 0; col < w / 2; ++col) {
            size_t bi = (col * 2) / barW;
            if (bi > 7) bi = 7;
            line[col * 2] = kU[bi];
            line[col * 2 + 1] = kV[bi];
        }
    }

    // Text-like detail across the lower third.
    //
    // Without this the card is useless as a bitrate probe: eight flat bars
    // compress to ~300 kbps at 1080p30, the rate controller never has to spend
    // anything, and every bitrate setting produces byte-identical output. That
    // made "did my bitrate knob do anything?" unanswerable. Narrow dark runs on
    // a light field are roughly what a text-heavy desktop costs to encode, and
    // shifting them two pixels per frame keeps them from being predicted away.
    const size_t docTop = h / 3;
    const size_t docBottom = h * 2 / 3;
    const size_t shift = (m_patternFrame * 2) % 16;
    for (size_t row = docTop; row < docBottom; ++row) {
        uint8_t* line = y + row * w;
        // One seed per text line, so runs stay stable down the line and change
        // between lines -- real glyph rows behave the same way.
        uint32_t s = 0x9e3779b9u ^ (uint32_t)((row / 8) * 2654435761u);
        // Blank space between text lines, like line leading.
        if (row % 8 == 7) {
            for (size_t x = 0; x < w; ++x) line[x] = 236;
            continue;
        }
        size_t x = 0;
        while (x < w) {
            s = s * 1664525u + 1013904223u;
            // Light gap first, then a short dark run: text is mostly paper.
            size_t gap = 2 + ((s >> 8) & 0x3);
            if (gap > w - x) gap = w - x;
            for (size_t k = 0; k < gap; ++k) line[x++] = 236;
            size_t run = 1 + ((s >> 20) & 0x3);
            if (run > w - x) run = w - x;
            for (size_t k = 0; k < run; ++k) line[x++] = 30;
        }
        // Shift the whole row sideways so the encoder sees new motion.
        if (shift != 0 && w > 16) {
            uint8_t tmp[16];
            for (size_t k = 0; k < shift; ++k) tmp[k] = line[k];
            for (size_t k = 0; k + shift < w; ++k) line[k] = line[k + shift];
            for (size_t k = 0; k < shift; ++k) line[w - shift + k] = tmp[k];
        }
    }

    ++m_patternFrame;
}

bool ScreenCapture::recover() {
    // A rebuild is expensive (device + duplication + allocations) and the
    // condition that caused it usually lasts a few seconds, so never run it
    // more than once per 1.5 s.
    const uint64_t now = ::GetTickCount64();
    if (m_lastRecoverMs != 0 && now - m_lastRecoverMs < 1500) return false;
    m_lastRecoverMs = now;

    const size_t wantW = m_srcWidth, wantH = m_srcHeight;
    LOGW("capture: desktop duplication lost, rebuilding (was %zux%zu)", wantW, wantH);
    // m_device is recomputed by start() from the argument, so pass what we had.
    const std::wstring device = m_device;
    if (!start(m_maxWidth, device)) {
        LOGW("capture: rebuild failed, no capture until the session restarts");
        return false;
    }
    if (m_srcWidth != wantW || m_srcHeight != wantH) {
        // The encoder's input type, the mirror header and the receiver's surface
        // were all sized for wantW x wantH; feeding a different geometry would
        // produce garbage instead of a picture, so stop cleanly instead.
        LOGW("capture: desktop resized %zux%zu -> %zux%zu, restart the mirror to pick it up",
             wantW, wantH, m_srcWidth, m_srcHeight);
        m_sizeFrozen = true;
        return false;
    }
    return true;
}

bool ScreenCapture::acquireFrame(CapturedFrame& out) {
    out.valid = false;
    out.data = nullptr;

    if (m_testPattern) {
        if (m_pattern.empty()) return false;
        fillTestPattern();
        out.data = m_pattern.data();
        out.format = PixelFormat::Nv12;
        out.width = m_width;
        out.height = m_height;
        out.stride = m_width;
        out.timestampNs = mono_ns();
        out.valid = true;
        return true;
    }

    if (!m_impl || m_sizeFrozen) return false;

    const uint8_t* src = nullptr;
    size_t srcStride = 0;

    if (m_impl->gdi) {
        if (!BitBlt(m_impl->memDc, 0, 0, (int)m_srcWidth, (int)m_srcHeight, m_impl->screenDc,
                    m_gdiOriginX, m_gdiOriginY, SRCCOPY | CAPTUREBLT)) {
            // CAPTUREBLT asks for layered windows too and is refused outright in
            // some sessions (it comes back as ERROR_ACCESS_DENIED, which reads
            // like "no desktop for you" even when a plain copy would work).
            // Losing tooltips and shadows beats losing the picture.
            if (!BitBlt(m_impl->memDc, 0, 0, (int)m_srcWidth, (int)m_srcHeight, m_impl->screenDc,
                        m_gdiOriginX, m_gdiOriginY, SRCCOPY)) {
                m_lastError = GetLastError();
                LOGW("capture: BitBlt failed (%lu)", m_lastError);
                return false;
            }
        }
        m_lastError = 0;
        src = reinterpret_cast<const uint8_t*>(m_impl->bits);
        srcStride = m_srcWidth * 4;
    } else {
        ComPtr<ID3D11Texture2D> desktop;
        bool ok = false;

        if (m_wgc) {
            // DWM-composition path. There is no frame to release here: the
            // capture object keeps the surface alive until release().
            HRESULT hr = S_OK;
            ID3D11Texture2D* tex = m_wgc->acquire(&hr);
            if (!tex) {
                if (FAILED(hr)) {
                    // A capture session that stopped producing frames does not
                    // recover by itself (the monitor was reconfigured, the
                    // system closed the item), so rebuild it like a lost
                    // duplication.
                    m_lastError = (unsigned long)hr;
                    LOGW("capture: DWM capture produced nothing (0x%08lx)", (unsigned long)hr);
                    if (recover()) m_lastError = 0;
                } else {
                    m_lastError = 0; // screen unchanged
                }
                return false;
            }
            desktop = tex; // ComPtr AddRefs; the capture object still owns it
            ok = true;
        } else {
            if (!m_impl->dup) return false;

            DXGI_OUTDUPL_FRAME_INFO info{};
            ComPtr<IDXGIResource> res;
            HRESULT hr = m_impl->dup->AcquireNextFrame(0, &info, &res);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                m_lastError = 0; // screen unchanged
                return false;
            }
            if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
                // The monitor went to sleep, the session locked, or the display
                // mode changed. The duplication object is dead for good at this
                // point -- it has to be rebuilt, otherwise every later frame
                // fails the same way and the TV silently keeps the last picture.
                m_lastError = (unsigned long)hr;
                if (recover()) m_lastError = 0;
                return false;
            }
            if (FAILED(hr)) {
                m_lastError = (unsigned long)hr;
                LOGW("capture: AcquireNextFrame failed (0x%08lx)", (unsigned long)hr);
                return false;
            }
            m_lastError = 0;
            ok = SUCCEEDED(res.As(&desktop));
        }

        // Convert (and scale) on the GPU, then pull back 1.5 bytes per pixel
        // instead of 4. For the duplication path the readback has to happen
        // before ReleaseFrame: the duplicated texture is only valid until then.
        const int staged = ok ? stageTexture(desktop.Get()) : -1;
        if (m_impl->dup) m_impl->dup->ReleaseFrame();
        if (m_wgc) m_wgc->release();
        if (staged < 0) {
            LOGW("capture: frame copy failed");
            return false;
        }
        if (m_conv) {
            out.data = m_impl->nv12.data();
            out.format = PixelFormat::Nv12;
            out.width = m_width;
            out.height = m_height;
            out.stride = m_width;
            out.timestampNs = mono_ns();
            out.valid = true;
            return true;
        }
        src = m_impl->cpu.data();
        srcStride = m_srcWidth * 4;
    }

    if (m_width != m_srcWidth || m_height != m_srcHeight) {
        downscaleBgra(src, m_srcWidth, m_srcHeight, srcStride, m_impl->scaled.data(), m_width,
                      m_height, m_width * 4);
        out.data = m_impl->scaled.data();
    } else {
        out.data = const_cast<uint8_t*>(src);
    }
    out.format = PixelFormat::Bgra;
    out.width = m_width;
    out.height = m_height;
    out.stride = m_width * 4;
    out.timestampNs = mono_ns();
    out.valid = true;
    return true;
}

} // namespace fc
