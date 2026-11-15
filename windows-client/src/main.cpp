// FlowCast Windows client - console entry point.
//
// Mirrors the Windows desktop (video + system audio) to a FlowCast receiver on
// the local network. Run with no arguments to get the window; any argument runs
// this parser instead, so `flowcast --host 192.168.0.201` behaves as before.
#include "cli.h"
#include "capture.h"
#include "client.h"
#include "h264enc.h"
#include "log.h"
#include "probe.h"
#include "text.h"
#include "wgc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

fc::FlowCastClient g_client;
std::atomic<bool> g_stopFlag{false};

BOOL WINAPI onConsoleCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stopFlag = true;
        return TRUE;
    }
    return FALSE;
}

void printUsage() {
    std::printf(
        "FlowCast Windows client - mirror this PC to a FlowCast receiver\n"
        "\n"
        "usage: flowcast [options]\n"
        "\n"
        "  --host <name|ip>   receiver to use; default: mDNS discovery\n"
        "  --receiver <text>  only accept a discovered receiver whose name contains\n"
        "                     this text (use when several TVs answer)\n"
        "  --port <n>         AirPlay port on the receiver (default 7000)\n"
        "  --name <text>      name shown on the TV (default \"Windows PC\")\n"
        "  --fps <n>          target frame rate (default 30)\n"
        "  --bitrate <bps>    video bitrate; default: derived from resolution\n"
        "  --bpp <n>          bits per pixel per frame for that derivation\n"
        "                     (default 0.35; raise for sharper text)\n"
        "  --max-qp <n>       quantiser ceiling 0-51, 0 = encoder default\n"
        "                     (default 30; lower keeps small text legible)\n"
        "  --apple-ua         send Apple's User-Agent and X-Apple-* headers on every\n"
        "                     request. For receivers that only answer senders they\n"
        "                     recognise as Apple (some MAXHUB / AirServer panels).\n"
        "  --skip-fairplay    skip the FairPlay /fp-setup handshake entirely and go\n"
        "                     straight to SETUP. On by default when the client's\n"
        "                     fallback variants run; this makes it the only attempt.\n"
        "  --hello-only       send the 16-byte /fp-setup greeting but not the 164-byte\n"
        "                     key message (the step strict panels go silent on).\n"
        "  --fp2-timeout <ms> how long to wait for /fp-setup #2 (default 3000)\n"
        "  --single-setup     describe the session in ONE SETUP, the way an Apple\n"
        "                     sender does (keys + streams + isScreenMirroring),\n"
        "                     instead of splitting it over SETUP /1 and /2.\n"
        "  --single-try       do not try the fallback handshake variants; the first\n"
        "                     attempt fails the run. Makes scripted runs fast.\n"
        "  --hevc             encode H.265 instead of H.264. Experimental: the\n"
        "                     picture is slightly better on text, but this PC's\n"
        "                     encoder caps at ~24 fps instead of 30\n"
        "  --max-width <n>    scale the desktop down to this width (default 1920)\n"
        "  --display <n>      capture display n (see --list-displays; default 1)\n"
        "  --list-displays    list the screens that can be captured, then exit\n"
        "  --probe-encoders   list the H.264 / H.265 encoder MFTs installed, then exit\n"
        "  --probe-host <ip[:port]>\n"
        "                     ask an AirPlay receiver which pairing methods it\n"
        "                     implements, then exit (for a TV that refuses us)\n"
        "  --probe-capture    open the capture path and report the size frames\n"
        "                     actually arrive at, then exit\n"
        "  --audio-bitrate <bps>  AAC bitrate (default 128000)\n"
        "  --no-video         audio only\n"
        "  --no-audio         video only\n"
        "  --discover-ms <n>  how long to browse for a receiver (default 3000)\n"
        "  --duration <sec>   stop automatically after N seconds (default: Ctrl+C)\n"
        "  --test-pattern     send a synthetic colour bar picture instead of the\n"
        "                     desktop (diagnose a desktop that cannot be captured)\n"
        "  -v, --verbose      debug logging\n"
        "  -h, --help         this text\n"
        "\n"
        "The receiver is found by browsing for _airplay._tcp. Every candidate is\n"
        "logged; --receiver narrows the choice down when several answer.\n"
        "Press Ctrl+C to stop mirroring.\n");
}

// Opens the real capture path and reports the size frames actually arrive at.
// The list above comes from EnumDisplayMonitors, i.e. what Windows *thinks* the
// desktop is; this asks Desktop Duplication for the surface it hands over, and
// the two disagree under Virtual Super Resolution - the desktop composites at
// 4K while the output stays at the panel's 1080p, so the 4K never arrives. That
// makes "why is my 4K mirrored at 1080p" one command, not a guess.
// Whether H.264 can be encoded is a completely separate question from whether
// the desktop can be captured: the encoder comes from the installed codec
// list, so a machine can capture perfectly and still have nothing that will
// encode. Reporting both from one command is what makes "it fails on that PC"
// diagnosable from a single paste, without a cast attempt.
int probeEncoder(size_t width, size_t height, int fps) {
    if (width == 0 || height == 0) {
        width = 1280;
        height = 720;
    }
    fc::H264Encoder enc;
    if (!enc.start(width, height, fps, 4400000)) {
        std::printf("encoder probe: failed (%s)\n", enc.startDetail().c_str());
        return 1;
    }
    std::printf("encoder probe: %zux%zu via %s\n", width, height, enc.backend().c_str());
    // Negotiating types is not the same as encoding: a driver that answers every
    // SetType and then emits nothing looks identical from the outside. Push one
    // neutral frame through and check bytes come out.
    const size_t yPlane = width * height;
    std::vector<uint8_t> nv12(yPlane + yPlane / 2, 0x80);
    memset(nv12.data(), 0x10, yPlane); // limited-range black
    std::vector<uint8_t> nals;
    int count = enc.encodeNv12(nv12.data(), width, height, 1, nals);
    std::printf("encoder output:%d NAL unit(s), %zu byte(s)%s\n", count, nals.size(),
                (count == 0 || nals.empty()) ? "  <-- accepted the types but produced nothing" : "");
    return (count > 0 && !nals.empty()) ? 0 : 1;
}

int probeCapture(const std::wstring& device) {
    // Three different answers to "how big is this screen", and knowing which is
    // which is the whole point of this command: the display mode is what the
    // driver outputs, GetSystemMetrics is DPI-virtualised (a non-aware process
    // on a 4K screen at 200% is told it has 1920x1080), and the duplication
    // surface is the texture actually handed over.
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (::EnumDisplaySettingsW(device.empty() ? nullptr : device.c_str(), ENUM_CURRENT_SETTINGS,
                               &dm)) {
        std::printf("display mode:  %lux%lu @ %luHz\n", (unsigned long)dm.dmPelsWidth,
                    (unsigned long)dm.dmPelsHeight, (unsigned long)dm.dmDisplayFrequency);
    }
    std::printf("system metrics:%ldx%ld (dpi aware: %s)\n", (long)::GetSystemMetrics(SM_CXSCREEN),
                (long)::GetSystemMetrics(SM_CYSCREEN),
                ::GetAwarenessFromDpiAwarenessContext(::GetThreadDpiAwarenessContext()) ==
                        DPI_AWARENESS_UNAWARE
                    ? "no"
                    : "yes");
    // The two facts that decide whether any capture API will cooperate.
    std::printf("remote session: %s, DWM capture available: %s\n",
                ::GetSystemMetrics(SM_REMOTESESSION) ? "yes" : "no",
                fc::WgcCapture::supported() ? "yes" : "no");
    std::printf("log file:      %s\n", fc::utf8_from_wide(fc::log_file_path()).c_str());

    fc::ScreenCapture cap;
    // Deliberately no max-width: the question is what the source can give, not
    // what a preset would shrink it to.
    if (!cap.start(0, device)) {
        // Which of the three APIs refused, and with what code: this one line is
        // usually the whole answer.
        std::printf("capture probe: capture failed to start (%s)\n", cap.startDetail().c_str());
        // Still worth asking about the encoder: a machine that fails both needs
        // to know it fails both.
        probeEncoder(0, 0, 30);
        return 1;
    }
    std::printf("capture probe: %zux%zu via %s\n", cap.width(), cap.height(),
                fc::ScreenCapture::backend_name(cap.backend()));
    fc::CapturedFrame f;
    for (int i = 0; i < 300; ++i) {
        if (cap.acquireFrame(f) && f.valid) {
            std::printf("first frame:   %zux%zu %s\n", f.width, f.height,
                        f.format == fc::PixelFormat::Nv12 ? "NV12" : "BGRA");
            // A frame that arrives but is entirely black is the one failure a
            // packet counter cannot see: the desktop was captured, the encoder
            // worked, and the TV shows nothing. Sample a grid instead of one
            // pixel, because a desktop is mostly paper-white at the top.
            unsigned long lit = 0, total = 0;
            const size_t stepX = f.width / 16 + 1;
            const size_t stepY = f.height / 16 + 1;
            for (size_t y = 0; y < f.height; y += stepY) {
                const uint8_t* row = f.data + y * f.stride;
                for (size_t x = 0; x < f.width; x += stepX) {
                    ++total;
                    const uint8_t v =
                        (f.format == fc::PixelFormat::Nv12) ? row[x] : row[x * 4 + 1];
                    if (v > 8) ++lit;
                }
            }
            std::printf("frame pixels:  %lu of %lu samples above black%s\n", lit, total,
                        lit == 0 ? "  <-- captured desktop is black" : "");
            probeEncoder(f.width, f.height, 30);
            return 0;
        }
        ::Sleep(10);
    }
    // "No new pixels" and "Windows will not give us the desktop at all" both
    // look like silence from here; the error code is what tells them apart, and
    // only the second one means mirroring can never work in this session.
    if (cap.lastError() != 0) {
        std::printf("capture probe: capture is being denied (backend %s, error %lu) - a locked "
                    "screen, a remote session, or another app holding the desktop\n",
                    fc::ScreenCapture::backend_name(cap.backend()), cap.lastError());
    } else {
        std::printf("capture probe: no frame arrived (static screen or a locked desktop)\n");
    }
    probeEncoder(0, 0, 30);
    return 1;
}

// Prints the screens someone can pick with --display. Desktop Duplication can
// only capture one screen at a time, so the list is the honest answer to
// "which screen is this going to grab".
void printDisplays() {
    const std::vector<fc::DisplayInfo> all = fc::list_displays();
    if (all.empty()) {
        std::printf("no attached display found\n");
        return;
    }
    std::printf("capturable screens:\n");
    for (const fc::DisplayInfo& d : all) {
        std::printf("  %d  %ux%u  %-14s%s\n", d.index, (unsigned)d.width, (unsigned)d.height,
                    fc::utf8_from_wide(d.device).c_str(), d.primary ? "  (primary)" : "");
    }
}

// Returns true when `arg` matched and a value was consumed from `next`.
bool matchValue(int argc, char** argv, int& i, const char* name, std::string& out) {
    if (std::strcmp(argv[i], name) != 0) return false;
    if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s needs a value\n", name);
        std::exit(2);
    }
    out = argv[++i];
    return true;
}

bool matchInt(int argc, char** argv, int& i, const char* name, long& out) {
    std::string s;
    if (!matchValue(argc, argv, i, name, s)) return false;
    char* end = nullptr;
    out = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') {
        std::fprintf(stderr, "error: %s expects a number, got \"%s\"\n", name, s.c_str());
        std::exit(2);
    }
    return true;
}

} // namespace

namespace fc {

void cli_attach_console() {
    // A pipe or a file redirect is already a working stdout: the exe inherits the
    // handle even though it is a GUI-subsystem binary. Reopening CONOUT$ here
    // would throw that away and make `flowcast ... | grep` print nothing.
    const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE && ::GetFileType(out) != FILE_TYPE_UNKNOWN) return;

    // Launched from cmd or PowerShell the parent owns a console; attaching to it
    // keeps the output in the same window instead of popping a new one.
    if (!::AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!::AllocConsole()) return;
    }
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);
    std::printf("\n");
}

int cli_main(int argc, char** argv) {
    fc::ClientConfig cfg;
    long durationSec = 0;
    int displayIndex = 0; // 0 = keep the default (primary)
    bool doProbe = false;

    fc::log_open_file(fc::log_default_path());

    for (int i = 1; i < argc; ++i) {
        std::string s;
        long n = 0;
        if (matchValue(argc, argv, i, "--host", s)) cfg.host = s;
        else if (matchValue(argc, argv, i, "--receiver", s)) cfg.receiverMatch = s;
        else if (matchValue(argc, argv, i, "--name", s)) cfg.name = s;
        else if (matchInt(argc, argv, i, "--port", n)) cfg.port = (uint16_t)n;
        else if (matchInt(argc, argv, i, "--fps", n)) cfg.fps = (int)n;
        else if (matchInt(argc, argv, i, "--bitrate", n)) cfg.videoBitrate = (int)n;
        else if (matchValue(argc, argv, i, "--bpp", s)) cfg.bitsPerPixel = std::atof(s.c_str());
        else if (matchInt(argc, argv, i, "--max-qp", n)) cfg.maxQp = (int)n;
        else if (std::strcmp(argv[i], "--skip-fairplay") == 0) {
            cfg.fairplay = FairplayMode::kSkip;
            cfg.fairplayPinned = true;
        } else if (std::strcmp(argv[i], "--hello-only") == 0) {
            cfg.fairplay = FairplayMode::kHelloOnly;
            cfg.fairplayPinned = true;
        }
        else if (matchInt(argc, argv, i, "--fp2-timeout", n)) cfg.fp2TimeoutMs = (int)n;
        else if (std::strcmp(argv[i], "--single-setup") == 0) {
            cfg.setup = SetupStyle::kSingle;
            cfg.setupPinned = true;
        }
        else if (std::strcmp(argv[i], "--single-try") == 0) cfg.retryVariants = false;
        else if (std::strcmp(argv[i], "--apple-ua") == 0) cfg.appleUa = true;
        else if (std::strcmp(argv[i], "--hevc") == 0) cfg.hevc = true;
        else if (matchInt(argc, argv, i, "--max-width", n)) cfg.maxWidth = (int)n;
        else if (matchInt(argc, argv, i, "--display", n)) displayIndex = (int)n;
        else if (std::strcmp(argv[i], "--list-displays") == 0) {
            printDisplays();
            return 0;
        }
        else if (std::strcmp(argv[i], "--probe-capture") == 0) doProbe = true;
        else if (matchValue(argc, argv, i, "--probe-host", s)) {
            // Accept "ip" or "ip:port". Receivers do not all listen on 7000 --
            // the MAXHUB panel that motivated this command advertises 7386 --
            // so the port has to come from the caller or from mDNS.
            std::string h = s;
            unsigned short p = 7000;
            const size_t colon = s.rfind(':');
            if (colon != std::string::npos) {
                h = s.substr(0, colon);
                p = (unsigned short)std::atoi(s.c_str() + colon + 1);
            }
            fc::run_probe(h.c_str(), p);
            return 0;
        }
        else if (std::strcmp(argv[i], "--probe-encoders") == 0) {
            list_video_encoders();
            return 0;
        }
        else if (matchInt(argc, argv, i, "--audio-bitrate", n)) cfg.audioBitrate = (int)n;
        else if (matchInt(argc, argv, i, "--discover-ms", n)) cfg.discoverMs = (int)n;
        else if (matchInt(argc, argv, i, "--duration", n)) durationSec = n;
        else if (std::strcmp(argv[i], "--no-video") == 0) cfg.video = false;
        else if (std::strcmp(argv[i], "--no-audio") == 0) cfg.audio = false;
        else if (std::strcmp(argv[i], "--test-pattern") == 0) cfg.testPattern = true;
        else if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--verbose") == 0)
            cfg.verbose = true;
        else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            printUsage();
            return 0;
        } else {
            std::fprintf(stderr, "error: unknown option \"%s\"\n\n", argv[i]);
            printUsage();
            return 2;
        }
    }

    if (displayIndex > 0) {
        const std::vector<fc::DisplayInfo> all = fc::list_displays();
        if (displayIndex > (int)all.size()) {
            std::fprintf(stderr, "error: --display %d but only %d screen(s) attached\n",
                         displayIndex, (int)all.size());
            return 2;
        }
        cfg.display = all[(size_t)displayIndex - 1].device;
    }
    if (doProbe) return probeCapture(cfg.display);
    if (cfg.fps <= 0 || cfg.fps > 120) {
        std::fprintf(stderr, "error: --fps must be between 1 and 120\n");
        return 2;
    }
    if (!cfg.video && !cfg.audio) {
        std::fprintf(stderr, "error: --no-video and --no-audio together leave nothing to send\n");
        return 2;
    }

    if (!SetConsoleCtrlHandler(onConsoleCtrl, TRUE)) {
        std::fprintf(stderr, "warning: could not install the Ctrl+C handler\n");
    }
    // Default Windows timer granularity is ~15.6 ms, which turns the 33 ms video
    // pacing and the audio thread's 2 ms poll into coarse, jittery wake-ups. A
    // 1 ms resolution is what makes "smooth" possible; it costs a little power.
    timeBeginPeriod(1);

    cfg.onProgress = [](const fc::FlowCastClient::Stats& st) {
        std::printf("  %6.1f s  video %6llu pkt / %6.1f MiB   audio %6llu pkt   %ux%u@%d\n",
                    st.elapsedSec, (unsigned long long)st.videoPackets,
                    (double)st.videoBytes / 1048576.0,
                    (unsigned long long)st.audioPackets, (unsigned)st.width,
                    (unsigned)st.height, st.fps);
        std::fflush(stdout);
    };

    std::printf("FlowCast Windows client\n");
    if (!g_client.start(cfg)) {
        std::fprintf(stderr, "\nfailed to start - see the log above\n");
        return 1;
    }
    std::printf("mirroring to %s - press Ctrl+C to stop\n",
                cfg.host.empty() ? "the discovered receiver" : cfg.host.c_str());

    // --duration is handled by a watchdog so the main thread can stay parked
    // inside run() just like it does for Ctrl+C. It wakes every 100 ms instead
    // of sleeping the whole duration in one go: run() now also returns early
    // (the receiver hung up, the capture died), and a single long Sleep would
    // keep join() -- and therefore the error message -- waiting for the timer
    // to expire anyway.
    std::thread watchdog;
    if (durationSec > 0) {
        watchdog = std::thread([durationSec]() {
            const int ticks = durationSec * 10;
            for (int i = 0; i < ticks; ++i) {
                if (g_stopFlag.load()) return;
                Sleep(100);
            }
            g_stopFlag = true;
        });
    }

    g_client.run(&g_stopFlag);
    // run() can also come back on its own (the receiver hung up, the capture
    // died). Stop the timer before joining it, otherwise the reason would sit
    // unprinted until the whole --duration had elapsed.
    g_stopFlag = true;
    if (watchdog.joinable()) watchdog.join();

    const std::wstring fatal = g_client.error();
    g_client.stop();
    timeEndPeriod(1);
    if (!fatal.empty()) {
        // The session killed itself: say why and leave a failing exit code, so a
        // script driving this does not read it as a normal 30-second run.
        char utf8[512];
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, fatal.c_str(), -1, utf8, sizeof(utf8) - 1,
                                            nullptr, nullptr);
        if (n > 0) {
            utf8[n > 0 ? n - 1 : 0] = '\0';
            std::fprintf(stderr, "error: %s\n", utf8);
        }
        return 2;
    }
    const auto st = g_client.stats();
    std::printf("stopped after %.1f s: %llu video packets (%.1f MiB), %llu audio packets, %ux%u@%d\n",
                st.elapsedSec, (unsigned long long)st.videoPackets,
                (double)st.videoBytes / 1048576.0, (unsigned long long)st.audioPackets,
                (unsigned)st.width, (unsigned)st.height, st.fps);
    return 0;
}

} // namespace fc
