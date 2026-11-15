// FlowCast Windows client - screen capture via DXGI Desktop Duplication.
//
// Desktop Duplication is the only API that gives us the whole desktop (all
// monitors combined as one) at a low, predictable latency, and it hands back
// GPU textures that the Media Foundation encoder can consume without a copy.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h> // HRESULT, used by the private tryStart() signature

// Declared by <d3d11.h>, which this header deliberately does not pull in: only
// the implementation touches D3D types. Global scope, or it would name a
// different type.
struct ID3D11Texture2D;

namespace fc {

class Nv12Converter;
class WgcCapture;

// Which API actually produced the frames. Worth surfacing: "the desktop was
// captured" means three very different things depending on the path, and the
// path is the first thing to look at when a picture is wrong.
enum class CaptureBackend {
    None,       // nothing is running
    Duplication,// DXGI Desktop Duplication (GPU texture, no copy)
    Graphics,   // Windows Graphics Capture (DWM composition)
    Gdi,        // BitBlt fallback (CPU copy of the screen)
    Synthetic,  // --test-pattern
};

// One attached display, identified the way Windows identifies it: by device
// name ("\\.\DISPLAY2"). The name follows the physical port, so it survives a
// reboot and a resolution change -- unlike an enumeration index, which moves as
// soon as a screen is unplugged.
struct DisplayInfo {
    int index = 1;        // 1-based, primary first
    std::wstring device;  // "\\.\DISPLAY1"
    int left = 0;         // desktop coordinates of this screen
    int top = 0;
    int width = 0;
    int height = 0;
    bool primary = false;
};

// Every screen that can be captured, primary first then left to right.
std::vector<DisplayInfo> list_displays();

// The display `device` names, or the primary one when the name is empty or no
// longer attached (a saved choice must not turn into "capture nothing").
DisplayInfo find_display(const std::wstring& device);

enum class PixelFormat {
    Bgra, // 4 bytes per pixel, `stride` bytes per row
    Nv12, // 1.5 bytes per pixel, `stride` == width, Y plane then interleaved UV
};

struct CapturedFrame {
    uint8_t* data = nullptr;
    PixelFormat format = PixelFormat::Bgra;
    size_t width = 0;
    size_t height = 0;
    size_t stride = 0;
    uint64_t timestampNs = 0; // mono_ns() when the frame was acquired
    bool valid = false;
};

class ScreenCapture {
public:
    ScreenCapture() = default;
    ~ScreenCapture();

    // Captures one display. `device` is a display name from list_displays();
    // empty (or a screen that is no longer attached) means the primary display.
    // `maxWidth` caps the delivered frame width; a wider desktop is filtered
    // down so a 4K panel does not push 4K into the encoder.
    bool start(size_t maxWidth = 0, const std::wstring& device = std::wstring());
    void stop();

    // Skips the desktop entirely and synthesises a moving colour-bar pattern in
    // NV12. `--test-pattern` uses it to run the encode/send path on a machine
    // whose desktop cannot be captured (a remote session, a locked screen), so
    // "the receiver shows nothing" can be told apart from "nothing was ever
    // sent". Never used by the GUI.
    bool startTestPattern(size_t width, size_t height);

    // Grabs one frame. `out.data` stays valid until the next acquireFrame().
    // Returns false when nothing new was produced (e.g. a static screen).
    bool acquireFrame(CapturedFrame& out);

    // Size of the frames handed out, i.e. after any --max-width downscale.
    size_t width() const { return m_width; }
    size_t height() const { return m_height; }

    // True when frames come out already converted to NV12 by the GPU. The
    // encoder uses this to skip its own (much slower) BGRA conversion.
    bool nv12() const { return m_conv != nullptr; }

    // Why the last acquireFrame() returned false: 0 means "nothing new" (a
    // static screen, the normal case), anything else is a real failure that
    // will not fix itself - Windows refusing the desktop (a locked screen, a
    // remote session) is 5 / 0x80070005 and never produces a frame again.
    unsigned long lastError() const { return m_lastError; }

    // The path the frames come from (see CaptureBackend).
    CaptureBackend backend() const { return m_backend; }
    static const char* backend_name(CaptureBackend b);

    // Why the last start() gave up, as "dxgi=0x80070005, wgc=0x80004005,
    // gdi=5". A single sentence in the UI cannot carry this, and without it a
    // failure on someone else's machine is a guess.
    const std::string& startDetail() const { return m_startDetail; }

private:
    struct Impl;
    // One bring-up attempt; `hr` receives the failing HRESULT.
    static bool tryStart(Impl* im, const std::wstring& device, size_t* width, size_t* height,
                         HRESULT* hr);
    // DWM-composition path, used when the duplication is refused.
    bool startWgc(size_t maxWidth, const std::wstring& device);
    bool startGdiFallback(size_t maxWidth);
    // Converts/scales one GPU frame: 1 = NV12 ready in m_impl->nv12, 0 = BGRA
    // staged in m_impl->cpu, -1 = the frame was lost.
    int stageTexture(ID3D11Texture2D* desktop);
    // Derives m_width/m_height from the native desktop size and maxWidth.
    void applyScaleLimit(size_t maxWidth);
    // Re-creates the duplication after DXGI_ERROR_ACCESS_LOST. Returns false
    // when the rebuild failed or the desktop changed size underneath us.
    bool recover();
    // Builds the GPU converter for the current geometry. Returns false (leaving
    // m_conv null) when the driver cannot do it; the CPU path stays in use.
    bool startConverter();
    // Repaints the synthetic frame in place (m_pattern).
    void fillTestPattern();
    // Appends "name=0xcode" to m_startDetail.
    void noteFailure(const char* stage, long code);

    Impl* m_impl = nullptr;
    Nv12Converter* m_conv = nullptr;
    WgcCapture* m_wgc = nullptr;
    CaptureBackend m_backend = CaptureBackend::None;
    std::string m_startDetail;
    size_t m_width = 0;
    size_t m_height = 0;
    size_t m_srcWidth = 0;
    size_t m_srcHeight = 0;
    size_t m_maxWidth = 0;      // remembered so recover() can rebuild identically
    std::wstring m_device;      // ditto: the same screen must be picked again
    uint64_t m_lastRecoverMs = 0;
    int m_convFailures = 0;     // consecutive GPU conversion failures
    bool m_sizeFrozen = false;  // rebuild changed the mode: frames are unusable
    // Where the chosen screen sits in desktop coordinates, for the GDI path.
    int m_gdiOriginX = 0;
    int m_gdiOriginY = 0;
    // 0 while capture is merely idle, the Win32/DXGI code once it is broken.
    unsigned long m_lastError = 0;
    // Synthetic source (see startTestPattern): the frame buffer and a counter
    // that drives the motion so a frozen picture is obvious on the TV.
    bool m_testPattern = false;
    std::vector<uint8_t> m_pattern;
    uint64_t m_patternFrame = 0;
};

} // namespace fc
