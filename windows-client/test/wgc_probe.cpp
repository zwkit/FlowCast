// FlowCast Windows client - capability probe for Windows Graphics Capture.
//
// Desktop Duplication and plain GDI BitBlt are both refused in some sessions
// (a locked screen, a remote desktop), which leaves the client with audio only.
// Windows Graphics Capture goes through DWM instead of the display output, so
// it can work where the other two cannot. This probe answers that question in
// isolation before any of it is wired into capture.cpp: can we get one frame
// of the primary monitor right now, and how big is it?
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <d3d11.h>
#include <cstdio>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
namespace wd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

int main() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (winrt::hresult_error const& e) {
        std::printf("wgc probe: RoInitialize failed (0x%08lx)\n", (unsigned long)e.code());
        return 1;
    }

    std::printf("wgc probe: GraphicsCaptureSession::IsSupported() = %s\n",
                wgc::GraphicsCaptureSession::IsSupported() ? "yes" : "no");

    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                     D3D11_SDK_VERSION, d3d.put(), &fl, ctx.put());
    if (FAILED(hr)) {
        std::printf("wgc probe: D3D11CreateDevice failed (0x%08lx)\n", (unsigned long)hr);
        return 1;
    }

    winrt::com_ptr<IDXGIDevice> dxgi;
    d3d.as(dxgi);
    wd3d::IDirect3DDevice device{nullptr};
    hr = ::CreateDirect3D11DeviceFromDXGIDevice(
        dxgi.get(), reinterpret_cast<::IInspectable**>(winrt::put_abi(device)));
    if (FAILED(hr)) {
        std::printf("wgc probe: CreateDirect3D11DeviceFromDXGIDevice failed (0x%08lx)\n",
                    (unsigned long)hr);
        return 1;
    }

    // The primary monitor is what --display 1 means everywhere else.
    const HMONITOR mon = ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem,
                                                 IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{nullptr};
    hr = interop->CreateForMonitor(mon, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                   winrt::put_abi(item));
    if (FAILED(hr)) {
        std::printf("wgc probe: CreateForMonitor failed (0x%08lx)\n", (unsigned long)hr);
        return 1;
    }
    std::printf("wgc probe: item %dx%d\n", item.Size().Width, item.Size().Height);

    auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        device, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
    auto session = pool.CreateCaptureSession(item);
    session.StartCapture();

    wgc::Direct3D11CaptureFrame frame{nullptr};
    for (int i = 0; i < 200 && !frame; ++i) {
        frame = pool.TryGetNextFrame();
        if (!frame) ::Sleep(10);
    }
    if (!frame) {
        std::printf("wgc probe: no frame in 2 s\n");
        session.Close();
        return 1;
    }

    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::
                                         IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> tex;
    hr = access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), tex.put_void());
    if (FAILED(hr)) {
        std::printf("wgc probe: surface -> texture failed (0x%08lx)\n", (unsigned long)hr);
        return 1;
    }
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);

    // Sample a few rows rather than one pixel: the desktop is mostly one colour
    // at the top (title bars) so a single sample proves nothing.
    auto sample = [&](const wgc::Direct3D11CaptureFrame& fr) -> std::pair<unsigned long, unsigned long> {
        auto acc = fr.Surface()
                       .as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> t;
        if (FAILED(acc->GetInterface(winrt::guid_of<ID3D11Texture2D>(), t.put_void()))) return {0, 0};
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        D3D11_TEXTURE2D_DESC s = d;
        s.Usage = D3D11_USAGE_STAGING;
        s.BindFlags = 0;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        s.MiscFlags = 0;
        s.ArraySize = 1;
        s.MipLevels = 1;
        winrt::com_ptr<ID3D11Texture2D> st;
        if (FAILED(d3d->CreateTexture2D(&s, nullptr, st.put()))) return {0, 0};
        ctx->CopyResource(st.get(), t.get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(st.get(), 0, D3D11_MAP_READ, 0, &m))) return {0, 0};
        unsigned long nz = 0, total = 0;
        const uint8_t* b = static_cast<const uint8_t*>(m.pData);
        for (UINT y = 0; y < d.Height; y += d.Height / 16 + 1) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(b + (size_t)y * m.RowPitch);
            for (UINT x = 0; x < d.Width; x += d.Width / 16 + 1) {
                ++total;
                if ((row[x] & 0x00FFFFFFu) != 0) ++nz;
            }
        }
        ctx->Unmap(st.get(), 0);
        return {nz, total};
    };
    for (int k = 0; k < 5; ++k) {
        const auto [nz, total] = sample(frame);
        std::printf("wgc probe: frame %d %ux%u non-black %lu/%lu\n", k, desc.Width, desc.Height, nz,
                    total);
        frame.Close();
        frame = nullptr;
        for (int i = 0; i < 50 && !frame; ++i) {
            frame = pool.TryGetNextFrame();
            if (!frame) ::Sleep(10);
        }
        if (!frame) break;
    }
    std::printf("wgc probe: Windows Graphics Capture usable\n");

    if (frame) frame.Close();
    session.Close();
    pool.Close();
    return 0;
}
