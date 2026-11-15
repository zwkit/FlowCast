// FlowCast Windows client - persisted preferences.
#include "settings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <cstdio>

namespace fc {
namespace {

constexpr const wchar_t* kFile = L"client.ini";
constexpr const wchar_t* kSection = L"client";

std::wstring app_dir() {
    wchar_t* base = nullptr;
    std::wstring out;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base)) && base) {
        out = base;
        out += L"\\FlowCast";
    }
    ::CoTaskMemFree(base);
    if (out.empty()) {
        wchar_t tmp[MAX_PATH] = {0};
        if (::GetTempPathW(MAX_PATH, tmp)) {
            out = tmp;
            out += L"FlowCast";
        }
    }
    ::CreateDirectoryW(out.c_str(), nullptr); // already-exists is fine
    return out;
}

std::wstring read_str(const std::wstring& path, const wchar_t* key, const wchar_t* fallback) {
    wchar_t buf[512] = {0};
    ::GetPrivateProfileStringW(kSection, key, fallback, buf, 512, path.c_str());
    return buf;
}

void write_str(const std::wstring& path, const wchar_t* key, const std::wstring& value) {
    ::WritePrivateProfileStringW(kSection, key, value.c_str(), path.c_str());
}

int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

std::wstring settings_path() {
    std::wstring p = app_dir();
    p += L"\\";
    p += kFile;
    return p;
}

AppSettings settings_load() {
    AppSettings s;
    const std::wstring path = settings_path();

    const std::wstring lang = read_str(path, L"lang", L"system");
    if (lang == L"zh-Hans") s.lang = Lang::ZhHans;
    else if (lang == L"zh-Hant") s.lang = Lang::ZhHant;
    else if (lang == L"en") s.lang = Lang::En;
    else s.lang = Lang::System;

    // Preset-list revision. v1 had "native" at index 3; v2 inserted 1440p there
    // and pushed native down to 4, so a stored 3 from an older build has to move
    // rather than silently leaving the user on 1440p.
    const int rev = ::GetPrivateProfileIntW(kSection, L"presetRev", 1, path.c_str());
    int quality = ::GetPrivateProfileIntW(kSection, L"quality", 1, path.c_str());
    if (rev < 2 && quality == 3) quality = 4;
    s.quality = clamp_int(quality, 0, kQualityCount - 1);
    s.systemAudio =
        ::GetPrivateProfileIntW(kSection, L"systemAudio", 1, path.c_str()) != 0;
    s.lastPort = clamp_int(::GetPrivateProfileIntW(kSection, L"lastPort", 7000, path.c_str()), 1, 65535);
    s.lastAddress = read_str(path, L"lastAddress", L"");
    // Kept exactly as written, even if that screen is not attached right now:
    // unplugging a TV for an evening should not forget the choice. Both the UI
    // and the capture path fall back to the primary screen until it is back.
    s.display = read_str(path, L"display", L"");
    return s;
}

void settings_save(const AppSettings& s) {
    const std::wstring path = settings_path();
    const wchar_t* lang = L"system";
    switch (s.lang) {
    case Lang::ZhHans: lang = L"zh-Hans"; break;
    case Lang::ZhHant: lang = L"zh-Hant"; break;
    case Lang::En: lang = L"en"; break;
    case Lang::System:
    default: lang = L"system"; break;
    }
    write_str(path, L"lang", lang);
    write_str(path, L"quality", std::to_wstring(clamp_int(s.quality, 0, kQualityCount - 1)));
    write_str(path, L"presetRev", L"2");
    write_str(path, L"systemAudio", s.systemAudio ? L"1" : L"0");
    write_str(path, L"lastAddress", s.lastAddress);
    write_str(path, L"display", s.display);
    write_str(path, L"lastPort", std::to_wstring(clamp_int(s.lastPort, 1, 65535)));
}

QualityPreset quality_preset(int index) {
    switch (index) {
    case 0: return {1280, 30};
    case 2: return {1920, 60};
    case 3: return {2560, 30};  // 1440p: the middle ground for a 4K desktop
    case 4: return {0, 30};     // native: capture.cpp treats 0 as "no downscale"
    case 1:
    default: return {1920, 30};
    }
}

} // namespace fc
