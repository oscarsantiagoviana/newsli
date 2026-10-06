// sli_engine — NR worker on the secondary GPU: args, device on the target
// adapter, shared transport, then the frame loop. Port of the POC's
// engine_main.cpp with the SR/replay legacy deleted: this process runs NO
// upscaler at all — the game's native NGX does, in the game's process. The
// engine's only output is the display-domain GAIN; when the vendor model
// is not armed it delivers UNITY (1.0) — the host composes base * 1.0 =
// the untouched game frame.
#include "engine/engine_ctx.h"
#include "shared/log.h"

#include <shellapi.h>

static EngineCtx g_ctx;

// --luid <lo> <hi> --w <px> --h <px> --cf <dxgi> --map <name>  (that's all)
static bool ParseEngineArgs(int argc, wchar_t** argv, EngineArgs& a)
{
    for (int i = 1; i < argc; ++i)
    {
        const wchar_t* k = argv[i];
        auto val = [&]() -> const wchar_t* {
            return i + 1 < argc ? argv[i + 1] : nullptr;
        };
        if (!wcscmp(k, L"--luid") && i + 2 < argc)
        {
            a.luidLo = wcstoull(argv[i + 1], nullptr, 0);
            a.luidHi = wcstoull(argv[i + 2], nullptr, 0);
            i += 2;
        }
        else if (!wcscmp(k, L"--w") && val())  a.w  = (unsigned) wcstoul(val(), nullptr, 0), ++i;
        else if (!wcscmp(k, L"--h") && val())  a.h  = (unsigned) wcstoul(val(), nullptr, 0), ++i;
        else if (!wcscmp(k, L"--cf") && val()) a.cf = (unsigned) wcstoul(val(), nullptr, 0), ++i;
        else if (!wcscmp(k, L"--map") && val()) a.mapName = val(), ++i;
        else
        {
            sli::Log("unknown/missing arg %ls", k);
            return false;
        }
    }
    // A missing --luid in live mode means the spawner is broken; falling
    // back to "first NVIDIA adapter" would silently bind the engine to
    // the GAME's GPU. Fail instead.
    if (!(a.luidLo || a.luidHi) || !a.w || !a.h || !a.cf || a.mapName.empty())
    {
        sli::Log("validation: luid=%08llX-%08llX w=%u h=%u cf=%u map=%ls",
                 (unsigned long long) a.luidHi, (unsigned long long) a.luidLo,
                 a.w, a.h, a.cf, a.mapName.c_str());
        return false;
    }
    return true;
}

static bool PickAdapter(EngineCtx& c)
{
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        sli::Log("no factory");
        return false;
    }
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> ad;
        if (factory->EnumAdapters1(i, ad.ReleaseAndGetAddressOf()) != S_OK)
            break;
        DXGI_ADAPTER_DESC1 d {};
        if (FAILED(ad->GetDesc1(&d)) || d.VendorId != 0x10DE)
            continue;
        if (d.AdapterLuid.LowPart == (DWORD) c.args.luidLo &&
            d.AdapterLuid.HighPart == (LONG) c.args.luidHi)
        {
            char name[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name),
                                nullptr, nullptr);
            sli::Log("target adapter: %s", name);
            if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_12_0,
                                         IID_PPV_ARGS(&c.dev))))
            {
                sli::Log("D3D12CreateDevice failed");
                return false;
            }
            return true;
        }
    }
    sli::Log("adapter LUID %08llX-%08llX not found",
             (unsigned long long) c.args.luidHi, (unsigned long long) c.args.luidLo);
    return false;
}

static bool SetupDevice(EngineCtx& c)
{
    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(c.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&c.queue))) ||
        FAILED(c.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&c.alloc))) ||
        FAILED(c.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        c.alloc.Get(), nullptr,
                                        IID_PPV_ARGS(&c.cmd))) ||
        FAILED(c.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                  IID_PPV_ARGS(&c.localFence))))
    {
        sli::Log("queue/alloc/list/fence failed");
        return false;
    }
    c.cmd->Close();
    c.localFenceEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return c.localFenceEvt != nullptr;
}

int wmain(int argc, wchar_t** argv)
{
    // Log beside the exe (the host spawns us with the game dir as cwd);
    // crash dumps land there too.
    wchar_t wdir[MAX_PATH] {};
    GetModuleFileNameW(nullptr, wdir, MAX_PATH);
    wchar_t* slash = wcsrchr(wdir, L'\\');
    if (slash) *slash = 0;
    char logPath[MAX_PATH] = "sli_engine.log";
    {
        char dirA[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, wdir, -1, dirA, sizeof(dirA),
                            nullptr, nullptr);
        _snprintf_s(logPath, MAX_PATH, _TRUNCATE, "%s\\sli_engine.log", dirA);
    }
    sli::LogInit(logPath);
    sli::CrashHandlerInstall(nullptr);

    sli::Log("engine starting, pid %u, argc=%d", GetCurrentProcessId(), argc);
    for (int i = 1; i < argc; ++i)
        sli::Log("argv[%d]=%ls", i, argv[i]);

    if (!ParseEngineArgs(argc, argv, g_ctx.args))
    {
        sli::Log("bad args");
        return 1;
    }
    sli::Log("args ok: %ux%u cf=%u map=%ls", g_ctx.args.w, g_ctx.args.h,
             g_ctx.args.cf, g_ctx.args.mapName.c_str());

    if (!PickAdapter(g_ctx) || !SetupDevice(g_ctx))
        return 2;
    sli::Log("device ready");

    if (!OpenSharedTransport(g_ctx))
        return 4;
    // READY goes through the mapping, never through a shared fence: the
    // ready signal must not be able to bleed into the produce fence the
    // game's frame sync depends on.
    g_ctx.hs->ready = sli::READY_MAGIC;
    sli::Log("READY written to the handshake");

    const int rc = RunFrameLoop(g_ctx);
    sli::Log("engine exit rc=%d", rc);
    // Hard-stop after the clean exit (R82): the vendor NGX runtime leaves a
    // worker thread alive past main() that AVs inside nvngx_dlss.dll during
    // CRT teardown (57 identical dumps, D3D12Core.dll+0x1D9DC). LogLine is
    // durable (open/write/close per line), so a silent TerminateProcess
    // here is strictly safer than the use-after-teardown race.
    TerminateProcess(GetCurrentProcess(), rc);
    return rc;
}
