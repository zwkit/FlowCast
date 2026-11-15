// FlowCast Windows client - UTF-8 <-> UTF-16 helpers.
//
// The wire format (plists, mDNS labels) is UTF-8 while every Win32 call wants
// UTF-16, so the two conversions live here instead of being open-coded.
#pragma once
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace fc {

inline std::string utf8_from_wide(const wchar_t* w, int len) {
    if (!w) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, len, &s[0], n, nullptr, nullptr);
    if (len < 0 && !s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

inline std::string utf8_from_wide(const wchar_t* w) { return utf8_from_wide(w, -1); }
inline std::string utf8_from_wide(const std::wstring& w) {
    return utf8_from_wide(w.c_str(), (int)w.size());
}

inline std::wstring wide_from_utf8(const char* s, int len) {
    if (!s || len == 0) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s, len, &w[0], n);
    return w;
}

inline std::wstring wide_from_utf8(const std::string& s) {
    return wide_from_utf8(s.data(), (int)s.size());
}

} // namespace fc
