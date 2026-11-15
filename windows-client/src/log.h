// FlowCast Windows client - tiny leveled logger, no dependencies.
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace fc {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

// Sink receives the fully formatted line (no trailing newline).
using LogSink = void (*)(LogLevel level, const char* line, void* user);

// Returns milliseconds since an arbitrary but monotonic-ish origin.
double now_ms();

void log_init(LogLevel minLevel, LogSink sink, void* user);
void log_set_level(LogLevel level);
LogLevel log_level();
void log_write(LogLevel level, const char* fmt, ...);

// Formats into a caller buffer; always NUL-terminates.
void log_format(char* out, size_t cap, const char* fmt, ...);

const char* log_level_name(LogLevel level);

// --- file sink -------------------------------------------------------------
// The client is a GUI-subsystem exe: double-clicked from Explorer it has no
// console at all, so anything that goes wrong on someone else's machine leaves
// no trace behind. Every line is therefore also appended to a log file there.
//
// Default location: %LOCALAPPDATA%\FlowCast\flowcast.log (Next to the exe when
// LOCALAPPDATA is missing). Rotated once at 1 MiB, keeping one .old file, so a
// forgotten install cannot grow without bound.
std::wstring log_default_path();
// Opens (creating the directory) and truncates-if-rotated the file. Safe to
// call twice; the second call is a no-op. Returns false when the path is not
// writable -- logging carries on to the other sinks.
bool log_open_file(const std::wstring& path);
void log_close_file();
// The file currently in use, or empty when none was opened.
std::wstring log_file_path();

} // namespace fc

#define LOGD(...) ::fc::log_write(::fc::LogLevel::Debug, __VA_ARGS__)
#define LOGI(...) ::fc::log_write(::fc::LogLevel::Info, __VA_ARGS__)
#define LOGW(...) ::fc::log_write(::fc::LogLevel::Warn, __VA_ARGS__)
#define LOGE(...) ::fc::log_write(::fc::LogLevel::Error, __VA_ARGS__)
