// Verifies the GPU BGRA -> NV12 converter and measures what it buys.
//
// Two things are checked, because both fail silently otherwise:
//   1. Colour. A wrong matrix or a wrong range does not crash, it just makes
//      the mirrored desktop look washed out; this converts known colour blocks
//      and converts them back, so a 601/709 or full/limited mix-up shows up.
//   2. Cost. The scalar loop this replaces was the reason 4K could not hold
//      30 fps, so the measured microseconds per frame are printed.
#include "src/nv12conv.h"
#include "src/h264enc.h"
#include "src/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int fails = 0;
static void check(const char* what, bool ok) {
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

// --- BT.709 limited range, the inverse of what the video processor does ------
static int clamp8(double v) {
    const int i = (int)std::lround(v);
    return i < 0 ? 0 : (i > 255 ? 255 : i);
}

static void yuvToRgb(int Y, int U, int V, int& r, int& g, int& b) {
    const double y = (Y - 16) * 255.0 / 219.0;
    const double u = (U - 128) * 255.0 / 224.0;
    const double v = (V - 128) * 255.0 / 224.0;
    r = clamp8(y + 1.5748 * v);
    g = clamp8(y - 0.1873 * u - 0.4681 * v);
    b = clamp8(y + 1.8556 * u);
}

struct Rgb {
    int r, g, b;
};

static Rgb readNv12Pixel(const uint8_t* nv12, size_t w, size_t h, size_t x, size_t y) {
    const int Y = nv12[y * w + x];
    const uint8_t* uv = nv12 + w * h;
    const size_t ci = (y / 2) * w + (x / 2) * 2;
    Rgb c{};
    yuvToRgb(Y, uv[ci], uv[ci + 1], c.r, c.g, c.b);
    return c;
}

// Mean colour of a square, so chroma subsampling on the block edges cannot
// skew a single-pixel probe.
static Rgb readNv12Block(const uint8_t* nv12, size_t w, size_t h, size_t x0, size_t y0,
                         size_t side) {
    long r = 0, g = 0, b = 0, n = 0;
    for (size_t y = y0; y < y0 + side; y += 4) {
        for (size_t x = x0; x < x0 + side; x += 4) {
            const Rgb c = readNv12Pixel(nv12, w, h, x, y);
            r += c.r;
            g += c.g;
            b += c.b;
            n++;
        }
    }
    return Rgb{(int)(r / n), (int)(g / n), (int)(b / n)};
}

struct Block {
    const char* name;
    Rgb rgb;
    int tolerance;
};

// The tolerance is per-channel and generous on the saturated primaries: 4:2:0
// chroma is quantised to 8 bits, which alone is worth ~2 levels.
static const Block kBlocks[] = {
    {"black", {0, 0, 0}, 4},        {"white", {255, 255, 255}, 4},
    {"mid grey", {128, 128, 128}, 4}, {"near black", {16, 16, 16}, 4},
    {"red", {255, 0, 0}, 8},        {"green", {0, 255, 0}, 8},
    {"blue", {0, 0, 255}, 8},       {"skin", {200, 150, 120}, 6},
};
static const int kBlockCount = (int)(sizeof(kBlocks) / sizeof(kBlocks[0]));
static const size_t kBlockSide = 64;
static const size_t kBlockStride = 96;

static ID3D11Device* createDevice(ID3D11DeviceContext** ctx) {
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    ID3D11Device* dev = nullptr;
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                         D3D11_SDK_VERSION, &dev, &got, ctx);
    if (FAILED(hr)) {
        printf("D3D11CreateDevice failed (0x%08lx)\n", (unsigned long)hr);
        return nullptr;
    }
    return dev;
}

// Fills `tex` with the colour blocks laid out along the top-left.
static void paintBlocks(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, size_t w, size_t h) {
    std::vector<uint32_t> px(w * h, 0xff202020); // dark grey background
    for (int i = 0; i < kBlockCount; i++) {
        const Rgb c = kBlocks[i].rgb;
        const uint32_t bgra = (uint32_t)c.r << 16 | (uint32_t)c.g << 8 | (uint32_t)c.b | 0xff000000u;
        const size_t x0 = 32 + (size_t)i * kBlockStride;
        for (size_t y = 32; y < 32 + kBlockSide; y++) {
            for (size_t x = x0; x < x0 + kBlockSide; x++) px[y * w + x] = bgra;
        }
    }
    ctx->UpdateSubresource(tex, 0, nullptr, px.data(), (UINT)(w * 4), 0);
}

static ID3D11Texture2D* makeBgraTexture(ID3D11Device* dev, size_t w, size_t h) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    // An input view needs a texture the video processor may read as a surface.
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &tex))) return nullptr;
    return tex;
}

// --- colour check -----------------------------------------------------------
static void checkColours(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    const size_t w = 1920, h = 1080;
    ID3D11Texture2D* src = makeBgraTexture(dev, w, h);
    if (!src) {
        check("converter: source texture created", false);
        return;
    }
    paintBlocks(ctx, src, w, h);

    fc::Nv12Converter conv;
    const bool ok = conv.init(dev, ctx, w, h, w, h);
    check("converter: init at 1920x1080", ok);
    if (!ok) {
        printf("  reason: %s\n", conv.error());
        src->Release();
        return;
    }

    std::vector<uint8_t> nv12(conv.bufferSize());
    check("converter: blit + readback", conv.convert(src) && conv.readback(nv12.data()));

    // Range check: limited-range black must land near Y=16, not Y=0. Getting
    // this wrong is what makes a mirrored desktop look grey and flat.
    const int yBlack = nv12[48 * w + 48];
    const int yWhite = nv12[48 * w + (32 + 1 * kBlockStride + 32)];
    printf("  luma: black=%d white=%d (limited range expects ~16 / ~235)\n", yBlack, yWhite);
    check("converter: black maps to Y~16 (limited range)", yBlack >= 13 && yBlack <= 19);
    check("converter: white maps to Y~235 (limited range)", yWhite >= 232 && yWhite <= 238);

    for (int i = 0; i < kBlockCount; i++) {
        const size_t x0 = 32 + (size_t)i * kBlockStride + 8;
        const Rgb got = readNv12Block(nv12.data(), w, h, x0, 40, kBlockSide - 16);
        const Rgb want = kBlocks[i].rgb;
        const int dr = std::abs(got.r - want.r);
        const int dg = std::abs(got.g - want.g);
        const int db = std::abs(got.b - want.b);
        const int worst = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
        char label[96];
        snprintf(label, sizeof(label), "converter: %s round-trips (dr=%d dg=%d db=%d tol=%d)",
                 kBlocks[i].name, dr, dg, db, kBlocks[i].tolerance);
        check(label, worst <= kBlocks[i].tolerance);
    }
    src->Release();
}

// --- throughput -------------------------------------------------------------
static double timeConvert(fc::Nv12Converter& conv, ID3D11Texture2D* src, std::vector<uint8_t>& out,
                          int iterations, bool readback, double* worstMs) {
    // Warm up: the first blit also builds the input view and the driver's
    // shaders, which is not steady-state cost.
    for (int i = 0; i < 3; i++) {
        conv.convert(src);
        if (readback) conv.readback(out.data());
    }
    double total = 0;
    *worstMs = 0;
    for (int i = 0; i < iterations; i++) {
        const auto t0 = std::chrono::steady_clock::now();
        conv.convert(src);
        if (readback) conv.readback(out.data());
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        total += ms;
        if (ms > *worstMs) *worstMs = ms;
    }
    return total / iterations;
}

static void measure(ID3D11Device* dev, ID3D11DeviceContext* ctx, const char* label, size_t sw,
                    size_t sh, size_t dw, size_t dh) {
    ID3D11Texture2D* src = makeBgraTexture(dev, sw, sh);
    if (!src) {
        printf("%-26s could not create the source texture\n", label);
        return;
    }
    paintBlocks(ctx, src, sw, sh);
    fc::Nv12Converter conv;
    if (!conv.init(dev, ctx, sw, sh, dw, dh)) {
        printf("%-26s init failed: %s\n", label, conv.error());
        src->Release();
        return;
    }
    std::vector<uint8_t> out(conv.bufferSize());
    double worst = 0;
    const double gpuMs = timeConvert(conv, src, out, 40, false, &worst);
    double worstRb = 0;
    const double fullMs = timeConvert(conv, src, out, 40, true, &worstRb);
    printf("%-26s blit %5.2f ms | blit+readback %5.2f ms (worst %.1f) | %.1f MB back\n", label,
           gpuMs, fullMs, worstRb, conv.bufferSize() / (1024.0 * 1024.0));
    src->Release();
}

// --- encoder, both input formats --------------------------------------------
static void measureEncoder(ID3D11Device* dev, ID3D11DeviceContext* ctx, size_t w, size_t h) {
    ID3D11Texture2D* src = makeBgraTexture(dev, w, h);
    if (!src) return;
    paintBlocks(ctx, src, w, h);
    fc::Nv12Converter conv;
    if (!conv.init(dev, ctx, w, h, w, h)) {
        src->Release();
        return;
    }
    std::vector<uint8_t> nv12(conv.bufferSize());
    conv.convert(src);
    conv.readback(nv12.data());

    // BGRA on its own, for the scalar conversion the old path ran per frame.
    std::vector<uint8_t> bgra(w * h * 4);
    {
        D3D11_MAPPED_SUBRESOURCE m{};
        D3D11_TEXTURE2D_DESC d{};
        src->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* stg = nullptr;
        if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &stg))) {
            ctx->CopyResource(stg, src);
            if (SUCCEEDED(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m))) {
                for (size_t y = 0; y < h; y++) {
                    memcpy(bgra.data() + y * w * 4,
                           (const uint8_t*)m.pData + y * m.RowPitch, w * 4);
                }
                ctx->Unmap(stg, 0);
            }
            stg->Release();
        }
    }

    const int fps = 30;
    const int bitrate = (int)((double)w * (double)h * fps * 0.16);
    for (int pass = 0; pass < 2; pass++) {
        fc::H264Encoder enc;
        if (!enc.start(w, h, fps, bitrate)) {
            printf("%zux%zu: encoder failed to start\n", w, h);
            src->Release();
            return;
        }
        std::vector<uint8_t> out;
        double total = 0, worst = 0;
        int produced = 0;
        const int iterations = 24;
        for (int i = 0; i < iterations + 3; i++) {
            out.clear();
            const auto t0 = std::chrono::steady_clock::now();
            const int n = pass == 0
                              ? enc.encodeFrame(bgra.data(), w, h, w * 4, 1000000ull * i, out)
                              : enc.encodeNv12(nv12.data(), w, h, 1000000ull * i, out);
            const auto t1 = std::chrono::steady_clock::now();
            if (i < 3) continue; // warm up the MFT
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            total += ms;
            if (ms > worst) worst = ms;
            if (n > 0) produced++;
        }
        const char* mode = pass == 0 ? "BGRA -> CPU convert" : "NV12 -> direct";
        printf("  %zux%zu %-20s %5.2f ms/frame (worst %.1f) %d/%d frames encoded\n", w, h, mode,
               total / iterations, worst, produced, iterations);
    }
    src->Release();
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    fc::log_init(fc::LogLevel::Warn, nullptr, nullptr);

    ID3D11DeviceContext* ctx = nullptr;
    ID3D11Device* dev = createDevice(&ctx);
    if (!dev) {
        printf("no D3D11 device, cannot test the converter\n");
        return 1;
    }
    {
        // Report what we are running on: the numbers only mean something with
        // the adapter named.
        IDXGIDevice* dxgiDev = nullptr;
        if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter) {
                DXGI_ADAPTER_DESC desc{};
                adapter->GetDesc(&desc);
                wprintf(L"adapter: %s\n", desc.Description);
                adapter->Release();
            }
            dxgiDev->Release();
        }
    }

    printf("\n--- colour ---\n");
    checkColours(dev, ctx);

    printf("\n--- converter throughput ---\n");
    measure(dev, ctx, "1920x1080 -> 1920x1080", 1920, 1080, 1920, 1080);
    measure(dev, ctx, "1920x1080 -> 1280x720", 1920, 1080, 1280, 720);
    measure(dev, ctx, "2560x1440 -> 2560x1440", 2560, 1440, 2560, 1440);
    measure(dev, ctx, "3840x2160 -> 3840x2160", 3840, 2160, 3840, 2160);

    printf("\n--- encoder, per frame (capture excluded) ---\n");
    measureEncoder(dev, ctx, 1920, 1080);
    measureEncoder(dev, ctx, 3840, 2160);

    ctx->Release();
    dev->Release();
    printf("\n%s\n", fails == 0 ? "nv12conv_test: all checks passed" : "nv12conv_test: FAILURES");
    return fails == 0 ? 0 : 1;
}
