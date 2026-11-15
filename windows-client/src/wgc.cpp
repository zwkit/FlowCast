// FlowCast Windows client - Windows Graphics Capture backend (DWM composition).
#include "wgc.h"
#include "log.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace wgcns = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
namespace wd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

#include <string>

namespace fc {

struct WgcCapture::Impl {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    wd3d::IDirect3DDevice rtDevice{nullptr};
    wgcns::GraphicsCaptureItem item{nullptr};
    wgcns::Direct3D11CaptureFramePool pool{nullptr};
    wgcns::GraphicsCaptureSession session{nullptr};
    // Kept alive between acquire() and release(): the texture belongs to the
    // frame, and letting the frame go would leave a dangling surface.
    wgcns::Direct3D11CaptureFrame frame{nullptr};
    winrt::com_ptr<ID3D11Texture2D> texture;

    int width = 0;
    int height = 0;
    // When the current stretch of "no frame yet" started, so a capture that
    // died can be told apart from a desktop nobody is touching.
    ULONGLONG waitStartMs = 0;
    bool started = false;
};

namespace {

struct MonitorSearch {
    std::wstring name;
    HMONITOR match = nullptr;
    HMONITOR primary = nullptr;
};

BOOL CALLBACK find_monitor(HMONITOR mon, HDC, LPRECT, LPARAM data) {
    auto* out = reinterpret_cast<MonitorSearch*>(data);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(mon, &mi)) return TRUE;
    if (!out->primary && (mi.dwFlags & MONITORINFOF_PRIMARY)) out->primary = mon;
    if (!out->name.empty() && _wcsicmp(mi.szDevice, out->name.c_str()) == 0) out->match = mon;
    return TRUE;
}

// A monitor capture that never produces a frame is a broken capture: unlike the
// desktop changing, it cannot fix itself. 8 s is well past the first frame WGC
// delivers on StartCapture even on a static screen.
constexpr ULONGLONG kNoFrameTimeoutMs = 8000;

} // namespace

WgcCapture::~WgcCapture() {
    stop();
}

bool WgcCapture::supported() {
    // Asked once: the answer depends on the OS build, not on the session.
    static const bool ok = []() {
        try {
            return wgcns::GraphicsCaptureSession::IsSupported();
        } catch (...) {
            // Windows 10 1803 and older have no such runtime call at all.
            return false;
        }
    }();
    return ok;
}

bool WgcCapture::start(const std::wstring& device, HRESULT* hr) {
    if (!hr) return false;
    *hr = S_OK;
    stop();

    // The pool is free-threaded, but activating WinRT types still needs an
    // apartment on this thread. MTA, because the video thread never pumps
    // messages and cannot be an STA.
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (winrt::hresult_error const& e) {
        if (e.code() != RPC_E_CHANGED_MODE) { // already initialised: fine
            *hr = e.code();
            return false;
        }
    }

    Impl* im = new Impl();
    try {
        D3D_FEATURE_LEVEL level{};
        HRESULT h = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                        D3D11_SDK_VERSION, im->device.put(), &level,
                                        im->context.put());
        if (FAILED(h)) {
            LOGW("wgc: hardware D3D11 device failed (0x%08lx), trying WARP", (unsigned long)h);
            h = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                    D3D11_SDK_VERSION, im->device.put(), &level, im->context.put());
        }
        if (FAILED(h)) {
            LOGW("wgc: no D3D11 device at all (0x%08lx)", (unsigned long)h);
            *hr = h;
            delete im;
            return false;
        }
        LOGI("wgc: D3D feature level 0x%x", (unsigned)level);

        winrt::com_ptr<IDXGIDevice> dxgi;
        if (FAILED(im->device->QueryInterface(IID_PPV_ARGS(dxgi.put())))) {
            *hr = E_NOINTERFACE;
            delete im;
            return false;
        }
        h = ::CreateDirect3D11DeviceFromDXGIDevice(
            dxgi.get(), reinterpret_cast<::IInspectable**>(winrt::put_abi(im->rtDevice)));
        if (FAILED(h)) {
            LOGW("wgc: CreateDirect3D11DeviceFromDXGIDevice failed (0x%08lx)", (unsigned long)h);
            *hr = h;
            delete im;
            return false;
        }

        MonitorSearch search;
        search.name = device;
        ::EnumDisplayMonitors(nullptr, nullptr, find_monitor, (LPARAM)&search);
        HMONITOR mon = search.match ? search.match : search.primary;
        if (!mon) mon = ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);

        auto interop = winrt::get_activation_factory<wgcns::GraphicsCaptureItem,
                                                     IGraphicsCaptureItemInterop>();
        h = interop->CreateForMonitor(mon, winrt::guid_of<wgcns::GraphicsCaptureItem>(),
                                      winrt::put_abi(im->item));
        if (FAILED(h)) {
            LOGW("wgc: CreateForMonitor failed (0x%08lx)", (unsigned long)h);
            *hr = h;
            delete im;
            return false;
        }

        const auto size = im->item.Size();
        im->pool = wgcns::Direct3D11CaptureFramePool::CreateFreeThreaded(
            im->rtDevice, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        im->session = im->pool.CreateCaptureSession(im->item);
        try {
            im->session.IsCursorCaptureEnabled(true);
        } catch (...) {
            // Best effort: without the cursor things still work.
        }
        try {
            // Windows 11 only. Removing the yellow capture border keeps the
            // mirrored picture clean; Windows 10 throws here and keeps it.
            im->session.IsBorderRequired(false);
        } catch (...) {
        }
        im->session.StartCapture();

        im->width = (int)size.Width;
        im->height = (int)size.Height;
        im->started = true;
        m_impl = im;
        LOGI("wgc: capturing monitor %dx%d via DWM composition", im->width, im->height);
        return true;
    } catch (winrt::hresult_error const& e) {
        LOGW("wgc: start failed (0x%08lx)", (unsigned long)e.code());
        *hr = e.code();
        delete im;
        return false;
    } catch (...) {
        *hr = E_FAIL;
        delete im;
        return false;
    }
}

void WgcCapture::stop() {
    if (!m_impl) return;
    Impl* im = m_impl;
    m_impl = nullptr;
    im->started = false;
    try {
        if (im->session) im->session.Close();
        if (im->pool) im->pool.Close();
    } catch (...) {
    }
    im->texture = nullptr;
    im->frame = nullptr;
    im->session = nullptr;
    im->pool = nullptr;
    im->item = nullptr;
    im->rtDevice = nullptr;
    delete im;
}

ID3D11Device* WgcCapture::device() const {
    return m_impl ? m_impl->device.get() : nullptr;
}

ID3D11DeviceContext* WgcCapture::context() const {
    return m_impl ? m_impl->context.get() : nullptr;
}

ID3D11Texture2D* WgcCapture::acquire(HRESULT* hr) {
    if (!hr) return nullptr;
    *hr = S_OK;
    if (!m_impl || !m_impl->started) {
        *hr = E_FAIL;
        return nullptr;
    }
    Impl* im = m_impl;
    release();
    try {
        auto frame = im->pool.TryGetNextFrame();
        if (!frame) {
            const ULONGLONG now = ::GetTickCount64();
            if (im->waitStartMs == 0) im->waitStartMs = now;
            if (now - im->waitStartMs > kNoFrameTimeoutMs) {
                *hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
            return nullptr;
        }
        im->waitStartMs = 0;

        auto access = frame.Surface()
                          .as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> tex;
        const HRESULT h =
            access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), tex.put_void());
        if (FAILED(h)) {
            *hr = h;
            return nullptr;
        }
        D3D11_TEXTURE2D_DESC desc{};
        tex->GetDesc(&desc);
        im->width = (int)desc.Width;
        im->height = (int)desc.Height;
        im->frame = frame;
        im->texture = tex;
        return tex.get();
    } catch (winrt::hresult_error const& e) {
        *hr = e.code();
        return nullptr;
    } catch (...) {
        *hr = E_FAIL;
        return nullptr;
    }
}

void WgcCapture::release() {
    if (!m_impl) return;
    m_impl->texture = nullptr;
    m_impl->frame = nullptr;
}

int WgcCapture::width() const {
    return m_impl ? m_impl->width : 0;
}

int WgcCapture::height() const {
    return m_impl ? m_impl->height : 0;
}

} // namespace fc
