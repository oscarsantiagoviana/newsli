// sli_log.cpp — implementation of the shared logging + crash handler.
#include "shared/log.h"
#include <windows.h>
#include <psapi.h>   // EnumProcessModules for the crash backtrace (K32*
                     // variants need no Psapi.lib link, but the header is
                     // required for the prototypes)
#include <dbghelp.h>
#include <cstdio>
#include <cstring>

namespace sli {

namespace {

struct LogState
{
    CRITICAL_SECTION cs;
    char     path[MAX_PATH];
    uint32_t maxBytes  = 10 * 1024 * 1024;
    uint32_t keepFiles = 3;
    bool     init      = false;
};
LogState g_log;

void RotateIfNeeded()
{
    // Rotate when the active file exceeds the budget:
    //   drop file.N, rename file.(N-1) -> file.N, ..., active -> file.1
    HANDLE f = CreateFileA(g_log.path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ |
                           FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    CloseHandle(f);
    if ((uint64_t) sz.QuadPart < g_log.maxBytes) return;

    char from[MAX_PATH], to[MAX_PATH];
    _snprintf_s(to, MAX_PATH, _TRUNCATE, "%s.%u", g_log.path, g_log.keepFiles);
    DeleteFileA(to); // drop the oldest backup
    for (uint32_t i = g_log.keepFiles - 1; i >= 1; --i)
    {
        _snprintf_s(from, MAX_PATH, _TRUNCATE, "%s.%u", g_log.path, i);
        _snprintf_s(to,   MAX_PATH, _TRUNCATE, "%s.%u", g_log.path, i + 1);
        MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING);
    }
    _snprintf_s(to, MAX_PATH, _TRUNCATE, "%s.1", g_log.path);
    MoveFileExA(g_log.path, to, MOVEFILE_REPLACE_EXISTING);
}

} // namespace

void LogInit(const char* path, uint32_t maxBytes, uint32_t keepFiles)
{
    if (!g_log.init)
    {
        InitializeCriticalSection(&g_log.cs);
        g_log.init = true;
    }
    EnterCriticalSection(&g_log.cs);
    strncpy_s(g_log.path, path, _TRUNCATE);
    g_log.maxBytes  = maxBytes  ? maxBytes  : 10 * 1024 * 1024;
    g_log.keepFiles = keepFiles ? keepFiles : 3;
    LeaveCriticalSection(&g_log.cs);
}

void LogLine(const char* line)
{
    if (!g_log.init) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[640];
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "[%02u:%02u:%02u.%03u] %s\r\n",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
    if (n <= 0) return;
    EnterCriticalSection(&g_log.cs);
    RotateIfNeeded();
    HANDLE f = CreateFileA(g_log.path, FILE_APPEND_DATA, FILE_SHARE_READ |
                           FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(f, buf, (DWORD) n, &written, nullptr);
        CloseHandle(f);
    }
    LeaveCriticalSection(&g_log.cs);
}

// R91b: the due-check extracted so LogRate can consult it BEFORE paying
// for _snprintf_s (see log.h). Same table, same CS, same semantics:
// returns true exactly when the window has elapsed — and CLAIMS it.
bool RateDue(uint32_t ms, const char* key)
{
    struct RateEntry { const char* key; uint32_t last; };
    static RateEntry table[64];
    static int used = 0;
    const uint32_t now = GetTickCount();
    EnterCriticalSection(&g_log.cs);
    for (int i = 0; i < used; ++i)
        if (table[i].key == key)
        {
            if (now - table[i].last < ms)
            {
                LeaveCriticalSection(&g_log.cs);
                return false;
            }
            table[i].last = now;
            LeaveCriticalSection(&g_log.cs);
            return true;
        }
    if (used < 64) { table[used] = { key, now }; ++used; }
    LeaveCriticalSection(&g_log.cs);
    return true;
}

void LogRateLine(uint32_t ms, const char* key, const char* line)
{
    // R91b BUGFIX: no second RateDue here — LogRate already claimed the
    // window BEFORE formatting (that's the whole point of the gate-first
    // shape); re-checking denied every line forever (double gate = the
    // first call claims, the second denies). Direct LogRateLine callers,
    // if any appear, do their own gating.
    (void) ms; (void) key;
    LogLine(line);
}

// ---------------------------------------------------------------------------
// Crash handler: minidump + first frames of context into the log.
// ---------------------------------------------------------------------------
// Format the module (and offset) a code address belongs to, using the loader
// list. Crash-context safe: no heap, bounded output.
static void CrashFormatModule(unsigned long long addr, char* out, size_t outCap)
{
    out[0] = '\0';
    if (!addr) { _snprintf_s(out, outCap, _TRUNCATE, "null"); return; }
    HMODULE mods[128];
    DWORD cb = 0;
    // K32EnumProcessModules works from the crash context (no Psapi.lib
    // link dependency; the psapi.h header only supplies prototypes).
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &cb) || !cb)
        { _snprintf_s(out, outCap, _TRUNCATE, "0x%llX (enum failed)", addr); return; }
    DWORD n = cb / sizeof(HMODULE);
    HMODULE best = nullptr;
    for (DWORD i = 0; i < n; ++i)
    {
        if ((uintptr_t)mods[i] <= (uintptr_t)addr)
            if (!best || (uintptr_t)mods[i] > (uintptr_t)best)
                best = mods[i];
    }
    if (!best) { _snprintf_s(out, outCap, _TRUNCATE, "0x%llX (no module)", addr); return; }
    char name[MAX_PATH];
    if (GetModuleFileNameA(best, name, MAX_PATH))
    {
        const char* base = strrchr(name, '\\');
        base = base ? base + 1 : name;
        _snprintf_s(out, outCap, _TRUNCATE, "%s+0x%llX", base,
                    addr - (unsigned long long)(uintptr_t)best);
    }
    else
        _snprintf_s(out, outCap, _TRUNCATE, "mod 0x%p+0x%llX", best,
                    addr - (unsigned long long)(uintptr_t)best);
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    // Log FIRST (never depends on MiniDumpWriteDump/dbgcore surviving): exception
    // code, faulting address, RIP module, AV read/write, and the return addresses
    // on the stack that resolve to our modules. THEN attempt the dump.
    if (g_log.init && ep)
    {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        char mod[128];
        CrashFormatModule((unsigned long long)(uintptr_t)
                              (er ? er->ExceptionAddress : nullptr),
                          mod, sizeof(mod));
        Log("UNHANDLED EXCEPTION code=0x%08X addr=0x%llX in %s",
            er ? er->ExceptionCode : 0,
            er ? (unsigned long long)(uintptr_t)er->ExceptionAddress : 0ull, mod);
#if defined(_M_X64) || defined(_WIN64)
        if (er && er->ExceptionCode == 0xC0000005 && er->NumberParameters >= 2)
            Log("  AV %s at 0x%llX",
                er->ExceptionInformation[0] == 0 ? "read" : "write",
                (unsigned long long)er->ExceptionInformation[1]);
        if (ep->ContextRecord)
        {
            CrashFormatModule(ep->ContextRecord->Rip, mod, sizeof(mod));
            Log("  RIP 0x%llX in %s",
                (unsigned long long)ep->ContextRecord->Rip, mod);
            // Walk the raw stack for return addresses inside our own modules —
            // identifies our frame even without symbols.
            const unsigned long long* sp =
                (const unsigned long long*)ep->ContextRecord->Rsp;
            MEMORY_BASIC_INFORMATION mbi;
            int logged = 0;
            for (int i = 0; i < 512 && logged < 6; ++i)
            {
                if (!VirtualQuery(sp + i, &mbi, sizeof(mbi))) break;
                if (mbi.State != MEM_COMMIT) break;
                if (mbi.Protect & (PAGE_READONLY|PAGE_READWRITE|PAGE_EXECUTE_READ|
                                   PAGE_EXECUTE_READWRITE))
                {
                    // readable qword; check if it points into a module image
                    unsigned long long v = sp[i];
                    if (v > 0x10000 && v < 0x7FFFFFFFFFFF)
                    {
                        char m2[128];
                        CrashFormatModule(v, m2, sizeof(m2));
                        if (strstr(m2, "nvngx") || strstr(m2, "sli"))
                        {
                            Log("  stack[0x%03X] -> %s", (unsigned)(i * 8), m2);
                            ++logged;
                        }
                    }
                }
                else break;
            }
        }
#endif
    }
    char dumpPath[MAX_PATH];
    _snprintf_s(dumpPath, MAX_PATH, _TRUNCATE, "%ssli_crash_%u.dmp",
                g_log.init ? "" : ".", GetCurrentProcessId());
    // Best-effort: write the dump beside the module's log path if known.
    if (g_log.init)
    {
        const char* slash = strrchr(g_log.path, '\\');
        if (slash)
        {
            size_t base = (size_t)(slash - g_log.path) + 1;
            memcpy(dumpPath, g_log.path, base);
            _snprintf_s(dumpPath + base, MAX_PATH - base, _TRUNCATE,
                        "sli_crash_%u.dmp", GetCurrentProcessId());
        }
    }
    HANDLE f = CreateFileA(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei{ GetCurrentThreadId(), ep, FALSE };
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                          MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(f);
    }
    if (g_log.init)
    {
        // TryEnter: if another thread holds the log CS at crash time,
        // blocking here would deadlock the dump — skip the log line instead
        // (the minidump above is the evidence that matters).
        if (TryEnterCriticalSection(&g_log.cs))
        {
            RotateIfNeeded();
            HANDLE lf = CreateFileA(g_log.path, FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
            if (lf != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                char buf[128];
                int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                                    "UNHANDLED EXCEPTION dump=%s\r\n",
                                    dumpPath);
                WriteFile(lf, buf, (DWORD) n, &written, nullptr);
                CloseHandle(lf);
            }
            LeaveCriticalSection(&g_log.cs);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void CrashHandlerInstall(const char* dumpDir)
{
    (void) dumpDir;  // the dump lands beside the log file, not here
    SetUnhandledExceptionFilter(CrashFilter);
}

} // namespace sli
