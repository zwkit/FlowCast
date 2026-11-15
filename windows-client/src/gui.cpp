// FlowCast Windows client - the windowed front end.
//
// The GUI and the console client share one binary: wWinMain hands control to the
// console parser when anything is passed on the command line, so scripts that
// already call `flowcast --host ...` keep working unchanged.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "cli.h"
#include "capture.h"
#include "client.h"
#include "i18n.h"
#include "log.h"
#include "mdns.h"
#include "settings.h"
#include "text.h"
#include "ui.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <objbase.h> // CreateStreamOnHGlobal, for the in-memory PNG in --make-icon
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

using namespace Gdiplus;

namespace {

// --- geometry ---------------------------------------------------------------
// Logical pixels at 96 DPI; everything is multiplied by the window scale.
constexpr float kW = 480.f;
constexpr float kH = 574.f;
// One settings row. The window grows by exactly this when the "capture screen"
// row appears, so nothing else on the card has to move.
constexpr float kRowH = 52.f;

constexpr UINT WM_APP_SESSION_ENDED = WM_APP + 1;
constexpr UINT WM_APP_SCAN_ENDED = WM_APP + 2;
constexpr UINT_PTR kTickTimer = 1;
constexpr UINT kTickMs = 120;
constexpr wchar_t kClass[] = L"FlowCastClientWnd";

enum class Phase { Idle, Connecting, Mirroring, Stopping, Failed };
enum class Hit { None, Close, Minimize, Device, Quality, Screen, Sound, Language, Main, Log };
enum class Drop { None, Language, Quality, Screen, Device };

struct Device {
    std::wstring name;
    std::string host;
    uint16_t port = 0;
};

// --- state shared with the worker threads -----------------------------------
std::mutex g_mu;
Phase g_phase = Phase::Idle;
std::wstring g_error;
std::wstring g_target;
std::vector<Device> g_devices;
bool g_scanning = false;
fc::ClientStats g_stats;
bool g_hasStats = false;

std::atomic<bool> g_stopFlag{false};
std::atomic<bool> g_scanBusy{false};
std::atomic<bool> g_workerRunning{false};

fc::FlowCastClient g_client;
std::thread g_worker;
std::thread g_scan;
HWND g_hwnd = nullptr;
bool g_quitAfterStop = false;

// --- UI-thread state --------------------------------------------------------
struct Layout {
    float w = 0.f;
    float h = 0.f;
    RectF logo;
    RectF title;
    RectF closeBtn;
    RectF minBtn;
    RectF tagline;
    RectF targetLabel;
    RectF device;
    RectF group;
    RectF rows[4];
    int rowCount = 3;
    RectF mainBtn;
    RectF status;
    RectF edit;
};

struct App {
    UINT dpi = 96;
    float scale = 1.f;
    bool dark = true;
    fc::AppSettings cfg;
    fc::Lang lang = fc::Lang::ZhHans;
    const fc::Strings* T = nullptr;

    int selected = -1;      // -1 = let mDNS pick
    bool manual = false;    // the typed address is the target
    bool editing = false;   // the address box is on screen
    std::wstring manualAddr;
    std::wstring editText;  // what the in-place editor currently holds
    int editCaret = 0;      // insertion point, in characters

    Hit hot = Hit::None;
    Hit down = Hit::None;
    Drop drop = Drop::None;
    int dropHot = -1;

    // Copy of the shared block, refreshed on every tick.
    Phase phase = Phase::Idle;
    std::wstring error;
    std::wstring target;
    std::vector<Device> devices;
    // Screens that can be captured. Re-read whenever the settings page is
    // opened, so a TV plugged in after launch shows up without a restart.
    std::vector<fc::DisplayInfo> displays;
    bool scanning = false;
    fc::ClientStats stats;
    bool hasStats = false;

    Layout layout;
};

App g_app;

// --- small helpers ----------------------------------------------------------
inline float sc(float v) { return v * g_app.scale; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
// Opacity-free blend: `t` of `b` mixed into `a`, one byte per channel. Used
// where the result has to be a predictable colour on screen rather than a
// promise the compositor may keep differently.
inline Color mix(const Color& a, const Color& b, float t) {
    return Color(255, (BYTE)(a.GetR() + (b.GetR() - (int)a.GetR()) * t),
                 (BYTE)(a.GetG() + (b.GetG() - (int)a.GetG()) * t),
                 (BYTE)(a.GetB() + (b.GetB() - (int)a.GetB()) * t));
}
inline COLORREF to_rgb(const Color& c) { return RGB(c.GetR(), c.GetG(), c.GetB()); }

// Only ever set by the offscreen renderer: with no visible window nothing holds
// focus, and a caret that is never drawn cannot be reviewed.
bool g_forceCaret = false;

// The "capture screen" row only exists when there is a choice to make. On a
// single-screen PC it would be a row that can never change anything.
bool multi_screen() { return g_app.displays.size() > 1; }

int group_rows() { return multi_screen() ? 4 : 3; }

// Row order: quality, [screen], sound, language. Named instead of indexed
// because "screen" is the one row that comes and goes.
int row_quality() { return 0; }
int row_screen() { return multi_screen() ? 1 : -1; }
int row_sound() { return multi_screen() ? 2 : 1; }
int row_language() { return group_rows() - 1; }

// Window height for the current row count. Everything above the settings card
// is fixed height, so this is the only thing that moves.
float window_height() { return kH + (group_rows() - 3) * kRowH; }

// The screen that will actually be captured. Resolved against the list the
// dropdown shows, so the row and the dropdown can never disagree; an empty or
// stale choice falls back to the primary screen exactly like capture does.
fc::DisplayInfo active_display() {
    for (const fc::DisplayInfo& d : g_app.displays) {
        if (d.device == g_app.cfg.display) return d;
    }
    for (const fc::DisplayInfo& d : g_app.displays) {
        if (d.primary) return d;
    }
    if (!g_app.displays.empty()) return g_app.displays.front();
    return fc::find_display(g_app.cfg.display);
}

Layout compute_layout(float w, float h) {
    Layout L;
    L.w = w;
    L.h = h;
    const float m = sc(24.f);
    const float cw = w - 2.f * m;

    // The header carries the same mark as the launcher icon, sized to the mark's
    // 104:72 aspect so it is not squashed into the square it sits in. Height is
    // matched to the title's cap height: a mark smaller than the word it labels
    // reads as a bullet point rather than an identity.
    L.logo = RectF(m, sc(14.f), sc(23.5f), sc(16.3f));
    L.title = RectF(m + sc(34.f), 0.f, w * 0.6f, sc(44.f));
    L.closeBtn = RectF(w - sc(12.f) - sc(30.f), sc(7.f), sc(30.f), sc(30.f));
    L.minBtn = RectF(L.closeBtn.X - sc(6.f) - sc(30.f), sc(7.f), sc(30.f), sc(30.f));

    L.tagline = RectF(m, sc(52.f), cw, sc(18.f));
    L.targetLabel = RectF(m, sc(84.f), cw, sc(16.f));

    L.device = RectF(m, sc(106.f), cw, sc(62.f));
    L.edit = RectF(L.device.X + sc(50.f), L.device.Y + sc(8.f), cw - sc(78.f), sc(26.f));

    L.rowCount = group_rows();
    const float groupH = kRowH * (float)L.rowCount;
    L.group = RectF(m, sc(190.f), cw, sc(groupH));
    for (int i = 0; i < L.rowCount; ++i) {
        L.rows[i] = RectF(m, sc(190.f) + sc(kRowH) * (float)i, cw, sc(kRowH));
    }

    L.mainBtn = RectF(m, sc(190.f) + sc(groupH) + sc(20.f), cw, sc(48.f));
    L.status = RectF(m, L.mainBtn.Y + sc(70.f), cw, sc(96.f));
    return L;
}

std::wstring computer_name() {
    wchar_t buf[256] = {0};
    DWORD n = 256;
    if (!::GetComputerNameExW(ComputerNameDnsHostname, buf, &n)) return L"Windows PC";
    return buf;
}

// mDNS instance labels escape spaces and dots as \DDD; undo it so a TV named
// "客厅 电视" does not show up as "客厅\032电视".
std::wstring unescape_label(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 3 < in.size() && in[i + 1] >= '0' && in[i + 1] <= '9' &&
            in[i + 2] >= '0' && in[i + 2] <= '9' && in[i + 3] >= '0' && in[i + 3] <= '9') {
            out.push_back((char)((in[i + 1] - '0') * 100 + (in[i + 2] - '0') * 10 + (in[i + 3] - '0')));
            i += 3;
        } else {
            out.push_back(in[i]);
        }
    }
    return fc::wide_from_utf8(out);
}

std::wstring device_label(const std::string& instance) {
    std::string s = instance;
    const size_t p = s.find("._airplay._tcp");
    if (p != std::string::npos) s = s.substr(0, p);
    const std::wstring w = unescape_label(s);
    return w.empty() ? std::wstring(L"AirPlay") : w;
}

void set_phase(Phase p) {
    std::lock_guard<std::mutex> g(g_mu);
    g_phase = p;
    if (p == Phase::Idle) {
        g_hasStats = false;
        g_target.clear();
    }
}

// --- log sink: the last error line becomes the text in the status card -------
void on_log(fc::LogLevel level, const char* line, void*) {
    if (level != fc::LogLevel::Error) return;
    std::wstring s = fc::wide_from_utf8(line);
    // Drop the "[ 12.345] " stamp and the "client: " channel; neither means
    // anything to someone looking at the window.
    if (!s.empty() && s[0] == L'[') {
        const size_t close = s.find(L"] ");
        if (close != std::wstring::npos) s = s.substr(close + 2);
    }
    if (s.rfind(L"client: ", 0) == 0) s = s.substr(8);
    std::lock_guard<std::mutex> g(g_mu);
    g_error = s;
}

// Turns the log line the session failed on into something a user can act on.
// Anything unrecognised is shown verbatim rather than hidden - a vague "failed"
// is worse than a technical sentence.
std::wstring friendly_error(const std::wstring& raw) {
    const fc::Strings& T = *g_app.T;
    if (raw.empty()) return T.noLan;
    if (raw.find(L"no FlowCast receiver") != std::wstring::npos ||
        raw.find(L"none matched") != std::wstring::npos ||
        raw.find(L"browsing mDNS") != std::wstring::npos) {
        return T.noLan;
    }
    // The receiver answered nothing and hung up. This has to be tested before
    // the SETUP branch below: that one says "check the TV is on its waiting
    // page", and here the TV was waiting - it just decided to drop us.
    if (raw.find(L"(dropped)") != std::wstring::npos ||
        raw.find(L"(no reply)") != std::wstring::npos) {
        return T.errHangup;
    }
    if (raw.find(L"fp-setup") != std::wstring::npos || raw.find(L"SETUP") != std::wstring::npos ||
        raw.find(L"OPTIONS") != std::wstring::npos || raw.find(L"RECORD") != std::wstring::npos) {
        // Keep the receiver's status code on the end of the sentence. "refused"
        // The status code used to be appended here. It now gets a row of its
        // own (see error_detail) because at the end of this sentence it was the
        // first thing the card ellipsised away - the one number the reader
        // needs was the one number that never fitted.
        return T.errRefused;
    }
    if (raw.find(L"no picture from the desktop") != std::wstring::npos) {
        return T.errNoCapture;
    }
    if (raw.find(L"encoder") != std::wstring::npos) {
        return T.errEncoder;
    }
    if (raw.find(L"receiver closed") != std::wstring::npos) {
        return T.errDisconnected;
    }
    if (raw.find(L"connect") != std::wstring::npos || raw.find(L"socket") != std::wstring::npos ||
        // A typed address that does not resolve and a receiver that stops
        // answering are the same user-facing problem: nothing is there.
        raw.find(L"resolve") != std::wstring::npos ||
        raw.find(L"timeout") != std::wstring::npos ||
        raw.find(L"timed out") != std::wstring::npos) {
        return T.errNet;
    }
    return raw;
}

// The one line that actually says what went wrong, pulled out of the log line so
// it can be drawn on its own row instead of living at the end of a sentence that
// gets ellipsised away. "SETUP #1 failed (403)" becomes "SETUP #1 · 403": the
// step says whether the receiver answered at all, and the code says why, which
// is the whole difference between "it will never work" and "close the phone
// mirroring first".
std::wstring error_detail(const std::wstring& raw) {
    if (raw.empty()) return std::wstring();
    // Longest first: "SETUP #1" contains "SETUP", and "fp-setup #1" contains
    // "SETUP" too - matching the short form first would label it wrong.
    static const wchar_t* kSteps[] = {L"fp-setup #2", L"fp-setup #1", L"SETUP #2",
                                      L"SETUP #1",    L"OPTIONS",      L"RECORD"};
    const wchar_t* step = nullptr;
    size_t at = std::wstring::npos;
    for (const wchar_t* s : kSteps) {
        const size_t p = raw.find(s);
        if (p == std::wstring::npos) continue;
        if (!step || p < at) {
            step = s;
            at = p;
        }
    }
    const size_t open = raw.find(L'(');
    const size_t close = open == std::wstring::npos ? std::wstring::npos : raw.find(L')', open);
    const bool hasTail = close != std::wstring::npos && close > open + 1;
    if (step && hasTail) {
        std::wstring out = step;
        out += L"  \u00b7  ";
        out += raw.substr(open + 1, close - open - 1);
        return out;
    }
    // No status code to quote. The raw sentence is then the only information
    // there is, and it is short enough to draw.
    return raw;
}

// --- device discovery -------------------------------------------------------
void on_service(const fc::MdnsServiceInfo& info, void* user) {
    auto* out = static_cast<std::vector<Device>*>(user);
    const std::string host = info.address.empty() ? info.host : info.address;
    if (host.empty() || info.port == 0) return;
    for (const Device& d : *out) {
        if (d.host == host && d.port == info.port) return;
    }
    Device d;
    d.name = device_label(info.instance);
    d.host = host;
    d.port = info.port;
    out->push_back(std::move(d));
}

void begin_scan() {
    if (g_scanBusy.exchange(true)) return;
    if (g_scan.joinable()) g_scan.join();
    {
        std::lock_guard<std::mutex> g(g_mu);
        g_scanning = true;
        g_devices.clear();
    }
    g_scan = std::thread([]() {
        std::vector<Device> found;
        fc::MdnsBrowser browser;
        browser.start("_airplay._tcp", on_service, &found, 2200);
        {
            std::lock_guard<std::mutex> g(g_mu);
            g_devices = std::move(found);
            g_scanning = false;
        }
        g_scanBusy = false;
        ::PostMessageW(g_hwnd, WM_APP_SCAN_ENDED, 0, 0);
    });
}

// --- session ----------------------------------------------------------------
fc::ClientConfig build_config() {
    fc::ClientConfig c;
    c.name = fc::utf8_from_wide(computer_name());
    const fc::QualityPreset q = fc::quality_preset(g_app.cfg.quality);
    c.maxWidth = q.maxWidth;
    c.fps = q.fps;
    c.display = g_app.cfg.display;
    c.audio = g_app.cfg.systemAudio;
    c.video = true;
    c.discoverMs = 3000;

    if (g_app.manual && !g_app.manualAddr.empty()) {
        c.host = fc::utf8_from_wide(g_app.manualAddr);
        c.port = (uint16_t)g_app.cfg.lastPort;
    } else if (g_app.selected >= 0 && g_app.selected < (int)g_app.devices.size()) {
        c.host = g_app.devices[(size_t)g_app.selected].host;
        c.port = g_app.devices[(size_t)g_app.selected].port;
    }
    c.onProgress = [](const fc::ClientStats& s) {
        std::lock_guard<std::mutex> g(g_mu);
        g_stats = s;
        g_hasStats = true;
    };
    return c;
}

std::wstring current_target_label() {
    if (g_app.manual && !g_app.manualAddr.empty()) return g_app.manualAddr;
    if (g_app.selected >= 0 && g_app.selected < (int)g_app.devices.size()) {
        return g_app.devices[(size_t)g_app.selected].name;
    }
    return std::wstring();
}

void start_session() {
    if (g_workerRunning) return;
    if (g_worker.joinable()) g_worker.join();

    fc::ClientConfig cfg = build_config();
    const std::wstring label = current_target_label();
    {
        std::lock_guard<std::mutex> g(g_mu);
        g_phase = Phase::Connecting;
        g_error.clear();
        g_target = label;
        g_hasStats = false;
    }

    g_stopFlag = false;
    g_workerRunning = true;
    g_worker = std::thread([cfg]() {
        // start() already retries once as an Apple sender when the receiver
        // refuses the handshake, so a panel that only talks to iPhones needs no
        // flag here and no explanation on the card.
        if (!g_client.start(cfg)) {
            std::wstring err;
            {
                std::lock_guard<std::mutex> g(g_mu);
                err = g_error;
            }
            g_workerRunning = false;
            {
                std::lock_guard<std::mutex> g(g_mu);
                g_phase = Phase::Failed;
                g_error = err;
                g_hasStats = false;
            }
            ::PostMessageW(g_hwnd, WM_APP_SESSION_ENDED, 0, 0);
            return;
        }
        {
            std::lock_guard<std::mutex> g(g_mu);
            g_phase = Phase::Mirroring;
        }
        g_client.run(&g_stopFlag);
        // Read the reason before stop(): it deletes the session state, and an
        // empty answer would drop the user back to Idle with no explanation.
        const std::wstring err = g_client.error();
        g_client.stop();
        g_workerRunning = false;
        // A capture or encoder that never started ends the session by itself;
        // showing that instead of dropping silently back to Idle is the whole
        // point of the error() channel.
        if (!err.empty()) {
            std::lock_guard<std::mutex> g(g_mu);
            g_phase = Phase::Failed;
            g_error = err;
            g_hasStats = false;
        } else {
            set_phase(Phase::Idle);
        }
        ::PostMessageW(g_hwnd, WM_APP_SESSION_ENDED, 0, 0);
    });
}

void request_stop() {
    if (!g_workerRunning) return;
    g_stopFlag = true;
    std::lock_guard<std::mutex> g(g_mu);
    g_phase = Phase::Stopping;
}

// --- the address box --------------------------------------------------------
// The address is edited in place: no child EDIT control. A themed child control
// fights the hand-painted surface (background, caret, dark mode), and this field
// only ever holds an ASCII address, so a few dozen lines of WM_CHAR handling buy
// full control over how it looks.
void clamp_caret() {
    const int n = (int)g_app.editText.size();
    if (g_app.editCaret < 0) g_app.editCaret = 0;
    if (g_app.editCaret > n) g_app.editCaret = n;
}

void insert_text(const std::wstring& text) {
    if (text.empty()) return;
    clamp_caret();
    // Long past any host:port worth typing, and an unbounded string would make
    // the field's scroll arithmetic pointless.
    constexpr size_t kMax = 96;
    const size_t room = g_app.editText.size() < kMax ? kMax - g_app.editText.size() : 0;
    if (room == 0) return;
    const std::wstring take = text.size() <= room ? text : text.substr(0, room);
    g_app.editText.insert((size_t)g_app.editCaret, take);
    g_app.editCaret += (int)take.size();
}

void paste_clipboard() {
    if (!::OpenClipboard(g_hwnd)) return;
    HANDLE h = ::GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* src = (const wchar_t*)::GlobalLock(h);
        if (src) {
            std::wstring text;
            for (const wchar_t* c = src; *c; ++c) {
                const wchar_t ch = *c;
                if (ch >= 0x20 && ch != 0x7f && ch != L'\r' && ch != L'\n') text.push_back(ch);
            }
            ::GlobalUnlock(h);
            insert_text(text);
        }
    }
    ::CloseClipboard();
}

void show_edit(bool on) {
    g_app.editing = on;
    if (on) {
        g_app.manual = true;
        // Seed from what the user typed last time, never from a receiver in the
        // list: reaching for the address box means the listed ones are wrong.
        std::wstring seed = g_app.manualAddr;
        if (seed.empty()) seed = g_app.cfg.lastAddress;
        g_app.editText = seed;
        g_app.editCaret = (int)seed.size();
        ::SetFocus(g_hwnd);
    } else {
        g_app.editText.clear();
        g_app.editCaret = 0;
    }
}

void commit_edit() {
    if (!g_app.editing) return;
    std::wstring v = g_app.editText;
    while (!v.empty() && (v.front() == L' ' || v.front() == L'\t')) v.erase(v.begin());
    while (!v.empty() && (v.back() == L' ' || v.back() == L'\t')) v.pop_back();
    show_edit(false);
    if (v.empty()) {
        g_app.manual = !g_app.manualAddr.empty();
        return;
    }
    g_app.manualAddr = v;
    g_app.cfg.lastAddress = v;
    g_app.selected = -1;
    g_app.manual = true;
    fc::settings_save(g_app.cfg);
}

void cancel_edit() {
    show_edit(false);
    g_app.manual = !g_app.manualAddr.empty();
}

bool caret_visible() {
    if (g_forceCaret) return true;
    if (::GetFocus() != g_hwnd) return false;
    const DWORD ms = ::GetCaretBlinkTime();
    if (ms == 0 || ms == INFINITE) return true;
    return ((::GetTickCount64() / ms) & 1) == 0;
}

// --- text formatting --------------------------------------------------------
std::wstring quality_text(const fc::Strings& T, int q) {
    switch (q) {
    case 0: return T.q720;
    case 2: return T.q1080_60;
    case 3: return T.q1440;
    case 4: return T.qNative;
    case 1:
    default: return T.q1080;
    }
}

std::wstring duration_text(double sec) {
    const int total = sec > 0 ? (int)sec : 0;
    wchar_t buf[32];
    swprintf_s(buf, L"%02d:%02d", total / 60, total % 60);
    return buf;
}

std::wstring data_text(uint64_t bytes) {
    const double mb = (double)bytes / 1048576.0;
    wchar_t buf[32];
    if (mb < 1024.0) swprintf_s(buf, L"%.1f MB", mb);
    else swprintf_s(buf, L"%.2f GB", mb / 1024.0);
    return buf;
}

std::wstring language_text(const fc::Strings& T) {
    if (g_app.cfg.lang == fc::Lang::System) return T.langSystem;
    return fc::language_label(g_app.cfg.lang);
}

std::wstring resolution_text(const fc::DisplayInfo& d) {
    return std::to_wstring(d.width) + L"×" + std::to_wstring(d.height);
}

// The row value names the screen the way the picture will look: its resolution,
// plus a "primary" marker. The index is left to the dropdown -- on the row it
// would only push the actual information out of the window.
std::wstring screen_text(const fc::Strings& T) {
    const fc::DisplayInfo d = active_display();
    if (d.width <= 0) return L"—";
    return d.primary ? resolution_text(d) + T.screenMain : resolution_text(d);
}

// --- dropdown model ---------------------------------------------------------
struct DropItem {
    std::wstring label;
    std::wstring sub;
    bool checked = false;
    bool separator = false;
};

float drop_item_h() { return sc(44.f); }

std::vector<DropItem> build_items(Drop d) {
    const fc::Strings& T = *g_app.T;
    std::vector<DropItem> items;
    if (d == Drop::Language) {
        const fc::Lang order[fc::kLangCount] = {fc::Lang::System, fc::Lang::ZhHans,
                                                fc::Lang::ZhHant, fc::Lang::En};
        for (const fc::Lang l : order) {
            DropItem it;
            if (l == fc::Lang::System) {
                it.label = T.langSystem;
                it.sub = fc::language_label(fc::system_language());
            } else {
                it.label = fc::language_label(l);
            }
            it.checked = g_app.cfg.lang == l;
            items.push_back(it);
        }
    } else if (d == Drop::Quality) {
        for (int q = 0; q < fc::kQualityCount; ++q) {
            DropItem it;
            it.label = quality_text(T, q);
            it.checked = g_app.cfg.quality == q;
            items.push_back(it);
        }
    } else if (d == Drop::Screen) {
        // Numbered like the Windows display settings page, so "2" here is the
        // same "2" the user just picked over there.
        const std::wstring active = active_display().device;
        for (const fc::DisplayInfo& dv : g_app.displays) {
            DropItem it;
            it.label = std::to_wstring(dv.index) + L" · " + resolution_text(dv) +
                       (dv.primary ? T.screenMain : L"");
            it.checked = dv.device == active;
            items.push_back(it);
        }
    } else if (d == Drop::Device) {
        // No "automatic" entry: with a TV and a phone both advertising AirPlay,
        // letting the browser pick whichever answers first is how a mirror ends
        // up on the wrong screen. pull_state() preselects the first one instead.
        for (size_t i = 0; i < g_app.devices.size(); ++i) {
            const Device& dv = g_app.devices[i];
            DropItem it;
            it.label = dv.name;
            it.sub = fc::wide_from_utf8(dv.host) + L":" + std::to_wstring((unsigned)dv.port);
            it.checked = !g_app.manual && g_app.selected == (int)i;
            items.push_back(it);
        }

        DropItem manual;
        manual.label = T.manualEntry;
        manual.separator = g_app.devices.size() > 0;
        manual.checked = g_app.manual && !g_app.manualAddr.empty();
        items.push_back(manual);

        DropItem refresh;
        refresh.label = T.refresh;
        items.push_back(refresh);
    }
    return items;
}

RectF drop_anchor(const Layout& L, Drop d) {
    switch (d) {
    case Drop::Language: return L.rows[row_language()];
    case Drop::Quality: return L.rows[row_quality()];
    case Drop::Screen: return row_screen() >= 0 ? L.rows[row_screen()] : RectF();
    case Drop::Device: return L.device;
    default: return RectF();
    }
}

RectF drop_rect(const Layout& L, Drop d, int count) {
    const RectF anchor = drop_anchor(L, d);
    const float pad = sc(6.f);
    const float h = (float)count * drop_item_h() + pad * 2.f;
    float y = anchor.Y + anchor.Height + sc(5.f);
    if (y + h > L.h - sc(10.f)) y = anchor.Y - sc(5.f) - h;
    if (y < sc(10.f)) y = sc(10.f);
    return RectF(anchor.X, y, anchor.Width, h);
}

int drop_hit_test(const Layout& L, POINT pt) {
    const Drop d = g_app.drop;
    if (d == Drop::None) return -1;
    const std::vector<DropItem> items = build_items(d);
    if (items.empty()) return -1;
    const RectF box = drop_rect(L, d, (int)items.size());
    if (pt.x < (int)box.X || pt.x > (int)(box.X + box.Width)) return -1;
    const int idx = (int)((pt.y - (box.Y + sc(6.f))) / drop_item_h());
    if (idx < 0 || idx >= (int)items.size()) return -1;
    return idx;
}

// --- painting ---------------------------------------------------------------
void paint_chrome(Graphics& g, const Layout& L, const ui::Theme& th) {
    ui::icon_app_logo_mono(g, L.logo, th.accent, th.bgTop);
    ui::draw_text(g, L"FlowCast", L.title, ui::FontRole::Title, th.text, ui::AlignH::Left,
                  ui::AlignV::Middle);

    // Both window buttons share one interaction ramp, because they are one
    // control that happens to come in two variants. Only the hue differs:
    // closing is destructive, so its wash and glyph go red, while minimising
    // stays neutral. Before, minimise got a barely visible wash and kept its
    // grey glyph while close turned red and strong - hovering the two felt like
    // two different levers.
    //
    // Ramp: idle = no wash, faint glyph. Hover = wash, glyph goes to full
    // strength (danger for close). Pressed = the same wash, darker, and the
    // glyph stays put so the button reads as held rather than re-hovered.
    auto win_button = [&](const RectF& r, Hit h, bool close) {
        const bool hot = g_app.hot == h;
        const bool held = hot && g_app.down == h;
        if (hot) {
            // Mixed here rather than drawn with an alpha, because GDI+ blends
            // in linear light: a 10% alpha wash comes out around 28% on screen
            // and turns the button into a grey slab. An explicit colour also
            // makes the two buttons provably identical - same mix, same weight -
            // and only the hue differs.
            ui::fill_round(g, r, sc(8.f),
                           mix(th.bgTop, close ? th.danger : th.text, held ? 0.24f : 0.14f));
        }
        const Color fg = !hot ? th.textDim : (close ? th.danger : th.text);
        // The glyph is drawn on a 24-unit grid; a 14px box shrank the X to under
        // 5px, noticeably lighter than every other mark in the window. A 24px
        // box centred in the 30px button puts the cross at 8px and the minus at
        // 9px - the size these glyphs are meant to be.
        const float gs = sc(24.f);
        const RectF inner(r.X + (r.Width - gs) / 2.f, r.Y + (r.Height - gs) / 2.f, gs, gs);
        if (close) ui::icon_close(g, inner, fg, sc(1.5f));
        else ui::icon_minus(g, inner, fg, sc(1.5f));
    };
    win_button(L.minBtn, Hit::Minimize, false);
    win_button(L.closeBtn, Hit::Close, true);
}

void paint_device_card(Graphics& g, const Layout& L, const ui::Theme& th) {
    const fc::Strings& T = *g_app.T;
    const bool hot = g_app.hot == Hit::Device;
    ui::fill_round(g, L.device, sc(12.f), hot ? th.cardHover : th.card);
    // The accent outline is the only thing telling the user this card is now a
    // text field: the box itself is intentionally borderless so it reads as the
    // name being edited in place.
    ui::stroke_round(g, L.device, sc(12.f),
                     g_app.editing ? th.accent : (hot ? th.borderStrong : th.border),
                     sc(g_app.editing ? 1.6f : 1.f));

    std::wstring name;
    std::wstring sub;
    Color titleColor = th.text;
    Color iconColor = th.textDim;

    const bool haveAuto = !g_app.devices.empty();
    if (g_app.scanning) {
        name = T.scanning;
        iconColor = th.accent;
    } else if (g_app.manual && !g_app.manualAddr.empty()) {
        name = g_app.manualAddr;
        sub = L"AirPlay :" + std::to_wstring((unsigned)g_app.cfg.lastPort);
        iconColor = th.accent;
    } else if (g_app.selected >= 0 && g_app.selected < (int)g_app.devices.size()) {
        const Device& d = g_app.devices[(size_t)g_app.selected];
        name = d.name;
        sub = fc::wide_from_utf8(d.host) + L":" + std::to_wstring((unsigned)d.port);
        iconColor = th.accent;
    } else if (haveAuto) {
        name = T.autoReceiver;
        sub = std::to_wstring((unsigned long long)g_app.devices.size()) + T.deviceSuffix;
        iconColor = th.accent;
    } else {
        name = T.noReceiver;
        sub = T.noLan;
        titleColor = th.textDim;
        iconColor = th.danger;
    }

    const float iconSize = sc(20.f);
    const RectF ib(L.device.X + sc(16.f), L.device.Y + (L.device.Height - iconSize) / 2.f, iconSize,
                   iconSize);
    ui::icon_tv(g, ib, iconColor, sc(1.4f));

    const float textW = L.device.Width - sc(94.f);
    if (g_app.editing) {
        // The value sits on the title line, exactly where the device name was, so
        // the card keeps its shape and only the caret signals the mode change.
        const RectF line(L.device.X + sc(50.f), L.device.Y + sc(9.f), textW, sc(22.f));
        if (g_app.editText.empty()) {
            ui::draw_text(g, T.manualHint, line, ui::FontRole::Strong, th.textFaint,
                          ui::AlignH::Left, ui::AlignV::Middle);
        } else {
            const std::wstring head = g_app.editText.substr(0, (size_t)g_app.editCaret);
            const float caretX = ui::text_width(g, head, ui::FontRole::Strong);
            // Scroll the string instead of pushing the caret out of the card.
            const float edge = line.Width - sc(4.f);
            const float shift = caretX > edge ? caretX - edge : 0.f;
            const GraphicsState saved = g.Save();
            g.SetClip(RectF(line.X - sc(2.f), line.Y - sc(3.f), line.Width + sc(4.f),
                            line.Height + sc(6.f)));
            ui::draw_text(g, g_app.editText,
                          RectF(line.X - shift, line.Y, line.Width, line.Height),
                          ui::FontRole::Strong, th.text, ui::AlignH::Left, ui::AlignV::Middle);
            g.Restore(saved);
            if (caret_visible()) {
                SolidBrush caretBrush(th.accent);
                g.FillRectangle(&caretBrush,
                                RectF(line.X - shift + caretX, line.Y + sc(4.f), sc(1.6f),
                                      line.Height - sc(8.f)));
            }
            ui::draw_text(g, T.manualHint,
                          RectF(L.device.X + sc(50.f), L.device.Y + sc(34.f), textW, sc(17.f)),
                          ui::FontRole::Small, th.textFaint, ui::AlignH::Left, ui::AlignV::Middle);
        }
    } else {
        ui::draw_text(g, name, RectF(L.device.X + sc(50.f), L.device.Y + sc(9.f), textW, sc(22.f)),
                      ui::FontRole::Strong, titleColor, ui::AlignH::Left, ui::AlignV::Middle);
        if (!sub.empty()) {
            ui::draw_text(g, sub,
                          RectF(L.device.X + sc(50.f), L.device.Y + sc(34.f), textW, sc(17.f)),
                          ui::FontRole::Small, th.textFaint, ui::AlignH::Left, ui::AlignV::Middle);
        }
    }

    const RectF chv(L.device.X + L.device.Width - sc(36.f),
                    L.device.Y + (L.device.Height - sc(18.f)) / 2.f, sc(18.f), sc(18.f));
    if (g_app.scanning) {
        const GraphicsState saved = g.Save();
        const float cx = chv.X + chv.Width / 2.f;
        const float cy = chv.Y + chv.Height / 2.f;
        g.TranslateTransform(cx, cy);
        g.RotateTransform((float)std::fmod((double)::GetTickCount64() / 6.0, 360.0));
        g.TranslateTransform(-cx, -cy);
        ui::icon_refresh(g, chv, th.accent, sc(1.6f));
        g.Restore(saved);
    } else if (!g_app.editing) {
        ui::icon_chevron(g, chv, th.textFaint, sc(1.6f), false);
    }
}

void paint_group(Graphics& g, const Layout& L, const ui::Theme& th) {
    const fc::Strings& T = *g_app.T;
    ui::fill_round(g, L.group, sc(12.f), th.card);
    ui::stroke_round(g, L.group, sc(12.f), th.border, sc(1.f));

    auto row_hot = [&](int i, Hit h) {
        if (g_app.hot != h) return;
        const RectF& r = L.rows[i];
        ui::fill_round(g, RectF(r.X + sc(4.f), r.Y + sc(4.f), r.Width - sc(8.f), r.Height - sc(8.f)),
                       sc(8.f), th.cardHover);
    };

    // A row that ends in a chevron: icon, label, right-aligned value. Three of
    // the four rows are exactly this shape, and they have to stay pixel-aligned.
    auto value_row = [&](int idx, Hit h, void (*icon)(Graphics&, const RectF&, const Color&, float),
                         const std::wstring& label, const std::wstring& value) {
        row_hot(idx, h);
        const RectF& r = L.rows[idx];
        const float ic = sc(20.f);
        icon(g, RectF(r.X + sc(16.f), r.Y + (r.Height - ic) / 2.f, ic, ic), th.textDim, sc(1.5f));
        ui::draw_text(g, label, RectF(r.X + sc(50.f), r.Y, r.Width * 0.5f, r.Height),
                      ui::FontRole::Body, th.text, ui::AlignH::Left, ui::AlignV::Middle);
        ui::draw_text(g, value,
                      RectF(r.X + r.Width - sc(48.f) - sc(160.f), r.Y, sc(160.f), r.Height),
                      ui::FontRole::Body, th.textDim, ui::AlignH::Right, ui::AlignV::Middle);
        const RectF cv(r.X + r.Width - sc(34.f), r.Y + (r.Height - sc(18.f)) / 2.f, sc(18.f),
                       sc(18.f));
        ui::icon_chevron(g, cv, th.textFaint, sc(1.6f), false);
    };

    // separator lines first, highlight fills land on top of them
    for (int i = 1; i < L.rowCount; ++i) {
        Pen pen(th.separator, sc(1.f));
        g.DrawLine(&pen, L.group.X + sc(16.f), L.rows[i].Y, L.group.X + L.group.Width - sc(16.f),
                   L.rows[i].Y);
    }

    value_row(row_quality(), Hit::Quality, ui::icon_levels, T.quality,
              quality_text(T, g_app.cfg.quality));
    if (row_screen() >= 0) {
        value_row(row_screen(), Hit::Screen, ui::icon_screen, T.screen, screen_text(T));
    }

    // --- sound: a switch, not a value ----------------------------------------
    row_hot(row_sound(), Hit::Sound);
    {
        const RectF& r = L.rows[row_sound()];
        const float ic = sc(20.f);
        const bool on = g_app.cfg.systemAudio;
        ui::icon_speaker(g, RectF(r.X + sc(16.f), r.Y + (r.Height - ic) / 2.f, ic, ic),
                         on ? th.accent : th.textDim, sc(1.5f), on);
        ui::draw_text(g, T.sound, RectF(r.X + sc(50.f), r.Y, r.Width * 0.5f, r.Height),
                      ui::FontRole::Body, th.text, ui::AlignH::Left, ui::AlignV::Middle);

        const float tw = sc(42.f);
        const float thh = sc(24.f);
        const RectF track(r.X + r.Width - sc(16.f) - tw, r.Y + (r.Height - thh) / 2.f, tw, thh);
        ui::fill_round(g, track, thh / 2.f, on ? th.accent : th.track);
        const float knob = sc(9.f);
        const float kx = on ? track.X + track.Width - sc(3.f) - knob * 2.f : track.X + sc(3.f);
        ui::fill_round(g, RectF(kx, track.Y + sc(3.f), knob * 2.f, knob * 2.f), knob, th.knob);

        ui::draw_text(g, on ? T.soundOn : T.soundOff,
                      RectF(track.X - sc(130.f), r.Y, sc(120.f), r.Height), ui::FontRole::Small,
                      th.textFaint, ui::AlignH::Right, ui::AlignV::Middle);
    }

    value_row(row_language(), Hit::Language, ui::icon_globe, T.language, language_text(T));
}

void paint_main_button(Graphics& g, const Layout& L, const ui::Theme& th) {
    const fc::Strings& T = *g_app.T;
    const bool running = g_app.phase == Phase::Mirroring || g_app.phase == Phase::Connecting ||
                         g_app.phase == Phase::Stopping;
    const bool hot = g_app.hot == Hit::Main;

    if (running) {
        const Color fill = hot ? th.cardHover : th.card;
        ui::fill_round(g, L.mainBtn, sc(12.f), fill);
        ui::stroke_round(g, L.mainBtn, sc(12.f), th.borderStrong, sc(1.f));
    } else {
        Color fill = th.accent;
        if (hot) fill = th.accentHover;
        ui::fill_round(g, L.mainBtn, sc(12.f), fill);
    }
    if (g_app.down == Hit::Main) {
        ui::fill_round(g, L.mainBtn, sc(12.f), Color(24, 0, 0, 0));
    }

    const std::wstring label = running ? T.stop : T.start;
    const Color fg = running ? th.text : th.onAccent;
    const Color iconColor = running ? th.danger : th.onAccent;

    const float tw = ui::text_width(g, label, ui::FontRole::Button);
    const float ic = sc(17.f);
    const float gap = sc(9.f);
    const float total = ic + gap + tw;
    const float x = L.mainBtn.X + (L.mainBtn.Width - total) / 2.f;
    const float cy = L.mainBtn.Y + L.mainBtn.Height / 2.f;

    const RectF ib(x, cy - ic / 2.f, ic, ic);
    if (running) ui::icon_stop(g, ib, iconColor, sc(1.5f));
    else ui::icon_screen(g, ib, iconColor, sc(1.4f));

    ui::draw_text(g, label, RectF(x + ic + gap, L.mainBtn.Y, tw + sc(6.f), L.mainBtn.Height),
                  ui::FontRole::Button, fg, ui::AlignH::Left, ui::AlignV::Middle);
}

void paint_status(Graphics& g, const Layout& L, const ui::Theme& th) {
    const fc::Strings& T = *g_app.T;
    ui::fill_round(g, L.status, sc(12.f), th.card);
    ui::stroke_round(g, L.status, sc(12.f), th.border, sc(1.f));

    if (g_app.phase == Phase::Mirroring) {
        SolidBrush dot(th.accent);
        g.FillEllipse(&dot, RectF(L.status.X + sc(20.f), L.status.Y + sc(17.f), sc(7.f), sc(7.f)));
        ui::draw_text(g, T.mirroring,
                      RectF(L.status.X + sc(33.f), L.status.Y + sc(6.f), L.status.Width * 0.6f, sc(22.f)),
                      ui::FontRole::Small, th.accent, ui::AlignH::Left, ui::AlignV::Middle);

        // The size actually on the wire. Without it there is no way to tell a
        // quality preset that took effect from one that silently did nothing
        // because the desktop is smaller than the cap.
        if (g_app.stats.width > 0 && g_app.stats.height > 0) {
            wchar_t sizeBuf[32];
            swprintf_s(sizeBuf, L"%d×%d", g_app.stats.width, g_app.stats.height);
            ui::draw_text(g, sizeBuf,
                          RectF(L.status.X + L.status.Width * 0.4f, L.status.Y + sc(6.f),
                                L.status.Width * 0.6f - sc(20.f), sc(22.f)),
                          ui::FontRole::Small, th.textFaint, ui::AlignH::Right,
                          ui::AlignV::Middle);
        }

        const double sec = g_app.stats.elapsedSec;
        int fps = g_app.stats.fps;
        if (sec > 0.5 && g_app.stats.frames > 0) {
            fps = (int)std::lround((double)g_app.stats.frames / sec);
        }
        wchar_t fpsBuf[16];
        swprintf_s(fpsBuf, L"%d", fps);

        const std::wstring values[3] = {duration_text(sec), fpsBuf,
                                        data_text(g_app.stats.videoBytes + g_app.stats.audioBytes)};
        const wchar_t* labels[3] = {T.statTime, T.statFps, T.statData};
        const float cw = (L.status.Width - sc(40.f)) / 3.f;
        for (int i = 0; i < 3; ++i) {
            const float cx = L.status.X + sc(20.f) + cw * (float)i;
            ui::draw_text(g, values[i], RectF(cx, L.status.Y + sc(28.f), cw, sc(30.f)),
                          ui::FontRole::Stat, th.text, ui::AlignH::Left, ui::AlignV::Middle);
            ui::draw_text(g, labels[i], RectF(cx, L.status.Y + sc(60.f), cw, sc(18.f)),
                          ui::FontRole::Small, th.textFaint, ui::AlignH::Left, ui::AlignV::Middle);
        }
        return;
    }

    // idle / connecting / stopping / failed all share the icon + two lines layout
    std::wstring title;
    std::wstring sub;
    std::wstring detail;
    Color titleColor = th.text;
    Color iconColor = th.accent;
    bool spin = false;
    bool alert = false;

    switch (g_app.phase) {
    case Phase::Connecting:
        title = T.connecting;
        sub = g_app.target;
        spin = true;
        break;
    case Phase::Stopping:
        title = T.stopping;
        spin = true;
        break;
    case Phase::Failed:
        title = T.failed;
        sub = friendly_error(g_app.error);
        // Shown on its own row, above the advice sentence: the advice is
        // boilerplate and can be ellipsised, the step and status code cannot.
        detail = error_detail(g_app.error);
        titleColor = th.danger;
        iconColor = th.danger;
        alert = true;
        break;
    case Phase::Idle:
    default:
        if (g_app.devices.empty() && !g_app.manual) {
            // The device card already says "no receiver found"; repeating it
            // here would waste the card. This one carries the next step.
            title = T.readyHint;
            sub = T.manualEscape;
            titleColor = th.textDim;
            iconColor = th.accent;
        } else {
            title = T.ready;
            sub = T.readyHint;
            iconColor = th.accent;
        }
        break;
    }

    const float ic = sc(24.f);
    const RectF ib(L.status.X + sc(20.f), L.status.Y + (L.status.Height - ic) / 2.f, ic, ic);
    if (spin) {
        const GraphicsState saved = g.Save();
        const float cx = ib.X + ib.Width / 2.f;
        const float cy = ib.Y + ib.Height / 2.f;
        g.TranslateTransform(cx, cy);
        g.RotateTransform((float)std::fmod((double)::GetTickCount64() / 5.0, 360.0));
        g.TranslateTransform(-cx, -cy);
        ui::icon_refresh(g, ib, iconColor, sc(1.7f));
        g.Restore(saved);
    } else if (alert) {
        ui::icon_alert(g, ib, iconColor, sc(1.5f));
    } else if (g_app.devices.empty() && !g_app.manual) {
        ui::icon_tv(g, ib, iconColor, sc(1.5f));
    } else {
        ui::icon_check(g, ib, iconColor, sc(1.8f));
    }

    const float tx = ib.X + ic + sc(16.f);
    const float tw = L.status.X + L.status.Width - tx - sc(18.f);
    ui::draw_text(g, title, RectF(tx, L.status.Y + sc(22.f), tw, sc(22.f)), ui::FontRole::Strong,
                  titleColor, ui::AlignH::Left, ui::AlignV::Middle);
    if (!detail.empty() && !sub.empty()) {
        ui::draw_text(g, detail, RectF(tx, L.status.Y + sc(45.f), tw, sc(16.f)), ui::FontRole::Small,
                      titleColor, ui::AlignH::Left, ui::AlignV::Middle);
        ui::draw_text(g, sub, RectF(tx, L.status.Y + sc(68.f), tw, sc(17.f)), ui::FontRole::Small,
                      th.textFaint, ui::AlignH::Left, ui::AlignV::Middle);
    } else if (!sub.empty()) {
        ui::draw_text(g, sub, RectF(tx, L.status.Y + sc(46.f), tw, sc(18.f)), ui::FontRole::Small,
                      th.textFaint, ui::AlignH::Left, ui::AlignV::Middle);
    }
    // The log link rides on the title row, where there is nothing else: sharing
    // the row with the message truncated every capture error in Chinese.
    if (g_app.phase == Phase::Failed) {
        ui::draw_text(g, T.logLink, RectF(tx, L.status.Y + sc(22.f), tw, sc(22.f)),
                      ui::FontRole::Small, th.accent, ui::AlignH::Right, ui::AlignV::Middle);
    }
}

void paint_dropdown(Graphics& g, const Layout& L, const ui::Theme& th) {
    const Drop d = g_app.drop;
    if (d == Drop::None) return;
    const std::vector<DropItem> items = build_items(d);
    if (items.empty()) return;

    const RectF box = drop_rect(L, d, (int)items.size());
    const float ih = drop_item_h();
    const float pad = sc(6.f);

    // A soft stack of translucent rounds: cheaper than a real blur and reads the
    // same on both themes.
    const BYTE shade = g_app.dark ? 12 : 18;
    for (int i = 6; i >= 1; --i) {
        const float off = sc(1.1f) * (float)i;
        ui::fill_round(g, RectF(box.X - off * 0.5f, box.Y + off * 0.8f, box.Width + off,
                                box.Height - off * 0.8f),
                       sc(13.f), Color(shade, 0, 0, 0));
    }
    ui::fill_round(g, box, sc(12.f), th.card);
    ui::stroke_round(g, box, sc(12.f), th.borderStrong, sc(1.f));

    float y = box.Y + pad;
    for (int i = 0; i < (int)items.size(); ++i) {
        const RectF r(box.X + sc(5.f), y, box.Width - sc(10.f), ih);
        if (items[(size_t)i].separator) {
            Pen pen(th.separator, sc(1.f));
            g.DrawLine(&pen, box.X + sc(14.f), y, box.X + box.Width - sc(14.f), y);
        }
        if (g_app.dropHot == i) ui::fill_round(g, r, sc(8.f), th.cardHover);

        if (items[(size_t)i].checked) {
            const RectF ck(r.X + sc(9.f), r.Y + (ih - sc(17.f)) / 2.f, sc(17.f), sc(17.f));
            ui::icon_check(g, ck, th.accent, sc(1.8f));
        }

        const float tx = r.X + sc(34.f);
        const float tw = r.Width - sc(34.f) - sc(8.f);
        if (items[(size_t)i].sub.empty()) {
            ui::draw_text(g, items[(size_t)i].label, RectF(tx, r.Y, tw, ih), ui::FontRole::Body,
                          th.text, ui::AlignH::Left, ui::AlignV::Middle);
        } else {
            ui::draw_text(g, items[(size_t)i].label, RectF(tx, r.Y + sc(7.f), tw, sc(20.f)),
                          ui::FontRole::Body, th.text, ui::AlignH::Left, ui::AlignV::Middle);
            ui::draw_text(g, items[(size_t)i].sub, RectF(tx, r.Y + sc(26.f), tw, sc(16.f)),
                          ui::FontRole::Small, th.textFaint, ui::AlignH::Left, ui::AlignV::Middle);
        }
        y += ih;
    }
}

void paint(Graphics& g) {
    const Layout& L = g_app.layout;
    const ui::Theme& th = ui::theme(g_app.dark);

    ui::fill_round_vertical(g, RectF(0.f, 0.f, L.w, L.h), 0.f, th.bgTop, th.bgBottom);

    paint_chrome(g, L, th);
    ui::draw_text(g, g_app.T->tagline, L.tagline, ui::FontRole::Body, th.textDim,
                  ui::AlignH::Left, ui::AlignV::Middle);
    ui::draw_text(g, g_app.T->targetLabel, L.targetLabel, ui::FontRole::Small, th.textFaint,
                  ui::AlignH::Left, ui::AlignV::Middle);

    paint_device_card(g, L, th);
    paint_group(g, L, th);
    paint_main_button(g, L, th);
    paint_status(g, L, th);
    paint_dropdown(g, L, th);
}

void set_language(fc::Lang l, bool persist = true);

// --- visual regression hook -------------------------------------------------
// `--shot=<file.png>` renders the window to a bitmap and exits instead of going
// interactive. It exists so layout and typography can be checked without a
// human in front of the screen; nothing in the UI path depends on it.
bool png_encoder(CLSID* out) {
    UINT count = 0;
    UINT bytes = 0;
    if (GetImageEncodersSize(&count, &bytes) != Ok || bytes == 0) return false;
    std::vector<BYTE> buf(bytes);
    auto* info = reinterpret_cast<ImageCodecInfo*>(buf.data());
    if (GetImageEncoders(count, bytes, info) != Ok) return false;
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(info[i].MimeType, L"image/png") == 0) {
            *out = info[i].Clsid;
            return true;
        }
    }
    return false;
}

void apply_shot_state(const std::wstring& state) {
    if (state == L"mirror") {
        g_app.phase = Phase::Mirroring;
        g_app.stats.elapsedSec = 132.0;
        g_app.stats.frames = 3960;
        g_app.stats.fps = 30;
        g_app.stats.videoBytes = 41ull * 1048576ull;
        g_app.stats.audioBytes = 3ull * 1048576ull;
        g_app.stats.width = 1920;
        g_app.stats.height = 1080;
        g_app.scanning = false;
        if (g_app.devices.empty()) {
            Device d;
            d.name = L"\u5ba2\u5385\u7684\u7535\u89c6";
            d.host = "192.168.0.201";
            d.port = 7000;
            g_app.devices.push_back(d);
        }
    } else if (state == L"failed") {
        g_app.phase = Phase::Failed;
        g_app.error = L"client: no FlowCast receiver found on this network";
        g_app.scanning = false;
    } else if (state == L"failed-capture") {
        // The long form a real capture failure produces, including the codes:
        // the friendly sentence + "Open log" link only fit if the row is sized
        // for the worst case.
        g_app.phase = Phase::Failed;
        g_app.error =
            L"client: no picture from the desktop - capture could not start (dxgi=0x80070005, "
            L"wgc=0x80004005, gdi=0x00000005)";
        g_app.scanning = false;
    } else if (state == L"failed-encoder") {
        g_app.phase = Phase::Failed;
        g_app.error = L"client: the H.264 encoder would not start - no usable encoder MFT";
        g_app.scanning = false;
    } else if (state == L"connecting") {
        g_app.phase = Phase::Connecting;
        g_app.target = L"\u5ba2\u5385\u7684\u7535\u89c6";
        g_app.scanning = false;
    } else if (state == L"idle-none") {
        g_app.phase = Phase::Idle;
        g_app.scanning = false;
        g_app.devices.clear();
        g_app.selected = -1;
        g_app.manual = false;
    } else if (state == L"edit" || state == L"edit-long") {
        g_app.scanning = false;
        g_app.selected = -1;
        g_app.manual = true;
        g_app.editing = true;
        g_app.editText =
            state == L"edit" ? L"192.168.0.201"
                             : L"flowcast-receiver-in-the-living-room-upstairs.local:7000";
        g_app.editCaret = (int)g_app.editText.size() - (state == L"edit" ? 3 : 0);
        g_forceCaret = true; // no window has focus offscreen, but the caret matters
    } else if (state == L"lang") {
        g_app.scanning = false;
        g_app.drop = Drop::Language;
    } else if (state == L"quality") {
        g_app.scanning = false;
        g_app.drop = Drop::Quality;
    } else if (state == L"screens") {
        g_app.scanning = false;
        g_app.drop = Drop::Screen;
    } else if (state == L"devices") {
        g_app.scanning = false;
        g_app.devices.clear();
        Device a;
        a.name = L"\u5ba2\u5385\u7684\u7535\u89c6";
        a.host = "192.168.0.201";
        a.port = 7000;
        g_app.devices.push_back(a);
        Device b;
        b.name = L"Bedroom TV";
        b.host = "192.168.0.153";
        b.port = 7000;
        g_app.devices.push_back(b);
        g_app.drop = Drop::Device;
    } else if (state.rfind(L"hot-", 0) == 0 || state.rfind(L"down-", 0) == 0) {
        // `--shot-state=hot-close` / `down-min` / ... Offscreen rendering has no
        // cursor, so hover and press can only be reviewed by asserting them.
        // They exist so the two window buttons can be compared against each
        // other instead of trusted: same wash, same lift, only the hue differs.
        const bool held = state.rfind(L"down-", 0) == 0;
        g_app.hot = state.substr(held ? 5 : 4) == L"close" ? Hit::Close : Hit::Minimize;
        if (held) g_app.down = g_app.hot;
        g_app.scanning = false;
    }
}

// `--shot-screens=N` pretends N screens are attached, so both variants of the
// settings card (with and without the "capture screen" row) can be rendered on
// a single-monitor PC. Nothing outside the shot path calls this.
void seed_fake_displays(int count) {
    g_app.displays.clear();
    for (int i = 0; i < count; i++) {
        fc::DisplayInfo d;
        d.index = i + 1;
        d.device = L"\\\\.\\DISPLAY" + std::to_wstring(i + 1);
        d.primary = (i == 0);
        d.width = i == 0 ? 1920 : 3840;
        d.height = i == 0 ? 1080 : 2160;
        d.left = i == 0 ? 0 : 1920;
        g_app.displays.push_back(d);
    }
}

bool save_shot(const std::wstring& path, const std::wstring& state, const std::wstring& lang) {
    // persist = false: rendering a preview must not rewrite the user's choice.
    if (lang == L"en") set_language(fc::Lang::En, false);
    else if (lang == L"zh-hant") set_language(fc::Lang::ZhHant, false);
    else if (lang == L"zh-hans") set_language(fc::Lang::ZhHans, false);
    apply_shot_state(state);
    const Layout& L = g_app.layout;
    Bitmap bmp((INT)L.w, (INT)L.h, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        ui::prepare(g);
        paint(g);
    }
    CLSID png;
    if (!png_encoder(&png)) return false;
    return bmp.Save(path.c_str(), &png, nullptr) == Ok;
}

// --- icons ------------------------------------------------------------------
// The launcher icon matches the Android app: a white rounded card carrying the
// two-panel mark. Deliberately theme-independent - an app icon that changes
// colour with the desktop theme stops being an identity.
void render_app_icon(Bitmap& bmp) {
    const int px = (int)bmp.GetWidth();
    Graphics g(&bmp);
    ui::prepare(g);
    g.SetCompositingMode(CompositingModeSourceCopy);
    g.Clear(Color(0, 0, 0, 0));
    g.SetCompositingMode(CompositingModeSourceOver);
    ui::fill_round(g, RectF(0.f, 0.f, (float)px, (float)px), px * 0.23f, Color(255, 255, 255, 255));
    // 68% of the square keeps the mark legible at 16 px while leaving a margin
    // the Android adaptive-icon safe area would have provided implicitly. At
    // 16-24 px the back panel is barely a pixel wide, so the mark grows instead
    // of dissolving - the same optical sizing font designers do.
    const float share = px <= 24 ? 0.82f : 0.68f;
    const float markW = px * share;
    const float markH = markW * 72.f / 104.f;
    ui::icon_app_logo(g, RectF((px - markW) / 2.f, (px - markH) / 2.f, markW, markH));
}

HICON make_app_icon(int px) {
    Bitmap bmp(px, px, PixelFormat32bppARGB);
    render_app_icon(bmp);
    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    return icon;
}

HICON load_app_icon(int px) {
    // The exe carries the icon as a resource (res/flowcast.rc). Loading it here
    // means the title bar, taskbar, Alt-Tab and Explorer all show one drawing;
    // the painted fallback covers a build made without rc.exe.
    HICON icon = (HICON)::LoadImageW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                     px, px, LR_DEFAULTCOLOR);
    return icon ? icon : make_app_icon(px);
}

// Encode one bitmap as PNG in memory. Used for the large icon sizes: a 256x256
// DIB is 270 KB, the same drawing as PNG is a few KB, and the whole icon set
// ends up inside the exe.
bool png_blob(Bitmap& bmp, std::vector<BYTE>& out) {
    CLSID clsid;
    if (!png_encoder(&clsid)) return false;
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) != S_OK) return false;
    bool ok = bmp.Save(stream, &clsid, nullptr) == Ok;
    if (ok) {
        STATSTG st = {};
        if (stream->Stat(&st, STATFLAG_NONAME) == S_OK && st.cbSize.QuadPart > 0) {
            HGLOBAL h = nullptr;
            if (GetHGlobalFromStream(stream, &h) == S_OK && h) {
                const void* p = ::GlobalLock(h);
                if (p) {
                    out.assign((const BYTE*)p, (const BYTE*)p + (size_t)st.cbSize.QuadPart);
                    ::GlobalUnlock(h);
                    ok = !out.empty();
                } else {
                    ok = false;
                }
            } else {
                ok = false;
            }
        } else {
            ok = false;
        }
    }
    stream->Release();
    return ok;
}

// `--make-icon=<file.ico>` writes the multi-size icon that gets compiled into
// the exe by rc.exe. Generating it here instead of shipping a hand-drawn asset
// keeps one source of truth: the same drawing code paints the window icon, the
// taskbar icon and the file icon, so they can never drift apart.
bool write_ico(const std::wstring& path) {
    static const int kSizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256};
    const int count = (int)(sizeof(kSizes) / sizeof(kSizes[0]));

    std::vector<std::vector<BYTE>> blobs;
    for (int i = 0; i < count; ++i) {
        const int s = kSizes[i];
        Bitmap bmp(s, s, PixelFormat32bppARGB);
        render_app_icon(bmp);
        if (s >= 128) {
            std::vector<BYTE> png;
            if (!png_blob(bmp, png)) return false;
            blobs.push_back(std::move(png));
            continue;
        }
        Rect rc(0, 0, s, s);
        BitmapData bd = {};
        if (bmp.LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) return false;
        const int maskStride = ((s + 31) / 32) * 4;
        const size_t pixels = (size_t)s * 4u * (size_t)s;
        const size_t maskBytes = (size_t)maskStride * (size_t)s;
        std::vector<BYTE> blob(40 + pixels + maskBytes, 0);
        auto* hdr = reinterpret_cast<BITMAPINFOHEADER*>(blob.data());
        hdr->biSize = sizeof(BITMAPINFOHEADER);
        hdr->biWidth = s;
        hdr->biHeight = s * 2; // XOR bitmap + AND mask, the ICO convention
        hdr->biPlanes = 1;
        hdr->biBitCount = 32;
        hdr->biCompression = BI_RGB;
        hdr->biSizeImage = (DWORD)(pixels + maskBytes);
        // ICO stores rows bottom-up; LockBits hands them over top-down.
        BYTE* dst = blob.data() + 40;
        for (int y = 0; y < s; ++y) {
            memcpy(dst + (size_t)(s - 1 - y) * (size_t)s * 4u,
                   (const BYTE*)bd.Scan0 + (size_t)y * (size_t)bd.Stride, (size_t)s * 4u);
        }
        bmp.UnlockBits(&bd);
        blobs.push_back(std::move(blob));
    }

    std::vector<BYTE> out(6 + (size_t)count * 16u, 0);
    out[2] = 1; // type: icon
    out[4] = (BYTE)(count & 0xFF);
    out[5] = (BYTE)(count >> 8);
    size_t offset = 6 + (size_t)count * 16u;
    for (int i = 0; i < count; ++i) {
        BYTE* e = out.data() + 6 + (size_t)i * 16u;
        const int s = kSizes[i];
        e[0] = (BYTE)(s >= 256 ? 0 : s); // 0 means 256
        e[1] = (BYTE)(s >= 256 ? 0 : s);
        e[4] = 1;  // colour planes
        e[6] = 32; // bits per pixel
        const DWORD len = (DWORD)blobs[(size_t)i].size();
        memcpy(e + 8, &len, 4);
        const DWORD off = (DWORD)offset;
        memcpy(e + 12, &off, 4);
        offset += len;
    }
    for (const std::vector<BYTE>& b : blobs) out.insert(out.end(), b.begin(), b.end());

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    const size_t written = fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return written == out.size();
}

// --- log file ---------------------------------------------------------------
// A failure the user can only describe in words is a failure we have to guess
// at. The failed card opens the log instead: one click on someone else's
// machine replaces a round of questions.
void open_log() {
    const std::wstring path = fc::log_file_path();
    if (path.empty()) return;
    // Notepad rather than the default handler: .log is unassociated on plenty
    // of installs, and ShellExecute would then do nothing at all.
    std::wstring args = L"\"" + path + L"\"";
    ::ShellExecuteW(nullptr, L"open", L"notepad.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

// --- interaction ------------------------------------------------------------
bool in_rect(const RectF& r, POINT pt) {
    return (float)pt.x >= r.X && (float)pt.x < r.X + r.Width && (float)pt.y >= r.Y &&
           (float)pt.y < r.Y + r.Height;
}

Hit hit_test(const Layout& L, POINT pt) {
    if (in_rect(L.closeBtn, pt)) return Hit::Close;
    if (in_rect(L.minBtn, pt)) return Hit::Minimize;
    if (in_rect(L.device, pt)) return Hit::Device;
    if (in_rect(L.rows[row_quality()], pt)) return Hit::Quality;
    if (row_screen() >= 0 && in_rect(L.rows[row_screen()], pt)) return Hit::Screen;
    if (in_rect(L.rows[row_sound()], pt)) return Hit::Sound;
    if (in_rect(L.rows[row_language()], pt)) return Hit::Language;
    if (in_rect(L.mainBtn, pt)) return Hit::Main;
    // The status card only reacts when it is showing a failure: that is the one
    // state where there is a log worth reading.
    if (g_app.phase == Phase::Failed && in_rect(L.status, pt)) return Hit::Log;
    return Hit::None;
}

void set_language(fc::Lang l, bool persist) {
    g_app.cfg.lang = l;
    g_app.lang = fc::resolve_lang(l);
    g_app.T = &fc::strings(g_app.lang);
    ui::fonts_init(g_app.scale, fc::font_family(g_app.lang));
    if (persist) fc::settings_save(g_app.cfg);
}

void apply_drop_item(int idx) {
    const Drop d = g_app.drop;
    const std::vector<DropItem> items = build_items(d);
    if (idx < 0 || idx >= (int)items.size()) return;

    if (d == Drop::Language) {
        const fc::Lang order[fc::kLangCount] = {fc::Lang::System, fc::Lang::ZhHans,
                                                fc::Lang::ZhHant, fc::Lang::En};
        set_language(order[idx]);
    } else if (d == Drop::Quality) {
        g_app.cfg.quality = idx;
        fc::settings_save(g_app.cfg);
    } else if (d == Drop::Screen) {
        if (idx < (int)g_app.displays.size()) {
            // Stored as the device name, so unplugging one screen does not
            // silently retarget the mirror at another.
            g_app.cfg.display = g_app.displays[(size_t)idx].device;
            fc::settings_save(g_app.cfg);
        }
    } else if (d == Drop::Device) {
        const int n = (int)g_app.devices.size();
        if (idx >= 0 && idx < n) {
            g_app.selected = idx;
            g_app.manual = false;
        } else if (idx == n) {
            g_app.drop = Drop::None;
            show_edit(true);
            ::InvalidateRect(g_hwnd, nullptr, FALSE);
            return;
        } else if (idx == n + 1) {
            begin_scan();
        }
    }
    g_app.drop = Drop::None;
    ::InvalidateRect(g_hwnd, nullptr, FALSE);
}

bool pull_state() {
    Phase phase;
    std::wstring error;
    std::wstring target;
    std::vector<Device> devices;
    bool scanning;
    fc::ClientStats stats;
    bool hasStats;
    {
        std::lock_guard<std::mutex> g(g_mu);
        phase = g_phase;
        error = g_error;
        target = g_target;
        devices = g_devices;
        scanning = g_scanning;
        stats = g_stats;
        hasStats = g_hasStats;
    }
    bool changed = false;
    if (phase != g_app.phase || error != g_app.error || scanning != g_app.scanning ||
        devices.size() != g_app.devices.size() || target != g_app.target) {
        changed = true;
    }
    g_app.phase = phase;
    g_app.error = error;
    g_app.target = target;
    g_app.devices = std::move(devices);
    g_app.scanning = scanning;
    g_app.stats = stats;
    g_app.hasStats = hasStats;
    if (g_app.selected >= (int)g_app.devices.size()) g_app.selected = -1;
    // Showing which TV is targeted is worth more than staying on "let discovery
    // decide": the card then always names a real device.
    if (g_app.selected < 0 && !g_app.manual && !g_app.devices.empty()) g_app.selected = 0;
    return changed;
}

void request_close() {
    if (g_workerRunning) {
        g_quitAfterStop = true;
        request_stop();
        return;
    }
    if (g_worker.joinable()) g_worker.join();
    ::DestroyWindow(g_hwnd);
}

// --- tray icon --------------------------------------------------------------
// Minimising puts the window away and leaves the session running, which is the
// point: mirroring should not stop because the user tidied their desktop. The
// icon is then the only way back, so it goes up before the window hides and the
// window refuses to hide if the shell would not take the icon.
constexpr UINT WM_APP_TRAY = WM_APP + 3;
constexpr UINT kTrayId = 1;
enum { kTrayShow = 1, kTrayStop, kTrayQuit };

NOTIFYICONDATAW g_nid{};
bool g_trayAdded = false;
bool g_trayHidden = false;
bool g_trayHintShown = false;
UINT g_taskbarCreatedMsg = 0;

void tray_add() {
    if (g_trayAdded) return;
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = kTrayId;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    // Load at the shell's own small-icon size. Handing a 32 px image to a 16 px
    // slot and letting the shell downscale it is what makes tray icons look
    // blurry next to everything else down there.
    const int cx = ::GetSystemMetrics(SM_CXSMICON);
    g_nid.hIcon = load_app_icon(cx > 0 ? cx : 16);
    lstrcpynW(g_nid.szTip, L"FlowCast", ARRAYSIZE(g_nid.szTip));
    g_trayAdded = ::Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
    if (!g_trayAdded && g_nid.hIcon) {
        ::DestroyIcon(g_nid.hIcon);
        g_nid.hIcon = nullptr;
    }
}

void tray_remove() {
    if (!g_trayAdded) return;
    ::Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_nid.hIcon) ::DestroyIcon(g_nid.hIcon);
    g_nid.hIcon = nullptr;
    g_trayAdded = false;
}

void restore_from_tray() {
    g_trayHidden = false;
    ::ShowWindow(g_hwnd, SW_SHOW);
    ::SetForegroundWindow(g_hwnd);
    ::InvalidateRect(g_hwnd, nullptr, FALSE);
}

void hide_to_tray() {
    if (!g_trayAdded) tray_add();
    if (!g_trayAdded) {
        // No icon means no way back, so a plain taskbar minimise is the honest
        // fallback rather than a window that disappears for good.
        ::ShowWindow(g_hwnd, SW_MINIMIZE);
        return;
    }
    if (g_trayHidden) return;
    g_trayHidden = true;
    ::ShowWindow(g_hwnd, SW_HIDE);
    if (!g_trayHintShown) {
        // Said once, when the window first vanishes. Without it a hidden window
        // reads as "the app closed itself".
        g_trayHintShown = true;
        g_nid.uFlags = NIF_INFO;
        lstrcpynW(g_nid.szInfoTitle, L"FlowCast", ARRAYSIZE(g_nid.szInfoTitle));
        lstrcpynW(g_nid.szInfo, g_app.T ? g_app.T->trayHint : L"", ARRAYSIZE(g_nid.szInfo));
        g_nid.dwInfoFlags = NIIF_INFO;
        ::Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    }
}

void tray_menu() {
    const fc::Strings* T = g_app.T;
    HMENU m = ::CreatePopupMenu();
    if (!m) return;
    ::AppendMenuW(m, MF_STRING, kTrayShow, T ? T->trayShow : L"Show window");
    const bool busy = g_app.phase == Phase::Mirroring || g_app.phase == Phase::Connecting;
    ::AppendMenuW(m, MF_STRING | (busy ? 0u : MF_GRAYED), kTrayStop,
                  T ? T->trayStop : L"Stop mirroring");
    ::AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(m, MF_STRING, kTrayQuit, T ? T->trayQuit : L"Quit");

    POINT pt;
    ::GetCursorPos(&pt);
    // The window is hidden here, but TrackPopupMenu still needs an owner that
    // owns the foreground or the menu will not dismiss when the user clicks
    // away from it.
    ::SetForegroundWindow(g_hwnd);
    ::TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
    ::PostMessageW(g_hwnd, WM_NULL, 0, 0);
    ::DestroyMenu(m);
}

// Re-reads the attached screens and grows/shows the "capture screen" row when a
// second one appears (or drops it again when it is unplugged). Returns true when
// the card changed height, i.e. the window has to be resized.
bool refresh_displays() {
    const int before = group_rows();
    g_app.displays = fc::list_displays();
    // A saved screen that is not attached right now is deliberately kept: the
    // row falls back to the primary one (active_display), and plugging the TV
    // back in resumes capturing it without the user re-picking it every time.
    return group_rows() != before;
}

void relayout_and_resize(HWND hwnd, bool from_dpi) {
    RECT rc;
    ::GetClientRect(hwnd, &rc);
    g_app.layout = compute_layout((float)(rc.right - rc.left), (float)(rc.bottom - rc.top));
    if (from_dpi) {
        const int w = (int)std::lround(kW * g_app.scale);
        const int h = (int)std::lround(window_height() * g_app.scale);
        ::SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ::InvalidateRect(hwnd, nullptr, FALSE);
}

// --- window procedure -------------------------------------------------------
LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Explorer restarted and dropped every tray icon. The message id is only
    // known at runtime, so it has to be compared before the switch.
    if (g_taskbarCreatedMsg != 0 && msg == g_taskbarCreatedMsg) {
        const bool had = g_trayAdded;
        g_trayAdded = false;
        if (had) tray_add();
        return 0;
    }

    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        g_app.layout = compute_layout(kW * g_app.scale, window_height() * g_app.scale);
        relayout_and_resize(hwnd, false);

        ::SetTimer(hwnd, kTickTimer, kTickMs, nullptr);

        const HICON iconBig = load_app_icon((int)std::lround(32.f * g_app.scale));
        const HICON iconSmall = load_app_icon((int)std::lround(16.f * g_app.scale));
        if (iconBig) ::SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)iconBig);
        if (iconSmall) ::SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)iconSmall);

        int round = 2; // DWMWCP_ROUND
        ::DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round,
                                sizeof(round));
        begin_scan();
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = ::BeginPaint(hwnd, &ps);
        RECT rc;
        ::GetClientRect(hwnd, &rc);
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w > 0 && h > 0) {
            // Render off-screen first, exactly like the --shot path. ClearType
            // needs a real 32bpp surface to work at all; asked to draw straight
            // into the window DC, GDI+ silently falls back to grayscale and the
            // whole UI ends up a shade lighter than every native window beside
            // it. One 480x574 copy per paint is free.
            Bitmap off(w, h, PixelFormat32bppARGB);
            {
                Graphics og(&off);
                ui::prepare(og);
                paint(og);
            }
            Graphics g(hdc);
            g.SetCompositingMode(CompositingModeSourceCopy);
            g.SetPixelOffsetMode(PixelOffsetModeNone);
            g.SetInterpolationMode(InterpolationModeNearestNeighbor);
            g.DrawImage(&off, 0, 0);
        }
        ::EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) relayout_and_resize(hwnd, false);
        return 0;

    case WM_DPICHANGED: {
        g_app.dpi = HIWORD(wParam);
        g_app.scale = (float)g_app.dpi / 96.f;
        ui::fonts_init(g_app.scale, fc::font_family(g_app.lang));
        relayout_and_resize(hwnd, true);
        return 0;
    }

    case WM_DISPLAYCHANGE: {
        // A screen was plugged in, unplugged, or changed mode. Re-read them so
        // the settings card reflects reality instead of what was attached at
        // launch; the row count decides whether the window has to grow.
        const bool rowsChanged = refresh_displays();
        g_app.drop = Drop::None;
        relayout_and_resize(hwnd, rowsChanged);
        return 0;
    }

    case WM_SETTINGCHANGE:
        // The window no longer follows the desktop theme (see wWinMain), so
        // ImmersiveColorSet is deliberately ignored: switching Windows to light
        // must not repaint this window light in the middle of a session.
        return 0;

    case WM_TIMER: {
        if (wParam != kTickTimer) break;
        const bool changed = pull_state();
        const bool animated = g_app.scanning || g_app.phase == Phase::Connecting ||
                              g_app.phase == Phase::Stopping || g_app.phase == Phase::Mirroring;
        // While the address is being typed the caret blinks, so the window has to
        // repaint on every tick regardless of whether anything else moved.
        if (changed || animated || g_app.editing) ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_APP_TRAY: {
        // The icon was added without NIM_SETVERSION, so lParam is the raw mouse
        // message rather than the version-4 packed event.
        const UINT ev = (UINT)lParam;
        if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK) {
            restore_from_tray();
        } else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
            tray_menu();
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kTrayShow:
            restore_from_tray();
            return 0;
        case kTrayStop:
            request_stop();
            return 0;
        case kTrayQuit:
            request_close();
            return 0;
        default:
            break;
        }
        break;

    case WM_SYSCOMMAND:
        // Covers the shell's own minimise paths (Win+D, the taskbar menu) as
        // well as our button, so the window can never end up collapsed to the
        // taskbar with the tray icon as a second, redundant way back.
        if ((wParam & 0xfff0) == SC_MINIMIZE) {
            hide_to_tray();
            return 0;
        }
        break;

    case WM_APP_SCAN_ENDED:
        if (g_scan.joinable()) g_scan.join();
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_APP_SESSION_ENDED:
        if (g_worker.joinable()) g_worker.join();
        if (g_quitAfterStop) {
            ::DestroyWindow(hwnd);
            return 0;
        }
        pull_state();
        // A failure the user cannot see is worse than an interrupted tidy-up:
        // bring the window back so the reason is on screen instead of leaving
        // an icon that looks like it is still working.
        if (g_trayHidden && g_app.phase == Phase::Failed) restore_from_tray();
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_NCHITTEST: {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ::ScreenToClient(hwnd, &pt);
        const Layout& L = g_app.layout;
        if (in_rect(L.closeBtn, pt) || in_rect(L.minBtn, pt)) return HTCLIENT;
        if ((float)pt.y < sc(44.f)) return HTCAPTION;
        return HTCLIENT;
    }

    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&tme);
        const POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (g_app.drop != Drop::None) {
            const int idx = drop_hit_test(g_app.layout, pt);
            if (idx != g_app.dropHot) {
                g_app.dropHot = idx;
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        const Hit h = hit_test(g_app.layout, pt);
        if (h != g_app.hot) {
            g_app.hot = h;
            ::InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (g_app.hot != Hit::None || g_app.dropHot != -1) {
            g_app.hot = Hit::None;
            g_app.dropHot = -1;
            ::InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        const POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (g_app.drop != Drop::None) {
            const int idx = drop_hit_test(g_app.layout, pt);
            if (idx >= 0) apply_drop_item(idx);
            else {
                g_app.drop = Drop::None;
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (g_app.editing) {
            // A click anywhere while typing only closes the editor; acting on it
            // would start a session with an address the user was still editing.
            commit_edit();
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        g_app.down = hit_test(g_app.layout, pt);
        if (g_app.down != Hit::None) ::SetCapture(hwnd);
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (::GetCapture() == hwnd) ::ReleaseCapture();
        const POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const Hit h = hit_test(g_app.layout, pt);
        const Hit down = g_app.down;
        g_app.down = Hit::None;
        if (h != Hit::None && h == down) {
            switch (h) {
            case Hit::Close:
                request_close();
                return 0;
            case Hit::Minimize:
                hide_to_tray();
                break;
            case Hit::Device:
                g_app.drop = g_app.drop == Drop::Device ? Drop::None : Drop::Device;
                if (g_app.drop == Drop::Device && g_app.devices.empty() && !g_app.scanning) {
                    begin_scan();
                }
                break;
            case Hit::Quality:
                g_app.drop = g_app.drop == Drop::Quality ? Drop::None : Drop::Quality;
                break;
            case Hit::Screen: {
                if (g_app.drop == Drop::Screen) {
                    g_app.drop = Drop::None;
                    break;
                }
                // Re-read first: a screen plugged in since launch (or since the
                // last look) has to be in the list before it can be chosen.
                const bool rowsChanged = refresh_displays();
                g_app.drop = Drop::Screen;
                if (rowsChanged) relayout_and_resize(hwnd, true);
                break;
            }
            case Hit::Language:
                g_app.drop = g_app.drop == Drop::Language ? Drop::None : Drop::Language;
                break;
            case Hit::Sound:
                g_app.cfg.systemAudio = !g_app.cfg.systemAudio;
                fc::settings_save(g_app.cfg);
                break;
            case Hit::Main:
                if (g_app.phase == Phase::Stopping) break;
                if (g_workerRunning) request_stop();
                else start_session();
                break;
            case Hit::Log:
                open_log();
                break;
            default:
                break;
            }
        }
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN: {
        // Everything the inline editor needs is here: it owns the keyboard while
        // it is on screen, and Escape/Return must not fall through to the window
        // shortcuts below.
        if (g_app.editing) {
            const int n = (int)g_app.editText.size();
            bool repaint = true;
            switch (wParam) {
            case VK_ESCAPE:
                cancel_edit();
                break;
            case VK_RETURN:
                commit_edit();
                break;
            case VK_BACK:
                if (g_app.editCaret > 0) {
                    g_app.editText.erase((size_t)(g_app.editCaret - 1), 1);
                    --g_app.editCaret;
                }
                break;
            case VK_DELETE:
                if (g_app.editCaret < n) g_app.editText.erase((size_t)g_app.editCaret, 1);
                break;
            case VK_LEFT:
                if (g_app.editCaret > 0) --g_app.editCaret;
                break;
            case VK_RIGHT:
                if (g_app.editCaret < n) ++g_app.editCaret;
                break;
            case VK_HOME:
                g_app.editCaret = 0;
                break;
            case VK_END:
                g_app.editCaret = n;
                break;
            case 'V':
                if (::GetKeyState(VK_CONTROL) < 0) paste_clipboard();
                else repaint = false;
                break;
            case 'C':
            case 'X':
                // Nothing is selectable yet, so the only useful outcome of these
                // is silence instead of the system beep.
                if (::GetKeyState(VK_CONTROL) < 0) return 0;
                repaint = false;
                break;
            default:
                repaint = false;
                break;
            }
            if (repaint) ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_ESCAPE && g_app.drop != Drop::None) {
            g_app.drop = Drop::None;
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    }

    case WM_CHAR: {
        if (!g_app.editing) break;
        const wchar_t ch = (wchar_t)wParam;
        // Control characters are commands, already handled in WM_KEYDOWN; letting
        // them through would print boxes into the address.
        if (ch < 0x20 || ch == 0x7f) return 0;
        insert_text(std::wstring(1, ch));
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_CLOSE:
        request_close();
        return 0;

    case WM_DESTROY:
        ::KillTimer(hwnd, kTickTimer);
        // Before PostQuitMessage: a tray icon that outlives its window is a
        // dead ghost in the notification area until the shell next cleans up.
        tray_remove();
        ::PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);

    bool forceGui = false;
    std::wstring shotPath;
    std::wstring shotState;
    std::wstring themeOverride;
    std::wstring shotLang;
    std::wstring iconPath;
    int shotScreens = 0;
    int shotContrast = -1;
    int shotAA = -1;   // -1 = leave the built-in choice alone
    for (int i = 1; i < argc && argv; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--gui") {
            forceGui = true;
        } else if (a.rfind(L"--make-icon=", 0) == 0) {
            forceGui = true;
            iconPath = a.substr(12);
        } else if (a.rfind(L"--shot-state=", 0) == 0) {
            forceGui = true;
            shotState = a.substr(13);
        } else if (a.rfind(L"--theme=", 0) == 0) {
            forceGui = true;
            themeOverride = a.substr(8);
        } else if (a.rfind(L"--shot-lang=", 0) == 0) {
            forceGui = true;
            shotLang = a.substr(12);
        } else if (a.rfind(L"--shot=", 0) == 0) {
            forceGui = true;
            shotPath = a.substr(7);
        } else if (a.rfind(L"--shot-screens=", 0) == 0) {
            forceGui = true;
            shotScreens = _wtoi(a.c_str() + 15);
        } else if (a.rfind(L"--shot-contrast=", 0) == 0) {
            forceGui = true;
            shotContrast = _wtoi(a.c_str() + 16);
        } else if (a.rfind(L"--shot-aa=", 0) == 0) {
            forceGui = true;
            shotAA = a.substr(10) == L"clearType" ? 1 : 0;
        }
    }

    if (argc > 1 && !forceGui) {
        fc::cli_attach_console();
        std::vector<std::string> args;
        args.reserve((size_t)argc);
        for (int i = 0; i < argc; ++i) args.push_back(fc::utf8_from_wide(argv[i]));
        std::vector<char*> ptrs;
        ptrs.reserve(args.size());
        for (std::string& str : args) ptrs.push_back(&str[0]);
        if (argv) ::LocalFree(argv);
        return fc::cli_main((int)ptrs.size(), ptrs.data());
    }
    if (argv) ::LocalFree(argv);

    // Per-monitor v2 before any window exists, so the layout can trust the
    // scale it computes up front.
    {
        using SetCtxFn = BOOL(WINAPI*)(HANDLE);
        auto fn = (SetCtxFn)(void*)::GetProcAddress(::GetModuleHandleW(L"user32.dll"),
                                                    "SetProcessDpiAwarenessContext");
        if (fn) fn((HANDLE)(intptr_t)-4);
    }

    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &gsi, nullptr) != Ok) return 1;

    if (!iconPath.empty()) {
        const bool ok = write_ico(iconPath);
        GdiplusShutdown(token);
        return ok ? 0 : 1;
    }

    {
        const UINT dpi = ::GetDpiForSystem();
        g_app.dpi = dpi ? dpi : 96;
        g_app.scale = (float)g_app.dpi / 96.f;
        g_app.cfg = fc::settings_load();
        // Dark is the shipped look, not a guess about the desktop. The mark, the
        // accent and the status card were all tuned against it, and following
        // the system meant a light Windows handed the user a light window that
        // the icon sits poorly on. --theme=light still forces it for contrast
        // comparisons; it is not persisted.
        g_app.dark = true;
        if (themeOverride == L"dark") g_app.dark = true;
        else if (themeOverride == L"light") g_app.dark = false;
        g_app.lang = fc::resolve_lang(g_app.cfg.lang);
        g_app.T = &fc::strings(g_app.lang);
        ui::fonts_init(g_app.scale, fc::font_family(g_app.lang));
        // Before the window exists, so the settings card is sized right the
        // first time it is painted instead of growing a frame later.
        if (shotScreens > 0) seed_fake_displays(shotScreens);
        else refresh_displays();
        g_app.layout = compute_layout(kW * g_app.scale, window_height() * g_app.scale);
    }

    // The host process owns the log sink: the status card shows the last error
    // line the session produced, which is the only place a user can see it.
    fc::log_init(fc::LogLevel::Info, on_log, nullptr);
    // Double-clicked from Explorer there is no console, so the same lines also
    // go to a file. Its path is logged so the failed card can point at it.
    fc::log_open_file(fc::log_default_path());
    fc::log_write(fc::LogLevel::Info, "gui: %ls", fc::log_file_path().c_str());

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClass;
    if (!::RegisterClassExW(&wc)) {
        GdiplusShutdown(token);
        return 1;
    }

    // Explorer broadcasts this after a restart, which is the only notice an app
    // gets that its tray icon was thrown away with the old notification area.
    g_taskbarCreatedMsg = ::RegisterWindowMessageW(L"TaskbarCreated");

    const int w = (int)std::lround(kW * g_app.scale);
    const int h = (int)std::lround(window_height() * g_app.scale);
    int x = 0;
    int y = 0;
    {
        RECT work{0, 0, 0, 0};
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        x = work.left + ((work.right - work.left) - w) / 2;
        y = work.top + ((work.bottom - work.top) - h) / 2;
    }

    HWND hwnd = ::CreateWindowExW(WS_EX_APPWINDOW, kClass, L"FlowCast", WS_POPUP, x, y, w, h,
                                  nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        GdiplusShutdown(token);
        return 1;
    }

    MSG msg;
    if (!shotPath.empty()) {
        // Let discovery finish so the card shows real receivers, then render.
        const DWORD deadline = ::GetTickCount() + 2600;
        while ((int)(deadline - ::GetTickCount()) > 0) {
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
            ::Sleep(10);
        }
        ui::set_text_aa(shotAA, shotContrast);
        save_shot(shotPath, shotState, shotLang);
    } else {
        ::ShowWindow(hwnd, SW_SHOW);
        ::UpdateWindow(hwnd);
        while (::GetMessageW(&msg, nullptr, 0, 0)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }

    if (g_worker.joinable()) {
        g_stopFlag = true;
        g_worker.join();
    }
    if (g_scan.joinable()) g_scan.join();
    ui::fonts_release();
    GdiplusShutdown(token);
    return 0;
}
