// FlowCast Windows client - leveled logger implementation.
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <string>

namespace fc {

namespace {
LogLevel g_minLevel = LogLevel::Info;
LogSink g_sink = nullptr;
void* g_user = nullptr;
std::mutex g_mu;

// File sink. Kept open for the life of the process and flushed per line: the
// interesting case is a session that dies, and a buffered tail would be lost
// exactly then.
FILE* g_file = nullptr;
std::wstring g_filePath;

constexpr uint64_t kRotateAt = 1024ull * 1024ull;

std::wstring local_appdata() {
    const DWORD need = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (need == 0) return std::wstring();
    std::wstring buf(need, L'\0');
    const DWORD got = ::GetEnvironmentVariableW(L"LOCALAPPDATA", &buf[0], need);
    if (got == 0 || got >= need) return std::wstring();
    buf.resize(got);
    return buf;
}

// Creates every missing component of `dir`, parents included.
void make_dirs(const std::wstring& dir) {
    if (dir.empty()) return;
    for (size_t i = 1; i < dir.size(); ++i) {
        if (dir[i] != L'\\' && dir[i] != L'/') continue;
        const std::wstring part = dir.substr(0, i);
        if (::GetFileAttributesW(part.c_str()) == INVALID_FILE_ATTRIBUTES) {
            ::CreateDirectoryW(part.c_str(), nullptr);
        }
    }
    if (::GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ::CreateDirectoryW(dir.c_str(), nullptr);
    }
}

uint64_t file_size(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}
} // namespace

double now_ms() {
    // QueryUnbiasedInterruptTime excludes time the machine spends asleep,
    // which is what a media sender wants for pacing.
    ULONGLONG ticks100ns = 0;
    QueryUnbiasedInterruptTime(&ticks100ns);
    return (double)ticks100ns / 10000.0;
}

const char* log_level_name(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO ";
    case LogLevel::Warn:  return "WARN ";
    case LogLevel::Error: return "ERROR";
    default:              return "     ";
    }
}

static void default_sink(LogLevel level, const char* line, void*) {
    FILE* out = (level == LogLevel::Error || level == LogLevel::Warn) ? stderr : stdout;
    fprintf(out, "%s %s\n", log_level_name(level), line);
    fflush(out);
}

void log_init(LogLevel minLevel, LogSink sink, void* user) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_minLevel = minLevel;
    g_sink = sink ? sink : default_sink;
    g_user = user;
}

void log_set_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_minLevel = level;
}

LogLevel log_level() {
    return g_minLevel;
}

std::wstring log_default_path() {
    std::wstring base = local_appdata();
    if (base.empty()) {
        // No profile directory (a very stripped-down account): fall back to the
        // folder the exe sits in. A portable copy on a USB stick wants that
        // anyway, and it is the only option left when LOCALAPPDATA is unset.
        wchar_t exe[MAX_PATH]{};
        const DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring p(exe, n);
        const size_t slash = p.find_last_of(L"\\/");
        base = (slash == std::wstring::npos) ? std::wstring(L".") : p.substr(0, slash);
        return base + L"\\flowcast.log";
    }
    return base + L"\\FlowCast\\flowcast.log";
}

bool log_open_file(const std::wstring& path) {
    if (path.empty()) return false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_file) return true;
    }

    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) make_dirs(path.substr(0, slash));

    // One generation of history. A log that is never trimmed eventually ends up
    // on a support ticket as a 200 MB attachment nobody can open.
    if (file_size(path) > kRotateAt) {
        const std::wstring old = path + L".old";
        ::DeleteFileW(old.c_str());
        ::MoveFileW(path.c_str(), old.c_str());
    }

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a") != 0 || !f) return false;

    std::lock_guard<std::mutex> lock(g_mu);
    if (g_file) { // another thread won the race
        fclose(f);
        return true;
    }
    g_file = f;
    g_filePath = path;

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    fprintf(g_file, "\n==== FlowCast client %04u-%02u-%02u %02u:%02u:%02u ====\n", st.wYear,
            st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fflush(g_file);
    return true;
}

void log_close_file() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    g_filePath.clear();
}

std::wstring log_file_path() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_filePath;
}

void log_format(char* out, size_t cap, const char* fmt, ...) {
    if (!out || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    if (n < 0) out[0] = '\0';
    else if ((size_t)n >= cap) out[cap - 1] = '\0';
}

void log_write(LogLevel level, const char* fmt, ...) {
    if (level < g_minLevel) return;
    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    char line[2112];
    log_format(line, sizeof(line), "[%9.3f] %s", now_ms() / 1000.0, body);
    std::lock_guard<std::mutex> lock(g_mu);
    // Falling back here rather than at init time means a caller that only
    // adjusts the level (the GUI installs its own sink, the CLI does not) still
    // gets console output.
    (g_sink ? g_sink : default_sink)(level, line, g_user);
    if (g_file) {
        fprintf(g_file, "%s %s\n", log_level_name(level), line);
        fflush(g_file);
    }
}

} // namespace fc
