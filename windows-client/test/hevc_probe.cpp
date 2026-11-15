// FlowCast Windows client - HEVC encoder capability probe.
//
// Run before touching the encoder, because the answer decides how much code the
// 4K path costs. Three questions, in the order that matters:
//
//   1. Is there an HEVC encoder MFT at all, and is it hardware-backed?
//   2. Will it accept 3840x2160 NV12 at 30 fps?
//   3. Does it take CPU memory buffers, or does it demand a D3D11 texture?
//
// (3) is the expensive one. A CPU-buffer encoder drops straight into the
// existing capture -> encodeFrame(BGRA) pipeline. A D3D11-only encoder needs a
// video-processor stage in front of it and a texture-aware capture path.
//
// H.264 runs the same battery as a control: if 4K fails for both, the probe is
// wrong; if it fails only for HEVC, the answer is real.
//
// Build: build.bat probe    (deliberately not named *_test.cpp so the self-test
// target does not pick it up)
#include "src/mferr.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <mferror.h>
#include <d3d11.h>
#include <d3d10.h> // ID3D10Multithread, which MF insists on for shared devices
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "strmiids.lib")

namespace {

// A D3D11 device is not optional for the AMD MFTs: they reject system-memory
// input outright, and every one of them reports MF_SA_D3D11_AWARE. Whether they
// will take a plain memory buffer once a device manager is attached is exactly
// what the probe has to find out - if they do, the capture path stays as-is and
// only the encoder changes.
struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IMFDXGIDeviceManager> manager;
    UINT resetToken = 0;
    bool ready = false;

    bool init() {
        UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        ComPtr<ID3D11DeviceContext> context;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr,
                                       0, D3D11_SDK_VERSION, &device, nullptr, &context);
        if (FAILED(hr)) return false;
        ctx = context;

        // MF requires the device to be multithread-protected before it hands
        // textures across threads.
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device.As(&mt))) mt->SetMultithreadProtected(TRUE);

        if (FAILED(MFCreateDXGIDeviceManager(&resetToken, &manager))) return false;
        if (FAILED(manager->ResetDevice(device.Get(), resetToken))) return false;
        ready = true;
        return true;
    }
};

Gpu g_gpu;

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string guid_str(REFGUID g) {
    wchar_t buf[64] = {};
    ::StringFromGUID2(g, buf, 64);
    return narrow(buf);
}

struct Encoder {
    ComPtr<IMFActivate> activate;
    std::string name = "(unnamed)";
    CLSID clsid{};
    bool hasClsid = false;
    bool hardware = false;
    bool async = false;
    bool d3dAware = false;
};

// MFTEnumEx with the same flags the production code uses, so the list here is
// the list the encoder will actually pick from.
std::vector<Encoder> enumEncoders(REFGUID subtype, bool requireHardware) {
    std::vector<Encoder> out;
    MFT_REGISTER_TYPE_INFO in = {MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outType = {MFMediaType_Video, subtype};
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    const DWORD flags = requireHardware
                            ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT |
                               MFT_ENUM_FLAG_ASYNCMFT)
                            : MFT_ENUM_FLAG_ALL;
    const HRESULT hr =
        MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &outType, &acts, &count);
    if (FAILED(hr) || !acts) return out;

    for (UINT32 i = 0; i < count; ++i) {
        Encoder e;
        wchar_t* friendly = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &len)) &&
            friendly) {
            e.name = narrow(friendly);
            ::CoTaskMemFree(friendly);
        }
        BOOL flag = FALSE;
        if (SUCCEEDED(acts[i]->GetUINT32(MF_SA_D3D11_AWARE, (UINT32*)&flag))) e.d3dAware = flag != 0;
        flag = FALSE;
        if (SUCCEEDED(acts[i]->GetUINT32(MF_TRANSFORM_ASYNC, (UINT32*)&flag))) e.async = flag != 0;
        e.hasClsid = SUCCEEDED(acts[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &e.clsid));
        e.hardware = requireHardware;
        e.activate = acts[i];
        out.push_back(std::move(e));
        acts[i]->Release();
    }
    ::CoTaskMemFree(acts);
    return out;
}

struct Outcome {
    bool typesOk = false;
    bool fedOk = false;
    int nalBytes = 0;
    int framesOut = 0;
    double msPerFrame = 0.0;
    double msWorst = 0.0;
    std::string why;
};

enum class Feed {
    MemoryBuffer, // CPU NV12 in an IMFMediaBuffer - what the client does today
    DxgiTexture,  // NV12 in a D3D11 texture, the only thing AMD's VCN accepts
};

// One full negotiation + feed attempt at a given size and codec.
Outcome tryEncode(IMFTransform* mft, REFGUID subtype, UINT32 profileOrZero, UINT32 w, UINT32 h,
                  UINT32 fps, UINT32 kbps, bool attachD3D, Feed feed) {
    Outcome r;

    HRESULT hr = mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    (void)hr;

    // The device manager has to be in place before any media type is set,
    // otherwise the MFT has already committed to the system-memory path.
    if (attachD3D) {
        if (!g_gpu.ready) {
            r.why = "no D3D11 device";
            return r;
        }
        hr = mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)g_gpu.resetToken);
        if (FAILED(hr)) {
            r.why += std::string("SET_D3D_MANAGER failed: ") + fc::mf_error_str(hr) + "; ";
        }
    }

    // Output type first: encoders refuse an input type until an output exists.
    ComPtr<IMFMediaType> out;
    if (FAILED(MFCreateMediaType(&out))) {
        r.why = "MFCreateMediaType failed";
        return r;
    }
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fps, 1);
    out->SetUINT32(MF_MT_AVG_BITRATE, kbps * 1000u);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (profileOrZero) out->SetUINT32(MF_MT_MPEG2_PROFILE, profileOrZero);

    hr = mft->SetOutputType(0, out.Get(), 0);
    if (FAILED(hr) && profileOrZero) {
        // Profile is advisory for some encoders; retry bare before blaming size.
        out->DeleteItem(MF_MT_MPEG2_PROFILE);
        hr = mft->SetOutputType(0, out.Get(), 0);
        if (SUCCEEDED(hr)) r.why = "(accepted only without an explicit profile) ";
    }
    if (FAILED(hr)) {
        r.why += std::string("output type refused: ") + fc::mf_error_str(hr);
        return r;
    }

    ComPtr<IMFMediaType> in;
    if (FAILED(MFCreateMediaType(&in))) {
        r.why = "MFCreateMediaType failed";
        return r;
    }
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    hr = mft->SetInputType(0, in.Get(), 0);
    if (FAILED(hr)) {
        r.why += std::string("input type refused: ") + fc::mf_error_str(hr);
        return r;
    }
    r.typesOk = true;

    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    // Desktop-like content, not a smooth gradient. Text strokes, panel edges and
    // sensor noise cost an encoder two to three times what a gradient does, and
    // a gradient would make every number below look better than reality.
    const size_t ySize = (size_t)w * h;
    const size_t uvSize = ySize / 2;
    std::vector<uint8_t> nv12(ySize + uvSize);
    uint32_t rng = 0x1234567u;
    for (UINT32 y = 0; y < h; ++y) {
        const bool textBand = (y % 24) < 11; // ~11px "text" lines with 13px gaps
        for (UINT32 x = 0; x < w; ++x) {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            int v = 32;
            if (textBand) {
                // Vertical strokes at a 4px pitch: sharp edges everywhere.
                v = ((x / 4) % 2) ? 210 : 40;
            } else if (x > w / 3 && x < 2 * w / 3) {
                v = 120; // flat panel, cheap to code
            }
            v += (int)(rng & 0x0F) - 8; // sensor noise, never free
            nv12[(size_t)y * w + x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
    std::memset(nv12.data() + ySize, 128, uvSize);

    MFT_OUTPUT_STREAM_INFO si{};
    if (FAILED(mft->GetOutputStreamInfo(0, &si))) {
        r.why += "GetOutputStreamInfo failed";
        return r;
    }

    // Texture upload resources, created once when the texture path is in use.
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11Texture2D> staging;
    if (feed == Feed::DxgiTexture) {
        if (!g_gpu.ready) {
            r.why += "no D3D11 device; ";
            return r;
        }
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_NV12;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        if (FAILED(g_gpu.device->CreateTexture2D(&d, nullptr, &tex))) {
            r.why += "CreateTexture2D(default NV12) failed; ";
            return r;
        }
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_gpu.device->CreateTexture2D(&d, nullptr, &staging))) {
            r.why += "CreateTexture2D(staging NV12) failed; ";
            return r;
        }
    }

    const int kFrames = 60;
    double sumMs = 0.0;
    int timed = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < kFrames; ++f) {
        const auto frameStart = std::chrono::steady_clock::now();
        ComPtr<IMFMediaBuffer> buf;
        if (feed == Feed::DxgiTexture) {
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(g_gpu.ctx->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0, &m))) {
                r.why += "staging Map failed";
                return r;
            }
            auto* db = (uint8_t*)m.pData;
            for (UINT32 y = 0; y < h; ++y) {
                std::memcpy(db + (size_t)y * m.RowPitch, nv12.data() + (size_t)y * w, w);
            }
            // NV12 chroma plane: same row pitch, half the rows, one byte per
            // chroma sample pair.
            uint8_t* duv = db + (size_t)m.RowPitch * h;
            const uint8_t* suv = nv12.data() + ySize;
            for (UINT32 y = 0; y < h / 2; ++y) {
                std::memcpy(duv + (size_t)y * m.RowPitch, suv + (size_t)y * w, w);
            }
            g_gpu.ctx->Unmap(staging.Get(), 0);
            g_gpu.ctx->CopyResource(tex.Get(), staging.Get());
            if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), tex.Get(), 0, FALSE,
                                                 &buf))) {
                r.why += "MFCreateDXGISurfaceBuffer failed";
                return r;
            }
        } else {
            if (FAILED(MFCreateMemoryBuffer((DWORD)nv12.size(), &buf))) {
                r.why += "MFCreateMemoryBuffer failed";
                return r;
            }
            BYTE* dst = nullptr;
            if (FAILED(buf->Lock(&dst, nullptr, nullptr))) {
                r.why += "buffer Lock failed";
                return r;
            }
            std::memcpy(dst, nv12.data(), nv12.size());
            buf->Unlock();
            buf->SetCurrentLength((DWORD)nv12.size());
        }

        ComPtr<IMFSample> sample;
        if (FAILED(MFCreateSample(&sample))) {
            r.why += "MFCreateSample failed";
            return r;
        }
        sample->AddBuffer(buf.Get());
        sample->SetSampleTime((LONGLONG)f * (10000000LL / fps));
        sample->SetSampleDuration(10000000LL / fps);

        hr = mft->ProcessInput(0, sample.Get(), 0);
        if (FAILED(hr)) {
            r.why += std::string("ProcessInput #") + std::to_string(f) + " failed: " +
                     fc::mf_error_str(hr);
            if (hr == MF_E_NOTACCEPTING) r.why += " [async MFT still locked]";
            return r;
        }

        // Drain whatever is ready. Encoders buffer a frame or two, so a failed
        // drain early on is expected and not an error.
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER odb{};
            odb.dwStreamID = 0;
            ComPtr<IMFMediaBuffer> outBuf;
            if (si.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) {
                // leave odb.pSample null, the MFT allocates
            } else {
                const DWORD need = si.cbSize ? si.cbSize : (DWORD)(w * h * 2);
                if (FAILED(MFCreateMemoryBuffer(need, &outBuf))) break;
                ComPtr<IMFSample> os;
                if (FAILED(MFCreateSample(&os))) break;
                os->AddBuffer(outBuf.Get());
                odb.pSample = os.Detach();
            }
            DWORD status = 0;
            const HRESULT dhr = mft->ProcessOutput(0, 1, &odb, &status);
            if (odb.pSample) {
                if (SUCCEEDED(dhr)) {
                    IMFMediaBuffer* mb = nullptr;
                    if (SUCCEEDED(odb.pSample->ConvertToContiguousBuffer(&mb)) && mb) {
                        DWORD len = 0;
                        if (SUCCEEDED(mb->GetCurrentLength(&len))) {
                            r.nalBytes += (int)len;
                            if (len > 0) r.framesOut++;
                        }
                        mb->Release();
                    }
                }
                odb.pSample->Release();
            }
            if (FAILED(dhr)) break;
        }

        const auto frameEnd = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
        // The first frames pay for buffer pooling, JIT and the initial I-frame;
        // a steady-state figure is what tells us whether the pipeline holds.
        if (f >= 10) {
            sumMs += ms;
            if (ms > r.msWorst) r.msWorst = ms;
            ++timed;
        }
    }
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);

    const auto t1 = std::chrono::steady_clock::now();
    (void)t1;
    r.msPerFrame = timed ? sumMs / timed : 0.0;
    r.fedOk = r.nalBytes > 0;
    if (!r.fedOk && r.why.empty()) r.why = "no output produced";
    return r;
}

// The IMFTransform route is blocked on the async handshake, so ask the question
// a different way: can the Media Foundation pipeline as a whole encode this
// codec? Sink Writer picks the encoder itself, handles async MFTs and D3D, and
// fails loudly if no encoder exists. That separates "the encoder is unusable"
// from "our hand-rolled transform setup is wrong".
void sinkWriterTest(REFGUID subtype, REFGUID inSubtype, const wchar_t* path, UINT32 w, UINT32 h,
                    UINT32 fps, UINT32 kbps) {
    ::DeleteFileW(path);
    ComPtr<IMFSinkWriter> writer;
    HRESULT hr = MFCreateSinkWriterFromURL(path, nullptr, nullptr, &writer);
    if (FAILED(hr)) {
        std::printf("  sinkwriter create : %s\n", fc::mf_error_str(hr));
        return;
    }

    ComPtr<IMFMediaType> ot;
    MFCreateMediaType(&ot);
    ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    ot->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(ot.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(ot.Get(), MF_MT_FRAME_RATE, fps, 1);
    ot->SetUINT32(MF_MT_AVG_BITRATE, kbps * 1000u);
    ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    DWORD stream = 0;
    hr = writer->AddStream(ot.Get(), &stream);
    if (FAILED(hr)) {
        std::printf("  AddStream         : %s\n", fc::mf_error_str(hr));
        return;
    }

    ComPtr<IMFMediaType> it;
    MFCreateMediaType(&it);
    it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    it->SetGUID(MF_MT_SUBTYPE, inSubtype);
    MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(it.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(it.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = writer->SetInputMediaType(stream, it.Get(), nullptr);
    if (FAILED(hr)) {
        std::printf("  SetInputMediaType : %s\n", fc::mf_error_str(hr));
        return;
    }

    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        std::printf("  BeginWriting      : %s\n", fc::mf_error_str(hr));
        return;
    }

    const size_t ySize = (size_t)w * h;
    size_t frameBytes = ySize + ySize / 2; // NV12
    if (inSubtype == MFVideoFormat_YUY2) frameBytes = ySize * 2;
    else if (inSubtype == MFVideoFormat_P010) frameBytes = ySize * 3;
    std::vector<uint8_t> nv12(frameBytes);
    uint32_t rng = 0x1234567u;
    for (UINT32 y = 0; y < h; ++y) {
        const bool band = (y % 24) < 11;
        for (UINT32 x = 0; x < w; ++x) {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            int v = band ? (((x / 4) % 2) ? 210 : 40) : 110;
            v += (int)(rng & 0x0F) - 8;
            nv12[(size_t)y * w + x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
    // Chroma (and the high bytes of a 10-bit plane) stay flat: enough to keep
    // the encoder honest, cheap to fill.
    std::memset(nv12.data() + ySize, 128, frameBytes - ySize);

    const int kFrames = 30;
    double sum = 0;
    int wrote = 0;
    for (int f = 0; f < kFrames; ++f) {
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(MFCreateMemoryBuffer((DWORD)nv12.size(), &buf))) break;
        BYTE* dst = nullptr;
        buf->Lock(&dst, nullptr, nullptr);
        std::memcpy(dst, nv12.data(), nv12.size());
        buf->Unlock();
        buf->SetCurrentLength((DWORD)nv12.size());
        ComPtr<IMFSample> s;
        MFCreateSample(&s);
        s->AddBuffer(buf.Get());
        s->SetSampleTime((LONGLONG)f * (10000000LL / fps));
        s->SetSampleDuration(10000000LL / fps);
        const auto a = std::chrono::steady_clock::now();
        hr = writer->WriteSample(stream, s.Get());
        sum += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a)
                   .count();
        if (FAILED(hr)) {
            std::printf("  WriteSample #%d   : %s\n", f, fc::mf_error_str(hr));
            break;
        }
        ++wrote;
    }

    hr = writer->Finalize();
    std::printf("  wrote %d frames, %.1f ms/frame, Finalize=%s\n", wrote, wrote ? sum / wrote : 0.0,
                SUCCEEDED(hr) ? "ok" : fc::mf_error_str(hr));

    // A non-trivial file means real encoded data landed in it.
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (::GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
        std::printf("  output size       : %llu bytes\n",
                    (unsigned long long)(((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow));
    }
    ::DeleteFileW(path);
}

// h264enc.cpp converts BGRA to NV12 with a per-pixel scalar loop before every
// frame. At 1080p that is invisible; at 4K it is 33 MB of reads per frame and
// could easily be the real reason a 4K stream cannot hold 30 fps. Measure it
// with the same algorithm and the same /O2 flags so the number is comparable.
void benchNv12Conversion(UINT32 w, UINT32 h) {
    const size_t stride = (size_t)w * 4;
    std::vector<uint8_t> bgra(stride * h);
    for (size_t i = 0; i < bgra.size(); ++i) bgra[i] = (uint8_t)(i * 37);
    std::vector<uint8_t> nv12((size_t)w * h * 3 / 2);

    const int kIters = 20;
    double sum = 0, worst = 0;
    for (int it = 0; it < kIters; ++it) {
        const auto t0 = std::chrono::steady_clock::now();
        uint8_t* dst = nv12.data();
        for (UINT32 y = 0; y < h; ++y) {
            const uint8_t* src = bgra.data() + (size_t)y * stride;
            uint8_t* dy = dst + (size_t)y * w;
            for (UINT32 x = 0; x < w; ++x) {
                const int b = src[x * 4 + 0], g = src[x * 4 + 1], r = src[x * 4 + 2];
                int yv = (r * 77 + g * 150 + b * 29) >> 8;
                dy[x] = (uint8_t)(yv < 0 ? 0 : (yv > 255 ? 255 : yv));
            }
        }
        uint8_t* duv = dst + (size_t)w * h;
        for (UINT32 y = 0; y < h / 2; ++y) {
            const uint8_t* s0 = bgra.data() + (size_t)(y * 2) * stride;
            const uint8_t* s1 = s0 + stride;
            uint8_t* d = duv + (size_t)y * w;
            for (UINT32 x = 0; x < w / 2; ++x) {
                int r = 0, g = 0, b = 0;
                const uint8_t* p[4] = {s0 + x * 8, s0 + x * 8 + 4, s1 + x * 8, s1 + x * 8 + 4};
                for (int i = 0; i < 4; ++i) {
                    b += p[i][0];
                    g += p[i][1];
                    r += p[i][2];
                }
                b >>= 2;
                g >>= 2;
                r >>= 2;
                int u = ((b * 127 - r * 43 - g * 84) >> 8) + 128;
                int v = ((r * 127 - g * 107 - b * 20) >> 8) + 128;
                d[x * 2 + 0] = (uint8_t)(u < 0 ? 0 : (u > 255 ? 255 : u));
                d[x * 2 + 1] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        sum += ms;
        if (ms > worst) worst = ms;
    }
    std::printf("  %ux%u BGRA->NV12 : avg %5.2f / max %5.2f ms  (budget at 30fps: 33.3)\n", w, h,
                sum / kIters, worst);
}

void probeCodec(const char* label, REFGUID subtype, UINT32 profile) {
    std::printf("\n=== %s ===\n", label);

    auto hw = enumEncoders(subtype, true);
    // The same query without the HARDWARE flag also lists software transforms,
    // which is how HEVC Video Extensions shows up when it is installed.
    auto all = enumEncoders(subtype, false);
    std::printf("hardware encoders : %zu\n", hw.size());
    for (size_t i = 0; i < hw.size(); ++i) {
        const Encoder& e = hw[i];
        std::printf("  [%zu] %s\n        d3d11=%s async=%s clsid=%s\n", i, e.name.c_str(),
                    e.d3dAware ? "yes" : "no", e.async ? "yes" : "no",
                    e.hasClsid ? guid_str(e.clsid).c_str() : "?");
    }
    std::printf("all encoders      : %zu%s\n", all.size(),
                all.size() > hw.size() ? "  (includes software)" : "  (same set)");
    for (size_t i = 0; i < all.size(); ++i) {
        bool dup = false;
        for (const auto& h : hw) {
            if (h.name == all[i].name) dup = true;
        }
        if (!dup) std::printf("  [+] software? %s\n", all[i].name.c_str());
    }
    if (hw.empty() && all.size() <= hw.size()) {
        std::printf("  -> no encoder for this codec on this machine\n");
        return;
    }

    struct Case {
        const char* label;
        UINT32 w, h, fps, kbps;
    };
    static const Case kCases[] = {
        {"1920x1080@30   8 Mbps", 1920, 1080, 30, 8000},
        {"2560x1440@30  16 Mbps", 2560, 1440, 30, 16000},
        {"3840x2160@30  30 Mbps", 3840, 2160, 30, 30000},
        {"3840x2160@60  50 Mbps", 3840, 2160, 60, 50000},
    };

    const auto& candidates = hw.empty() ? all : hw;
    struct Strategy {
        const char* label;
        bool attachD3D;
        Feed feed;
        bool direct; // CoCreateInstance(clsid) instead of the enumerated activate
    };
    for (size_t i = 0; i < candidates.size(); ++i) {
        const Encoder& enc = candidates[i];
        std::printf("\n  encoder [%zu] %s  (d3d11=%s async=%s)\n", i, enc.name.c_str(),
                    enc.d3dAware ? "yes" : "no", enc.async ? "yes" : "no");

        // Most desirable first: a plain memory-buffer encoder drops into the
        // existing capture path with no data-flow changes at all.
        std::vector<Strategy> strategies;
        strategies.push_back({"memory", false, Feed::MemoryBuffer, false});
        if (enc.d3dAware) {
            strategies.push_back({"d3d+memory", true, Feed::MemoryBuffer, false});
            strategies.push_back({"d3d+texture", true, Feed::DxgiTexture, false});
            if (enc.hasClsid) {
                strategies.push_back({"direct+d3d+texture", true, Feed::DxgiTexture, true});
            }
        }

        if (enc.async && enc.d3dAware) {
            // Why does an unlocked activation still say MF_E_TRANSFORM_ASYNC_LOCKED?
            // Print what each object actually reports, so the next attempt is
            // based on facts rather than another guess.
            static bool diagDone = false;
            if (!diagDone) {
                diagDone = true;
                enc.activate->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                UINT32 back = 0;
                std::printf("    [diag] unlock readback=%s\n",
                            SUCCEEDED(enc.activate->GetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, &back))
                                ? (back ? "TRUE" : "FALSE")
                                : "absent");
                UINT32 isAsync = 0;
                std::printf("    [diag] async flag   =%s\n",
                            SUCCEEDED(enc.activate->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync))
                                ? (isAsync ? "TRUE" : "FALSE")
                                : "absent");
                if (enc.hasClsid) {
                    ComPtr<IUnknown> unk;
                    HRESULT chr = ::CoCreateInstance(enc.clsid, nullptr, CLSCTX_INPROC_SERVER,
                                                     IID_IUnknown, (void**)&unk);
                    std::printf("    [diag] CoCreate    =%s\n", fc::mf_error_str(chr));
                    if (SUCCEEDED(chr) && unk) {
                        IMFAttributes* a2 = nullptr;
                        IMFTransform* t2 = nullptr;
                        const HRESULT qa = unk->QueryInterface(IID_PPV_ARGS(&a2));
                        const HRESULT qt = unk->QueryInterface(IID_PPV_ARGS(&t2));
                        std::printf("    [diag] QI attrs=%s transform=%s\n",
                                    SUCCEEDED(qa) ? "ok" : "no", SUCCEEDED(qt) ? "ok" : "no");
                        if (a2) a2->Release();
                        if (t2) t2->Release();
                    }
                }
            }
        }

        for (const Case& c : kCases) {
            bool done = false;
            std::string lastWhy;
            for (const Strategy& s : strategies) {
                // Fresh instance per attempt: once a transform has committed to a
                // memory type or a device manager it will not switch back.
                ComPtr<IMFTransform> mft;
                if (s.direct) {
                    const HRESULT chr = ::CoCreateInstance(
                        enc.clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft));
                    if (FAILED(chr)) {
                        lastWhy = std::string("CoCreateInstance: ") + fc::mf_error_str(chr);
                        continue;
                    }
                    ComPtr<IMFAttributes> attrs;
                    if (SUCCEEDED(mft.As(&attrs))) attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                } else {
                    HRESULT uhr = S_OK;
                    if (enc.async) uhr = enc.activate->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                    if (FAILED(enc.activate->ActivateObject(IID_PPV_ARGS(&mft)))) {
                        lastWhy = "activate failed";
                        continue;
                    }
                    (void)uhr;
                }
                const Outcome o =
                    tryEncode(mft.Get(), subtype, profile, c.w, c.h, c.fps, c.kbps, s.attachD3D,
                              s.feed);
                mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
                if (!o.typesOk) {
                    lastWhy = o.why;
                    continue;
                }
                if (o.fedOk) {
                    const double budget = 1000.0 / c.fps;
                    std::printf(
                        "    %-24s OK via %-17s avg %5.1f / max %5.1f ms (budget %.1f) "
                        "%d nals/%d B %s\n",
                        c.label, s.label, o.msPerFrame, o.msWorst, budget, o.framesOut,
                        o.nalBytes, o.msWorst <= budget ? "REALTIME" : "TOO SLOW");
                    done = true;
                    break;
                }
                lastWhy = o.why;
            }
            if (!done) std::printf("    %-24s FAILED  %s\n", c.label, lastWhy.c_str());
        }
    }
}

} // namespace

int main() {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION))) {
        std::printf("MFStartup failed\n");
        return 1;
    }
    g_gpu.init();

    std::printf("FlowCast HEVC probe\n");
    std::printf("D3D11 video device : %s\n", g_gpu.ready ? "yes" : "NO");

    probeCodec("H.264 (control)", MFVideoFormat_H264, eAVEncH264VProfile_Base);
    probeCodec("HEVC / H.265", MFVideoFormat_HEVC, eAVEncH265VProfile_Main_420_8);
    probeCodec("HEVC ES (raw NAL, no start codes)", MFVideoFormat_HEVC_ES,
               eAVEncH265VProfile_Main_420_8);

    std::printf("\n=== Sink Writer (pipeline picks the encoder) ===\n");
    struct SinkCase {
        const char* label;
        REFGUID sub;
        REFGUID in;
        const wchar_t* path;
        UINT32 w, h, fps, kbps;
    };
    static const SinkCase kSinkCases[] = {
        {"H.264  4K30  NV12  .mp4", MFVideoFormat_H264, MFVideoFormat_NV12, L"C:\\tmp\\fc_p.mp4",
         3840, 2160, 30, 30000},
        {"HEVC   4K30  NV12  .mp4", MFVideoFormat_HEVC, MFVideoFormat_NV12, L"C:\\tmp\\fc_p.mp4",
         3840, 2160, 30, 30000},
        // MPEG-2 transport stream: a second container, so a failure cannot be
        // blamed on the MP4 sink lacking HEVC support.
        {"HEVC   4K30  NV12  .ts", MFVideoFormat_HEVC, MFVideoFormat_NV12, L"C:\\tmp\\fc_p.ts",
         3840, 2160, 30, 30000},
        {"H.264  4K30  NV12  .ts", MFVideoFormat_H264, MFVideoFormat_NV12, L"C:\\tmp\\fc_p.ts",
         3840, 2160, 30, 30000},
        {"HEVC  1080p30 NV12  .ts", MFVideoFormat_HEVC, MFVideoFormat_NV12, L"C:\\tmp\\fc_p.ts",
         1920, 1080, 30, 10000},
    };
    for (const SinkCase& c : kSinkCases) {
        std::printf("%s\n", c.label);
        sinkWriterTest(c.sub, c.in, c.path, c.w, c.h, c.fps, c.kbps);
    }

    std::printf("\n=== Frame conversion cost (h264enc's own loop) ===\n");
    benchNv12Conversion(1920, 1080);
    benchNv12Conversion(2560, 1440);
    benchNv12Conversion(3840, 2160);

    std::printf("\n");
    MFShutdown();
    ::CoUninitialize();
    return 0;
}
