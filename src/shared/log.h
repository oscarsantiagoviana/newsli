// sli_log.h — logging with size rotation, plus a crash handler that writes
// a minidump. Ported from the POC's host_log.h / engine crash_log.h and
// upgraded for publication: rotation by size (the POC's append-only log hit
// hundreds of MB), compile-time-checked formatting (the R33 stack-garbage
// crash), and MiniDumpWriteDump on any unhandled exception.
#pragma once
#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstdint>

namespace sli {

// Initialize logging for this module. `path` is the full log file path;
// rotation keeps `path`, `path.1`, ..., `path.N` each under `maxBytes`.
// Must be called once before any Log() call; safe to call again (re-open).
void LogInit(const char* path, uint32_t maxBytes = 10 * 1024 * 1024,
             uint32_t keepFiles = 3);

// Core append; timestamps every line. NOT throttled — see LogRate.
void LogLine(const char* line);

// printf-style logging with compile-time format checking (C2338-style
// mismatches are compile errors, not live crashes — POC lesson R33).
template <typename... Args>
void Log(const char* fmt, Args&&... args)
{
    char buf[512];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args...);
    LogLine(buf);
}

// Rate-limited logging: the same call-site logs at most once per `ms`
// window (per unique fmt pointer — constants dedupe in rodata).
// R91b: RateDue is consulted BEFORE any formatting — the old shape ran
// _snprintf_s on every call and discarded the result (the throttle
// checked nothing until after the CPU work); hot-path call-sites paid
// the format even when the line never shipped.
bool RateDue(uint32_t ms, const char* key);
template <typename... Args>
void LogRate(uint32_t ms, const char* fmt, Args&&... args)
{
    if (!RateDue(ms, fmt))
        return;
    char buf[512];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args...);
    LogRateLine(ms, fmt, buf);
}
void LogRateLine(uint32_t ms, const char* key, const char* line);

// Crash handler: installs an unhandled-exception filter that writes
// `sli_crash_<pid>.dmp` beside the log file (the dumpDir argument is
// accepted but ignored — kept for call-site compatibility) and a
// backtrace line into the log. Returns void; chaining a previous filter
// is not supported (the minidump carries the full picture).
void CrashHandlerInstall(const char* dumpDir);

} // namespace sli
