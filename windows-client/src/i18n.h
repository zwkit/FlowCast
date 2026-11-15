// FlowCast Windows client - UI strings.
//
// Every label the window draws comes from here. Adding a language means adding
// one more Strings table plus a branch in resolve_lang(); nothing else changes.
#pragma once

namespace fc {

enum class Lang { System = 0, ZhHans = 1, ZhHant = 2, En = 3 };
constexpr int kLangCount = 4;

struct Strings {
    const wchar_t* tagline;
    const wchar_t* targetLabel;
    const wchar_t* scanning;
    const wchar_t* noReceiver;
    const wchar_t* autoReceiver;
    const wchar_t* refresh;
    const wchar_t* quality;
    const wchar_t* screen;      // label of the "which display" row
    const wchar_t* screenMain;  // appended to the primary display's description
    const wchar_t* sound;
    const wchar_t* language;
    const wchar_t* start;
    const wchar_t* stop;
    const wchar_t* connecting;
    const wchar_t* stopping;
    const wchar_t* ready;
    const wchar_t* readyHint;
    const wchar_t* mirroring;
    const wchar_t* failed;
    const wchar_t* langSystem;
    const wchar_t* q720;
    const wchar_t* q1080;
    const wchar_t* q1080_60;
    const wchar_t* q1440;
    const wchar_t* qNative;
    const wchar_t* soundOn;
    const wchar_t* soundOff;
    const wchar_t* statTime;
    const wchar_t* statFps;
    const wchar_t* statData;
    const wchar_t* manualHint;
    const wchar_t* manualEntry;
    // Appended to a bare count, so both "3 个设备" and "3 receivers" work.
    const wchar_t* deviceSuffix;
    const wchar_t* noLan;
    // Instead of repeating "no receiver found" in the status card, it points at
    // the one thing the user can still do from that screen.
    const wchar_t* manualEscape;
    const wchar_t* errRefused;
    const wchar_t* errNet;
    // Windows refused to hand over the desktop: a locked screen, a remote
    // session, or a display that cannot be duplicated.
    const wchar_t* errNoCapture;
    // The receiver's side went away (its app was closed): the session is over
    // even though nothing "failed" on this machine.
    const wchar_t* errDisconnected;
    // Capture worked but the H.264 encoder would not start: a driver/MFT
    // problem, not a desktop-access one, and the fix is different.
    const wchar_t* errEncoder;
    // Link on the failed card that opens the log file. Without it the only way
    // to diagnose a machine that is not this one is a console window the user
    // does not have.
    const wchar_t* logLink;
    // Tray icon context menu. Minimising puts the window away and leaves the
    // session running, so the menu has to offer a way back and a way out that
    // do not require finding the window.
    const wchar_t* trayShow;
    const wchar_t* trayStop;
    const wchar_t* trayQuit;
    // Balloon shown the first time the window goes to the tray, so a vanished
    // window is not mistaken for a crash.
    const wchar_t* trayHint;
    // The receiver hung up in the middle of the handshake instead of answering
    // with a status code. Distinct from errRefused on purpose: "the TV is not
    // on its waiting page" is the wrong advice here - it was waiting, and then
    // it decided this sender was not welcome.
    const wchar_t* errHangup;
};

// The UI language the machine is set to.
Lang system_language();

// Turns a stored preference into the language actually drawn. Lang::System is
// resolved against system_language(), which is why the default is "follow the
// system" rather than a hard-coded English or Chinese.
Lang resolve_lang(Lang setting);

const Strings& strings(Lang resolved);

// A font family that has real glyph coverage for `lang`. There is no single
// family shipping both simplified and traditional Chinese plus Latin, so this
// picks the platform UI face for each.
const wchar_t* font_family(Lang resolved);

// The language's own name, always written in that language - a picker that says
// "Chinese" to a Chinese user is useless.
const wchar_t* language_label(Lang lang);

} // namespace fc
