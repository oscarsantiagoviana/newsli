// test_log — F1 gate: rotation by size works; crash handler writes a dump.
#include "shared/log.h"
#include <cstdio>
#include <cstring>

static bool Exists(const char* p) { FILE* f = nullptr; if (fopen_s(&f, p, "rb") == 0 && f) { fclose(f); return true; } return false; }

int main()
{
    const char* dir = "logtest";
    CreateDirectoryA(dir, nullptr);
    const char* p = "logtest\\t.log";
    DeleteFileA(p); DeleteFileA("logtest\\t.log.1"); DeleteFileA("logtest\\t.log.2");

    sli::LogInit(p, 1024, 2);           // rotate every KB, keep 2 backups
    for (int i = 0; i < 100; ++i)
        sli::Log("line %04d padpadpadpadpadpadpadpadpadpadpadpadpadpadpad", i);

    bool ok = Exists(p) && Exists("logtest\\t.log.1") && Exists("logtest\\t.log.2");
    FILE* fa = nullptr; fopen_s(&fa, "logtest\\t.log", "rb");
    long active = 0;
    if (fa) { fseek(fa, 0, SEEK_END); active = ftell(fa); fclose(fa); }
    // active file must be under budget (+one line overshoot allowed)
    ok = ok && active < 1024 + 640;
    printf("rotation: active=%ld keep1=%d keep2=%d -> %s\n",
           active, Exists("logtest\\t.log.1"), Exists("logtest\\t.log.2"), ok ? "PASS" : "FAIL");

#ifndef T_LOG_NO_CRASH
    sli::CrashHandlerInstall(dir);
    // Deliberate AV to prove the dump path. This part runs ONLY in the
    // t_crash target (WILL_FAIL: a crash IS its expected result). The
    // rotation asserts above must fail LOUDLY in t_log — they are no longer
    // masked by an inverted pass condition (R82l).
    volatile int* bad = (int*) 16;
    *bad = 1;
#endif
    return ok ? 0 : 1;
}
