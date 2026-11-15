// FlowCast Windows client - persisted preferences.
//
// A plain INI in %APPDATA%\FlowCast\client.ini: readable, hand-editable, and
// writable even when the exe lives under Program Files. No registry, no JSON
// dependency.
#pragma once
#include <string>

#include "i18n.h"

namespace fc {

struct AppSettings {
    Lang lang = Lang::System;
    bool systemAudio = true;
    // 0 = 720p30, 1 = 1080p30, 2 = 1080p60, 3 = 1440p30, 4 = native desktop
    int quality = 1;
    // Screen to capture, e.g. "\\.\DISPLAY2". Empty = the primary display.
    // Stored as the device name rather than an index because an index shifts as
    // soon as a screen is unplugged, which would silently retarget the mirror.
    std::wstring display;
    std::wstring lastAddress;   // last receiver used, preselected for "manual"
    int lastPort = 7000;
};

std::wstring settings_path();
AppSettings settings_load();
void settings_save(const AppSettings& s);

// Quality presets, indexed by AppSettings::quality.
struct QualityPreset {
    int maxWidth;   // 0 = native desktop
    int fps;
};
QualityPreset quality_preset(int index);
constexpr int kQualityCount = 5;

} // namespace fc
