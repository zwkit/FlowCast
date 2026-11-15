// FlowCast Windows client - D3D11 video processor BGRA -> NV12 converter.
#include "nv12conv.h"
#include "log.h"

#include <wrl/client.h>

#include <cstring>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;

namespace fc {

struct Nv12Converter::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;

    ComPtr<ID3D11Texture2D> dst;               // NV12, render target of the blit
    ComPtr<ID3D11VideoProcessorOutputView> dstView;
    ComPtr<ID3D11Texture2D> staging;           // NV12, CPU readable

    // Input view of the current desktop texture. DXGI reuses the same texture
    // object for successive frames, so one view lasts the whole session; if it
    // ever hands out a different object the view is rebuilt.
    ComPtr<ID3D11VideoProcessorInputView> srcView;
    ID3D11Texture2D* srcTexture = nullptr;
    // Only created when the duplicated texture itself refuses an input view.
    ComPtr<ID3D11Texture2D> srcCopy;

    UINT srcW = 0, srcH = 0;
};

Nv12Converter::~Nv12Converter() {
    reset();
}

void Nv12Converter::reset() {
    delete m_impl;
    m_impl = nullptr;
    m_srcW = m_srcH = m_dstW = m_dstH = 0;
}

bool Nv12Converter::init(ID3D11Device* device, ID3D11DeviceContext* context, size_t srcWidth,
                         size_t srcHeight, size_t dstWidth, size_t dstHeight) {
    reset();
    m_error = "";
    if (!device || !context || srcWidth == 0 || srcHeight == 0 || dstWidth == 0 || dstHeight == 0) {
        m_error = "invalid size or device";
        return false;
    }

    Impl* im = new Impl();
    im->device = device;
    im->context = context;
    im->srcW = (UINT)srcWidth;
    im->srcH = (UINT)srcHeight;

    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&im->videoDevice))) || !im->videoDevice) {
        m_error = "device has no video processor";
        delete im;
        return false;
    }
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&im->videoContext))) || !im->videoContext) {
        m_error = "context has no video processor";
        delete im;
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate.Numerator = 30;
    cd.InputFrameRate.Denominator = 1;
    cd.OutputFrameRate.Numerator = 30;
    cd.OutputFrameRate.Denominator = 1;
    cd.InputWidth = (UINT)srcWidth;
    cd.InputHeight = (UINT)srcHeight;
    cd.OutputWidth = (UINT)dstWidth;
    cd.OutputHeight = (UINT)dstHeight;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    HRESULT hr = im->videoDevice->CreateVideoProcessorEnumerator(&cd, &im->enumerator);
    if (FAILED(hr)) {
        m_error = "CreateVideoProcessorEnumerator failed";
        LOGE("nv12: enumerator failed (0x%08lx)", (unsigned long)hr);
        delete im;
        return false;
    }

    // Both ends of the conversion have to be supported by this driver. This is
    // asked on the enumerator, not on the device.
    UINT inSupport = 0, outSupport = 0;
    im->enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &inSupport);
    im->enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &outSupport);
    if (!(inSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
        m_error = "BGRA is not accepted as a video processor input";
        delete im;
        return false;
    }
    if (!(outSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        m_error = "NV12 is not accepted as a video processor output";
        delete im;
        return false;
    }

    // Rate-conversion index 0 is the plain frame-rate-conversion-free path.
    hr = im->videoDevice->CreateVideoProcessor(im->enumerator.Get(), 0, &im->processor);
    if (FAILED(hr)) {
        m_error = "CreateVideoProcessor failed";
        LOGE("nv12: processor failed (0x%08lx)", (unsigned long)hr);
        delete im;
        return false;
    }

    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)dstWidth;
    d.Height = (UINT)dstHeight;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_NV12;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr = device->CreateTexture2D(&d, nullptr, &im->dst);
    if (FAILED(hr)) {
        m_error = "could not create the NV12 render target";
        LOGE("nv12: dst texture failed (0x%08lx)", (unsigned long)hr);
        delete im;
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovd.Texture2D.MipSlice = 0;
    hr = im->videoDevice->CreateVideoProcessorOutputView(im->dst.Get(), im->enumerator.Get(), &ovd,
                                                         &im->dstView);
    if (FAILED(hr)) {
        m_error = "could not create the output view";
        LOGE("nv12: output view failed (0x%08lx)", (unsigned long)hr);
        delete im;
        return false;
    }

    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device->CreateTexture2D(&sd, nullptr, &im->staging);
    if (FAILED(hr)) {
        m_error = "could not create the readback texture";
        LOGE("nv12: staging texture failed (0x%08lx)", (unsigned long)hr);
        delete im;
        return false;
    }

    // --- processor state ----------------------------------------------------
    ID3D11VideoContext* vc = im->videoContext.Get();
    ID3D11VideoProcessor* vp = im->processor.Get();
    vc->VideoProcessorSetStreamFrameFormat(vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vc->VideoProcessorSetStreamOutputRate(vp, 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, TRUE,
                                          nullptr);
    // Auto processing is the deinterlace/denoise pipeline; a progressive desktop
    // needs none of it. Scaling quality is left to the driver: the D3D11 video
    // processor has no filter-quality knob (D3D11_VIDEO_PROCESSOR_FILTER is the
    // proc-amp block, not the scaler), and its default is multi-tap.
    vc->VideoProcessorSetStreamAutoProcessingMode(vp, 0, FALSE);

    // Desktop pixels are full-range RGB; the stream is encoded as limited-range
    // BT.709, which is what an HD H.264 decoder assumes. Getting this pair
    // wrong is what makes a mirrored desktop look washed out or crushed.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
    in.Usage = 0;      // playback
    in.RGB_Range = 0;  // full range in
    in.YCbCr_Matrix = 1; // BT.709
    in.YCbCr_xvYCC = 0;
    in.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    vc->VideoProcessorSetStreamColorSpace(vp, 0, &in);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
    out.Usage = 0;
    out.RGB_Range = 0;
    out.YCbCr_Matrix = 1;
    out.YCbCr_xvYCC = 0;
    out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    vc->VideoProcessorSetOutputColorSpace(vp, &out);

    RECT dstRect{0, 0, (LONG)dstWidth, (LONG)dstHeight};
    vc->VideoProcessorSetOutputTargetRect(vp, TRUE, nullptr);
    vc->VideoProcessorSetStreamDestRect(vp, 0, TRUE, &dstRect);

    m_impl = im;
    m_srcW = srcWidth;
    m_srcH = srcHeight;
    m_dstW = dstWidth;
    m_dstH = dstHeight;
    return true;
}

bool Nv12Converter::convert(ID3D11Texture2D* bgra) {
    Impl* im = m_impl;
    if (!im || !bgra) return false;

    if (im->srcTexture != bgra || !im->srcView) {
        im->srcView.Reset();
        im->srcCopy.Reset();
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.FourCC = 0;
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.MipSlice = 0;
        ivd.Texture2D.ArraySlice = 0;
        HRESULT hr = im->videoDevice->CreateVideoProcessorInputView(
            bgra, im->enumerator.Get(), &ivd, &im->srcView);
        if (FAILED(hr)) {
            // A duplicated texture is not a render target; a few drivers want
            // an ordinary texture on the input side. One GPU-side copy buys
            // back the whole optimisation, so it is worth paying.
            LOGW("nv12: input view on the duplicated texture failed (0x%08lx), copying first",
                 (unsigned long)hr);
            D3D11_TEXTURE2D_DESC cd{};
            bgra->GetDesc(&cd);
            cd.Usage = D3D11_USAGE_DEFAULT;
            cd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            cd.CPUAccessFlags = 0;
            cd.MiscFlags = 0;
            cd.MipLevels = 1;
            cd.ArraySize = 1;
            if (FAILED(im->device->CreateTexture2D(&cd, nullptr, &im->srcCopy))) {
                LOGE("nv12: could not create the intermediate texture");
                return false;
            }
            hr = im->videoDevice->CreateVideoProcessorInputView(im->srcCopy.Get(),
                                                                im->enumerator.Get(), &ivd,
                                                                &im->srcView);
            if (FAILED(hr)) {
                LOGE("nv12: could not create an input view at all (0x%08lx)",
                     (unsigned long)hr);
                return false;
            }
        }
        im->srcTexture = bgra;
    }

    ID3D11VideoContext* vc = im->videoContext.Get();
    ID3D11VideoProcessor* vp = im->processor.Get();

    if (im->srcCopy) im->context->CopyResource(im->srcCopy.Get(), bgra);

    RECT srcRect{0, 0, (LONG)im->srcW, (LONG)im->srcH};
    vc->VideoProcessorSetStreamSourceRect(vp, 0, TRUE, &srcRect);

    D3D11_VIDEO_PROCESSOR_STREAM s{};
    s.Enable = TRUE;
    s.OutputIndex = 0;
    s.InputFrameOrField = 0;
    s.PastFrames = 0;
    s.FutureFrames = 0;
    s.pInputSurface = im->srcView.Get();
    HRESULT hr = vc->VideoProcessorBlt(vp, im->dstView.Get(), 0, 1, &s);
    if (FAILED(hr)) {
        LOGE("nv12: VideoProcessorBlt failed (0x%08lx)", (unsigned long)hr);
        return false;
    }
    im->context->CopyResource(im->staging.Get(), im->dst.Get());
    return true;
}

bool Nv12Converter::readback(uint8_t* dst) {
    Impl* im = m_impl;
    if (!im || !dst) return false;

    D3D11_MAPPED_SUBRESOURCE m{};
    HRESULT hr = im->context->Map(im->staging.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
        LOGE("nv12: readback map failed (0x%08lx)", (unsigned long)hr);
        return false;
    }
    const uint8_t* base = reinterpret_cast<const uint8_t*>(m.pData);
    const size_t w = m_dstW, h = m_dstH;
    for (size_t y = 0; y < h; y++) {
        memcpy(dst + y * w, base + y * m.RowPitch, w);
    }
    // The chroma plane starts one full luma plane (RowPitch * height) in, and
    // its rows use the same pitch.
    const uint8_t* uvSrc = base + m.RowPitch * h;
    uint8_t* uvDst = dst + w * h;
    for (size_t y = 0; y < h / 2; y++) {
        memcpy(uvDst + y * w, uvSrc + y * m.RowPitch, w);
    }
    im->context->Unmap(im->staging.Get(), 0);
    return true;
}

} // namespace fc
