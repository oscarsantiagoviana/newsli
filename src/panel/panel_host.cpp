// panel_host.cpp — the host side of the panel: GPU enumeration, the ctl
// v3 client (open mapping, 500 ms poll, batched Apply), the frame-scalars
// mapping that yields the engine counters and the game's own input values.
// All ctl calls run on the UI thread — the panel is a single ctl writer,
// so seq/ack never interleave.

#include "panel_internal.h"

#include <dxgi.h>
#include <tlhelp32.h>

HostView g_host;

// ---------------------------------------------------------------------------
// GPU enumeration: NVIDIA adapters only (VendorId 0x10DE), skipping the
// game's adapter when its LUID is known from the mirror — index N must
// mean the same adapter to the panel and to the host's
// PickSecondAdapterLuid, which counts non-game NVIDIA adapters the same
// way. No D3D device is created (POC parity).
// ---------------------------------------------------------------------------
void EnumGpus()
{
    HostView& h = g_host;
    h.gpuCount = 0;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return;
    for (UINT i = 0; h.gpuCount < 8; ++i)
    {
        IDXGIAdapter1* ad = nullptr;
        if (factory->EnumAdapters1(i, &ad) != S_OK)
            break;
        DXGI_ADAPTER_DESC1 d = {};
        const bool ok = SUCCEEDED(ad->GetDesc1(&d));
        ad->Release();
        if (!ok || d.VendorId != 0x10DE)
            continue;
        const bool isGame = h.gameLuidLo != 0 &&
            d.AdapterLuid.LowPart == (DWORD) h.gameLuidLo &&
            d.AdapterLuid.HighPart == (LONG) h.gameLuidHi;
        if (isGame)
            continue;
        GpuItem& g = h.gpus[h.gpuCount++];
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, g.name,
                            sizeof(g.name), nullptr, nullptr);
    }
    factory->Release();
}

bool OpenCtl()
{
    HostView& h = g_host;
    if (h.msg != nullptr)
        return true;
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
                                  sli::CTL_MAP_NAME);
    if (map == nullptr)
        return false;
    auto* m = reinterpret_cast<volatile sli::CtlMsg*>(
        MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(sli::CtlMsg)));
    if (m == nullptr)
    {
        CloseHandle(map);
        return false;
    }
    h.map = map;
    h.msg = m;
    // A foreign magic means a protocol the panel cannot speak: stay
    // read-only rather than corrupting a future host (ctl_common.h
    // forward-compat policy).
    h.incompatible = m->magic != sli::CTL_MAGIC;
    h.hostVersion = m->version;
    sli::Log("panel: ctl mapping opened (host v%u%s)",
             (unsigned) h.hostVersion, h.incompatible ? " INCOMPATIBLE" : "");
    Event("ctl mapping opened (host protocol v%u)",
          (unsigned) h.hostVersion);
    EnumGpus();
    return true;
}

void CloseCtl()
{
    HostView& h = g_host;
    if (h.scalars != nullptr) { UnmapViewOfFile((void*) h.scalars); h.scalars = nullptr; }
    if (h.frameMap != nullptr) { CloseHandle(h.frameMap); h.frameMap = nullptr; }
    if (h.msg != nullptr) { UnmapViewOfFile((void*) h.msg); h.msg = nullptr; }
    if (h.map != nullptr) { CloseHandle(h.map); h.map = nullptr; }
    h.countersValid = false;
    h.hostLive = false;
}

// The host's per-frame scalars mapping is named after the HOST pid, which
// the ctl mirror does not carry. enginePid does: the engine was spawned by
// the host, so its parent pid IS the host pid (toolhelp walk; no device,
// no privileges beyond process listing).
bool OpenFrameScalars()
{
    HostView& h = g_host;
    if (h.scalars != nullptr || h.enginePid == 0)
        return h.scalars != nullptr;
    DWORD hostPid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe))
        {
            do
            {
                if (pe.th32ProcessID == h.enginePid)
                {
                    hostPid = pe.th32ParentProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    if (hostPid == 0)
        return false;
    wchar_t name[64] = {};
    sli::FrameMapName(hostPid, name, 64);
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (map == nullptr)
        return false;
    auto* sc = reinterpret_cast<volatile sli::FrameScalars*>(
        MapViewOfFile(map, FILE_MAP_READ, 0, 0, sli::SCALARS_SIZE));
    if (sc == nullptr)
    {
        CloseHandle(map);
        return false;
    }
    h.frameMap = map;
    h.scalars = sc;
    sli::Log("panel: frame scalars opened (host pid %u)", (unsigned) hostPid);
    return true;
}

// The panel is a single ctl writer — seq/ack never interleave (all calls
// run on the UI thread).
//
// 500 ms poll: PING (verb get — the host rebuilds the mirror and acks) +
// snapshot the mirror + heartbeat freshness + engine counters.
static void PollHost()
{
    HostView& h = g_host;
    if (!OpenCtl())
        return;

    volatile sli::CtlMsg* m = h.msg;

    // heartbeat freshness: hb is the host's GetTickCount lo32 — the same
    // system clock, valid cross-process, wrap-safe in DWORD arithmetic.
    const DWORD now32 = GetTickCount();
    h.hostLive = !h.incompatible && (now32 - m->hb) < 2000;
    h.hostVersion = m->version;
    h.enginePid = m->enginePid;

    // outstanding ping resolution happens every frame in PingCheck()
    // (a pending ping can also be superseded by our own Apply, which
    // bumps seq past it — the host acts on the LATEST seq only)

    if (h.hostLive)
    {
        // fire the next PING+GET (verb get): payload is untouched, only
        // seq moves — the host applies nothing, rebuilds the mirror and
        // echoes seq.
        if (!h.pingPending && !h.incompatible)
        {
            h.pingSeq = m->seq + 1;
            h.pingPending = true;
            h.pingSentAt = GetTickCount64();
            m->verb = 1;  // get
            InterlockedExchange((volatile LONG*) &m->seq, (LONG) h.pingSeq);
        }
        for (int f = 0; f < (int) sli::CTL_FIELD_COUNT; ++f)
        {
            h.mirror[f] = m->mirror[f];
            h.mirrorSource[f] = m->mirrorSource[f];
        }
        const unsigned long long glLo = m->mirrorGameLuidLo;
        const unsigned long long glHi = m->mirrorGameLuidHi;
        const bool luidChanged =
            glLo != h.gameLuidLo || glHi != h.gameLuidHi;
        h.gameLuidLo = glLo;
        h.gameLuidHi = glHi;
        if (luidChanged && glLo != 0)
        {
            // the game's adapter is now known: the GPU list was enumerated
            // BEFORE (panel start, host maybe absent) and still contains
            // the game's own GPU — while the host's PickSecondAdapterLuid
            // counts non-game only. Combo index N would arm a DIFFERENT
            // adapter than the one the row shows. Re-enumerate with the
            // same rule the host uses.
            EnumGpus();
            Event("game GPU identified — GPU list re-filtered");
        }

        if (OpenFrameScalars() && h.scalars != nullptr)
        {
            const volatile sli::FrameScalars& fs = *h.scalars;
            h.produceFrame = fs.frame;
            h.prevDoneFrame = h.doneFrame;
            h.doneFrame = fs.engineHostFrame;
            h.countersValid = true;
            // spec status derivation: engineResult==1 AND the done
            // counter advancing between polls
            h.engineLive = fs.engineResult == 1 &&
                           h.doneFrame > h.prevDoneFrame;
        }
        else
        {
            h.countersValid = false;
            h.engineLive = false;
        }
        const bool liveNow = h.hostLive && h.engineLive;
        if (liveNow && !h.wasLive)
            Event("engine live: deltas composing (pid %u)",
                  (unsigned) h.enginePid);
        h.wasLive = liveNow;
    }
    else
    {
        h.engineLive = false;
        h.wasLive = false;
    }
}

// Resolve a pending ping the moment the ack shows up (called EVERY frame
// — the host answers within ~25 ms, and waiting for the next 500 ms poll
// to notice would report the cadence, not the latency).
static void PingCheck()
{
    HostView& h = g_host;
    if (!h.pingPending || h.msg == nullptr)
        return;
    const int32_t rel = (int32_t) (h.msg->ack - h.pingSeq);
    if (rel >= 0)
    {
        h.pingPending = false;
        // an Apply batch bumps seq past our ping: the ack we just saw is
        // the BATCH's, not the ping's — measuring it would report the
        // batch RTT as the ping (R82l honesty fix)
        if (h.msg->ack != h.pingSeq)
        {
            h.pingMs = 0;
            return;
        }
        h.pingMs = (DWORD) (GetTickCount64() - h.pingSentAt);
    }
    else if (GetTickCount64() - h.pingSentAt > 2000)
    {
        h.pingPending = false;
        h.pingMs = 0;
        Event("!PING seq %u: no ack in 2 s (host busy or gone)",
              (unsigned) h.pingSeq);
    }
}

void PollHostIfDue(ULONGLONG& nextPoll)
{
    PingCheck();
    if (GetTickCount64() >= nextPoll)
    {
        PollHost();
        nextPoll = GetTickCount64() + 500;  // spec cadence
    }
}

// One batched SET (up to CTL_MAX_BATCH pairs). Blocking but bounded: a
// fresh heartbeat means the host's 25 ms poll thread will answer; anything
// slower than 300 ms is a failure instead of freezing the UI for the
// POC's worst-case 48 s.
static bool SendBatch(const uint32_t* fields, const float* values, int count,
                      DWORD& ackMs)
{
    HostView& h = g_host;
    volatile sli::CtlMsg* m = h.msg;
    if (m == nullptr || h.incompatible)
        return false;
    const ULONGLONG t0 = GetTickCount64();
    m->verb = 4;  // set-batch
    m->batchCount = (uint32_t) count;
    for (int i = 0; i < count && i < (int) sli::CTL_MAX_BATCH; ++i)
    {
        m->batchField[i] = fields[i];
        m->batchValue[i] = values[i];
    }
    const uint32_t seq = m->seq + 1;
    // seq moves last: the host triggers on the seq change, so the payload
    // must be fully visible before it (InterlockedExchange is a barrier).
    InterlockedExchange((volatile LONG*) &m->seq, (LONG) seq);
    while (GetTickCount64() - t0 < 300)
    {
        if (m->ack == seq)
        {
            ackMs = (DWORD) (GetTickCount64() - t0);
            return true;
        }
        Sleep(5);
    }
    ackMs = (DWORD) (GetTickCount64() - t0);
    return false;
}

// Apply the WHOLE panel state (idempotent full-state apply, POC parity —
// 21 ctl-mapped rows in ceil(21/8) = 3 batch messages, not per-field
// round-trips).
void ApplyAll()
{
    HostView& h = g_host;
    if (h.msg == nullptr || !h.hostLive || h.incompatible)
    {
        // POC R69u behavior minus the modal: no host, nothing sent —
        // but the SAVE half still runs (R89f: Apply is Apply+Save), so
        // the config persists for the next arm instead of dying in RAM.
        if (SaveIni())
            Event("!Apply: host not live — nothing sent, but SAVED to "
                  "sli.ini (the host loads it when it arms)");
        else
            Event("!Apply: host not live — nothing sent and SAVE FAILED "
                  "(disk/perm?)");
        return;
    }
    uint32_t fields[CTL_MAX_BATCH];
    float values[CTL_MAX_BATCH];
    int n = 0, chunks = 0, sent = 0;
    DWORD totalMs = 0;
    const auto flush = [&]() -> bool {
        if (n == 0)
            return true;
        DWORD ms = 0;
        if (!SendBatch(fields, values, n, ms))
        {
            Event("!Apply batch %d: no ack in %u ms — rest not sent",
                  chunks + 1, (unsigned) ms);
            return false;
        }
        totalMs += ms;
        sent += n;
        ++chunks;
        n = 0;
        return true;
    };
    for (int i = 0; i < kNumControls; ++i)
    {
        if (kSpecs[i].ctlField <= 0)
            continue;  // local-only rows never hit the wire
        fields[n] = (uint32_t) kSpecs[i].ctlField;
        values[n] = SendValue(i);
        if (++n == (int) sli::CTL_MAX_BATCH && !flush())
            return;
    }
    if (!flush())
        return;
    memcpy(g_st.applied, g_st.value, sizeof(g_st.applied));
    g_st.appliedValid = true;
    // R89f (user request): Apply IS Apply+Save — one button, one intent.
    // The save persists for the next (re)arm even when the live batch
    // could not run (no host), so the config never lives only in RAM.
    if (SaveIni())
        Event("Apply: %d fields in %d batch(es), acked in %u ms — "
              "saved to sli.ini", sent, chunks, (unsigned) totalMs);
    else
        Event("!Apply sent (%d fields) but SAVE FAILED - sli.ini not "
              "written (disk/perm?)", sent);
}
