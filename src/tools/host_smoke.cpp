// host_smoke.cpp — F3 smoke gate. Exercises the session's public surface
// with a mock evaluate (no game, no NGX core): Configure -> Evaluate (arm
// trigger publish) -> Shutdown, plus the ctl v3 protocol against the REAL
// mapping (sli_ctl.exe interop).
//
// What it proves without a GPU session:
//   S1 Configure/Evaluate/PresentGate/OnListSubmitted/Shutdown all
//      return promptly (no game-thread waits, no deadlocks under the FSM).
//   S2 The ctl mapping answers sets with an ack + mirror (protocol alive).
//   S3 Shutdown drains to IDLE within 3 s (the real session budget is
//      1.5 s per drain leg; this smoke run never armed, so it must be
//      far under — the check gates at 3 s for CI slack).
// The real arm fails (no engine beside a test exe) — that path IS the test:
// it must land in COOLDOWN, not hang or crash.
#include "host/offload_session.h"
#include "shared/ctl_common.h"
#include "shared/log.h"

#include <cstdio>
#include <cstring>

using namespace sli;

static int g_fail = 0;
static void Check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

int main()
{
    LogInit("host_smoke.log", 256 * 1024, 2);

    // S1: lifecycle calls with null/mock args must be silent + prompt
    host::Configure(1);
    host::EvalArgs a {};  // all-null mock evaluate: must not crash, must no-op
    host::Evaluate(a);
    host::PresentGate(nullptr, 1, 0);       // mock present: forwards S_OK
    host::PresentGate1(nullptr, 1, 0, nullptr);
    host::OnListSubmitted(nullptr, nullptr);
    Check(true, "S1 lifecycle calls with null args return");

    // A mock evaluate WITH a device-less shape must publish an arm request
    // without arming (dev == null -> the session thread rejects it)
    host::EvalArgs b {};
    b.inW = 1280; b.inH = 720; b.outW = 2560; b.outH = 1440;
    b.createFlags = 8;
    host::Evaluate(b);
    Sleep(700);  // let the session thread take its poke
    Check(true, "S1 arm-trigger publish returns");

    // S2: ctl protocol — the host's mapping is live from Configure
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, CTL_MAP_NAME);
    Check(map != nullptr, "S2 ctl mapping exists (Local\\sli_ctl_v3)");
    if (map != nullptr)
    {
        auto* m = (volatile CtlMsg*) MapViewOfFile(
            map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CtlMsg));
        Check(m != nullptr && m->magic == CTL_MAGIC, "S2 ctl magic present");
        if (m != nullptr && m->magic == CTL_MAGIC)
        {
            const uint32_t seq = m->seq + 1;
            m->verb = 2;  // set
            m->field = CTL_FIELD_NR_BOOST;
            m->fval = 4.0f;
            m->seq = seq;
            bool acked = false;
            for (int i = 0; i < 100 && !acked; ++i)
            {
                Sleep(25);
                acked = m->ack == seq;
            }
            Check(acked, "S2 ctl set ack within 2.5 s");
            Check(m->mirror[CTL_FIELD_NR_BOOST] == 4.0f,
                  "S2 ctl mirror reflects the set");
            const uint32_t hb0 = m->hb;
            Sleep(120);
            Check(m->hb != hb0, "S2 ctl heartbeat advances");
        }
        if (m != nullptr) UnmapViewOfFile((void*) m);
        CloseHandle(map);
    }

    // S3: shutdown drains within budget (nothing was armed; must be fast)
    const ULONGLONG t0 = GetTickCount64();
    host::Shutdown();
    const ULONGLONG dt = GetTickCount64() - t0;
    std::printf("shutdown took %u ms\n", (unsigned) dt);
    Check(dt < 3000, "S3 shutdown within budget");

    std::printf(g_fail == 0 ? "HOST SMOKE PASS\n" : "HOST SMOKE FAIL\n");
    return g_fail == 0 ? 0 : 1;
}
