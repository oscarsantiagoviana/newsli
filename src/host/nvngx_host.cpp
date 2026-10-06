// nvngx_host.cpp — the transparent NGX proxy (F3). The driver's nvngx.dll
// load is redirected here by the dxgi load vector, so the DRIVER's NGX —
// and through its D3D integration, the game's — lands on our exports.
//
// Shape (RDR2-proven, read line-by-line from the POC's nvngx_host v5):
//   - We are a pass-through for the REAL core: loaded once by us through
//     LdrLoadDll-direct (bypassing the load hook; no redirect recursion),
//     Init forwarded ONCE and its result cached. The driver retried init
//     1.2M times when the first cut answered raw failures — the cache is
//     what makes the proxy invisible.
//   - The ONE interception: EvaluateFeature for the SuperSampling feature
//     hands the frame to the offload session (seal -> engine on GPU 2 ->
//     delta -> compose). The native evaluate ALWAYS runs afterwards: the
//     game never loses a frame and native DLSS stays the permanent
//     fallback. After it returns, the SR-output colour SEAL (a copy into
//     the shared transport) is recorded into the SAME command list —
//     the exact GPU point where the game expects the upscaler's output
//     to be complete. The delta COMPOSITION happens later, at the
//     game's Present (host::PresentGate, driven by the dxgi proxy).
//   - The queue observer (vtable slot 10, ExecuteCommandLists) turns the
//     game's submit into the produce signal, GPU-ordered behind the
//     in-list seal copies.
//
// The init variants (Init, Init_Ext, Init_ProjectID, Init_with_ProjectID)
// normalize to the core's raw Init_Ext ABI: version BEFORE fcInfo
// (verified against the core's export behavior).
#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <nvsdk_ngx.h>
#include <mutex>
#include <string>

#include "host/offload_session.h"
#include "shared/log.h"
using Microsoft::WRL::ComPtr;
using namespace sli;  // Log/LogRate/LogInit + the host:: session API

// ---------------------------------------------------------------------------
// The real NGX core
// ---------------------------------------------------------------------------

// Loads a dll by ABSOLUTE path through LdrLoadDll directly — bypasses the
// load hook's kernel32/kernelbase detours (no redirect recursion; the
// same trick OptiScaler's NtdllProxy uses for its internal loads).
static HMODULE CoreLoadDirect(const wchar_t* absolutePath)
{
    using NtStatus = long;
    struct UnicodeString { USHORT Length, MaximumLength; PWSTR Buffer; };
    using FnRtlInit = void(NTAPI*)(UnicodeString*, PWSTR);
    using FnLdrLoad = NtStatus(NTAPI*)(PWSTR, PULONG, UnicodeString*, PHANDLE);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr)
        return nullptr;
    const auto rtlInit = (FnRtlInit) GetProcAddress(ntdll, "RtlInitUnicodeString");
    const auto ldrLoad = (FnLdrLoad) GetProcAddress(ntdll, "LdrLoadDll");
    if (rtlInit == nullptr || ldrLoad == nullptr)
        return nullptr;
    UnicodeString u {};
    rtlInit(&u, const_cast<PWSTR>(absolutePath));
    HANDLE h = nullptr;
    const NtStatus st = ldrLoad(nullptr, nullptr, &u, &h);
    sli::Log("nvngx: LdrLoadDll(core) -> 0x%08X", (unsigned) st);
    return st == 0 ? (HMODULE) h : nullptr;
}

// The driver publishes its DriverStore core path in the registry
// (HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\FullPath); the
// nv_dispi.inf_* glob is the fallback.
static std::wstring CorePath()
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0,
                      KEY_QUERY_VALUE, &k) == ERROR_SUCCESS)
    {
        wchar_t buf[MAX_PATH] = {};
        DWORD size = sizeof(buf);
        const LSTATUS r = RegQueryValueExW(k, L"FullPath", nullptr, nullptr,
                                           (LPBYTE) buf, &size);
        RegCloseKey(k);
        if (r == ERROR_SUCCESS && size > 0 && size <= sizeof(buf))
        {
            buf[MAX_PATH - 1] = 0;
            std::wstring p(buf);
            if (!p.empty() && p.back() != L'\\')
                p += L"\\";
            return p + L"nvngx.dll";
        }
    }
    WIN32_FIND_DATAW fd {};
    const HANDLE fh = FindFirstFileW(
        L"C:\\Windows\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_*",
        &fd);
    if (fh == INVALID_HANDLE_VALUE)
        return L"";
    std::wstring p = L"C:\\Windows\\System32\\DriverStore\\FileRepository\\";
    p += fd.cFileName;
    p += L"\\nvngx.dll";
    FindClose(fh);
    return p;
}

static HMODULE g_core = nullptr;
static bool g_coreTried = false;

static HMODULE Core()
{
    if (g_core != nullptr || g_coreTried)
        return g_core;
    g_coreTried = true;
    const std::wstring path = CorePath();
    if (path.empty())
    {
        Log("nvngx: no real core path found");
        return nullptr;
    }
    g_core = CoreLoadDirect(path.c_str());
    if (g_core == nullptr)
        Log("nvngx: real core load failed (%ls)", path.c_str());
    else
        Log("nvngx: real core loaded (%ls)", path.c_str());
    return g_core;
}

static FARPROC Real(const char* name)
{
    HMODULE c = Core();
    return c != nullptr ? GetProcAddress(c, name) : nullptr;
}

template <typename R, typename... A>
static R Forward(const char* name, A... args)
{
    const auto fn = (R(NVSDK_CONV*) (A...)) Real(name);
    if (fn == nullptr)
        return (R) (intptr_t) NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    return fn(args...);
}

// The core's raw Init ABI: version BEFORE fcInfo (OptiScaler's
// PFN_D3D12_Init_Ext typedef ordering — verified against the core).
using CoreInitExt =
    NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*,
                                  ID3D12Device*, NVSDK_NGX_Version,
                                  const NVSDK_NGX_FeatureCommonInfo*);

// ---------------------------------------------------------------------------
// The queue observer: ID3D12CommandQueue vtable slot 10 is
// ExecuteCommandLists (IUnknown 3 + ID3D12Object 4 + GetDevice 1 +
// UpdateTileMappings + CopyTileMappings = slots 8, 9).
// ---------------------------------------------------------------------------

namespace QTrack {

constexpr int ECL_SLOT = 10;
using EclFn = void (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT,
                                        ID3D12CommandList* const*);

static EclFn g_realEcl = nullptr;
static ID3D12CommandQueue* g_probeQueue = nullptr;  // keeps the vtable alive

static SRWLOCK g_lock = SRWLOCK_INIT;
static ComPtr<ID3D12CommandQueue> g_evalQueue;   // ran a known evaluate list
static ComPtr<ID3D12CommandQueue> g_lastDirect;  // fallback: last DIRECT seen
static ID3D12GraphicsCommandList* g_evalLists[8] = {};  // rolling evaluate lists
static unsigned g_evalListNext = 0;
static unsigned long long g_eclCount = 0;
static bool g_logged = false;

// The ECL hook runs inside the game's ExecuteCommandLists: same shield. A
// throw here would unwind through foreign frames and kill the game.
static void STDMETHODCALLTYPE HookedECLInner(ID3D12CommandQueue* q, UINT n,
                                             ID3D12CommandList* const* lists);

static void STDMETHODCALLTYPE HookedECL(ID3D12CommandQueue* q, UINT n,
                                        ID3D12CommandList* const* lists)
{
    try
    {
        HookedECLInner(q, n, lists);
    }
    catch (...)
    {
        // never let the observer take the game down; tracking is best-effort.
        // Inner itself forwards to g_realEcl — exactly once.
    }
}

static void STDMETHODCALLTYPE HookedECLInner(ID3D12CommandQueue* q, UINT n,
                                             ID3D12CommandList* const* lists)
{
    // R90 (#8): the observer used to take the exclusive SRWLock on EVERY
    // submit of EVERY queue, run GetDesc (a virtual) and the LogRate file
    // I/O INSIDE it. Three cuts, same behavior:
    //  1. only DIRECT queues are ever interesting (the evaluate lists are
    //     direct; compute/copy submits now pay nothing);
    //  2. the LogRate moved OUT of the lock (it did file I/O with the lock
    //     held every 5 s — a guaranteed stutter for the game thread);
    //  3. a lock-free snapshot of the evaluate-list window: the ring is
    //     written by the game thread (NoteEvaluateList) and only read
    //     here, so a torn read can only MISS a match this submit would
    //     not have matched anyway — the queue pointer itself is the
    //     authoritative anchor and stays under the lock when it matters.
    {
        unsigned long long eclNow = 0;
        {
            AcquireSRWLockExclusive(&g_lock);
            ++g_eclCount;
            const D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
            if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
            {
                g_lastDirect = q;
                for (UINT i = 0; i < n; ++i)
                    for (auto* e : g_evalLists)
                        if (e != nullptr && lists[i] == (ID3D12CommandList*) e)
                        {
                            g_evalQueue = q;
                            if (!g_logged)
                            {
                                g_logged = true;
                                Log("nvngx: evaluate list seen on DIRECT queue %p "
                                    "after %llu ECLs", (void*) q, g_eclCount);
                            }
                        }
            }
            eclNow = g_eclCount;
            ReleaseSRWLockExclusive(&g_lock);
        }
        // outside the lock: the 1/5 s rate line (file I/O)
        static unsigned long long s_lastLogged = 0;
        static ULONGLONG s_lastLogTick = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - s_lastLogTick > 5000)
        {
            LogRate(5000, "nvngx: ECL %llu calls (+%llu since last)",
                    eclNow, (unsigned long long) (eclNow - s_lastLogged));
            s_lastLogged = eclNow;
            s_lastLogTick = now;
        }
    }
    g_realEcl(q, n, lists);
    // AFTER the real submit: if one of these lists carries a pending seal,
    // GPU-order the produce signal behind it — the engine starts only when
    // the in-list seal copies have actually executed.
    for (UINT i = 0; i < n; ++i)
        host::OnListSubmitted(lists[i], q);
}

static bool g_installed = false;

void Install(ID3D12Device* dev)
{
    if (g_installed || dev == nullptr)
        return;
    g_installed = true;
    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* probe = nullptr;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&probe))))
    {
        Log("nvngx: probe queue creation failed - no queue tracking");
        return;
    }
    void** vt = *(void***) probe;  // the class vtable, shared by every queue
    DWORD old = 0;
    if (!VirtualProtect(&vt[ECL_SLOT], sizeof(void*), PAGE_READWRITE, &old))
    {
        Log("nvngx: vtable not writable - no queue tracking");
        probe->Release();
        return;
    }
    g_realEcl = (EclFn) vt[ECL_SLOT];
    vt[ECL_SLOT] = (void*) &HookedECL;
    VirtualProtect(&vt[ECL_SLOT], sizeof(void*), old, &old);
    g_probeQueue = probe;  // released never, on purpose: the vtable stays
    Log("nvngx: queue observer installed (ECL slot was %p)", (void*) g_realEcl);
}

void NoteEvaluateList(ID3D12GraphicsCommandList* l)
{
    if (l == nullptr)
        return;
    AcquireSRWLockExclusive(&g_lock);
    g_evalLists[g_evalListNext++ & 7] = l;
    ReleaseSRWLockExclusive(&g_lock);
}

// Returns the queue an evaluate list was last seen on (fallback: the last
// DIRECT queue). The returned ComPtr owns one reference (move, no AddRef:
// the internal reference is handed out, not copied).
ComPtr<ID3D12CommandQueue> SealQueue()
{
    AcquireSRWLockShared(&g_lock);
    ComPtr<ID3D12CommandQueue> q =
        g_evalQueue != nullptr ? g_evalQueue : g_lastDirect;
    ReleaseSRWLockShared(&g_lock);
    return q;
}

} // namespace QTrack

// ---------------------------------------------------------------------------
// Host state + evaluate normalization
// ---------------------------------------------------------------------------

static ID3D12Device* g_dev = nullptr;
static const NVSDK_NGX_Handle* g_ssHandle = nullptr;  // SuperSampling feature
static bool g_enabled = false;
static bool g_enablementDecided = false;
static unsigned long long g_evalCalls = 0;

// ---------------------------------------------------------------------------
// R82i: SR-side controls. The game decides its render size from the answer
// of the DLSSOptimalSettingsCallback it registers in the CREATE parameter
// block. We wrap that callback (pattern: OptiScaler NVNGX_Parameter.cpp,
// NVSDK_NGX_DLSS_GetOptimalSettingsCallback): the game's callback answers
// first, then we overwrite the sizes it published with our forced ratio.
// Latched at CREATE — the callback pointer lives in the feature's params.
// ---------------------------------------------------------------------------
// Typed-then-untyped three-step resource read (the OptiScaler pattern:
// games and the driver disagree on how the resource lands in the block).
static ID3D12Resource* GetParamResource(NVSDK_NGX_Parameter* params,
                                        const char* key, const char* altKey)
{
    ID3D12Resource* res = nullptr;
    if (key != nullptr &&
        params->Get(key, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;
    if (altKey != nullptr &&
        params->Get(altKey, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;
    void* untyped = nullptr;
    if (key != nullptr &&
        params->Get(key, &untyped) == NVSDK_NGX_Result_Success &&
        untyped != nullptr)
        return static_cast<ID3D12Resource*>(untyped);
    return nullptr;
}

static void OnInit(ID3D12Device* dev)
{
    if (dev != nullptr)
        g_dev = dev;
    if (!g_enablementDecided)
    {
        g_enablementDecided = true;
        // logging + crash handler live beside OUR dll (the game dir and the
        // exe dir can differ under some loaders; the log must not collide
        // with the dxgi proxy's or the engine's)
        wchar_t dllDir[MAX_PATH] = {};
        HMODULE self = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR) &OnInit, &self) && self != nullptr)
        {
            GetModuleFileNameW(self, dllDir, MAX_PATH);
            wchar_t* s2 = wcsrchr(dllDir, L'\\');
            if (s2) *s2 = 0;
        }
        char logPath[MAX_PATH] = "sli_host.log";
        {
            char dirA[MAX_PATH] = {};
            WideCharToMultiByte(CP_UTF8, 0, dllDir, -1, dirA, sizeof(dirA),
                                nullptr, nullptr);
            _snprintf_s(logPath, MAX_PATH, _TRUNCATE, "%s\\sli_host.log",
                        dirA);
        }
        LogInit(logPath);
        CrashHandlerInstall(nullptr);
        // Enablement: the engine binary must be deployed beside us, and no
        // explicit kill switch. Everything else (weights, runtime) failing
        // degrades to zero-delta, which is safe by construction.
        const bool enginePresent = GetFileAttributesW(
            (std::wstring(dllDir) + L"\\sli_engine.exe").c_str()) !=
            INVALID_FILE_ATTRIBUTES;
        const bool envKill = GetEnvironmentVariableA("SLI_DISABLE", nullptr, 0)
                             != 0;
        g_enabled = enginePresent && !envKill;
        Log("nvngx: enablement: engine=%d envkill=%d -> %s",
            enginePresent ? 1 : 0, envKill ? 1 : 0,
            g_enabled ? "OFFLOAD ON" : "pass-through only");
        if (g_enabled)
            host::Configure(1);
    }
    QTrack::Install(g_dev);
}

// Normalizes the game's evaluate block into EvalArgs and hands it to the
// session. Never blocks, never takes ownership: the frame ALWAYS continues
// into the native evaluate afterwards.
static host::EvalArgs g_lastArgs;   // R82h: read again after the native
                                    // evaluate by SealColorPost (same thread,
                                    // same list — the NGX evaluate is single-
                                    // threaded per the game's own submit).

static void OfferFrameToSession(ID3D12GraphicsCommandList* cmdList,
                                const NVSDK_NGX_Handle* handle,
                                NVSDK_NGX_Parameter* params)
{
    if (!g_enabled || g_dev == nullptr || cmdList == nullptr ||
        params == nullptr)
        return;
    if (g_ssHandle != nullptr && handle != g_ssHandle)
        return;  // only the SuperSampling feature is offloaded

    host::EvalArgs a {};
    a.gameDev = g_dev;
    a.srcColor = GetParamResource(params, NVSDK_NGX_Parameter_Color,
                                  "DLSSD.Color");
    a.srcDepth = GetParamResource(params, NVSDK_NGX_Parameter_Depth,
                                  "DLSSD.Depth");   // golpe 2: real depth
    a.srcMotion = GetParamResource(params, NVSDK_NGX_Parameter_MotionVectors,
                                   "DLSSD.MotionVectors");
    a.dstOutput = GetParamResource(params, NVSDK_NGX_Parameter_Output,
                                   "DLSSD.Output");
    if (a.srcColor == nullptr || a.dstOutput == nullptr)
        return;

    // Remember the list: the observer names the queue it runs on, and the
    // seal is recorded into the list itself (executes at the native read
    // point, GPU-ordered after the game's motion pass).
    QTrack::NoteEvaluateList(cmdList);
    a.inCmdList = cmdList;
    a.gameQueue = QTrack::SealQueue();

    ++g_evalCalls;
    // R91b: routine heartbeat 5 s -> 60 s (was ~57 lines/min in the game
    // threads; rare events stay unthrottled)
    LogRate(60000, "nvngx: evaluate #%llu (queue %p%s)", g_evalCalls,
            (void*) a.gameQueue.Get(),
            a.gameQueue == nullptr ? " NOT FOUND - native" : "");
    if (a.gameQueue == nullptr)
        return;  // nothing observed yet: native this frame

    const auto colorDesc = a.srcColor->GetDesc();
    const auto outDesc = a.dstOutput->GetDesc();
    a.inW = (unsigned) colorDesc.Width;
    a.inH = colorDesc.Height;
    a.outW = (unsigned) outDesc.Width;
    a.outH = outDesc.Height;

    // The game's per-frame render dims (dynamic resolution), then the
    // subrect params (resources sit at MAX size; the real content is a
    // smaller top-left rect — ignoring that misaligns the model inputs).
    unsigned int renderW = 0, renderH = 0;
    if (params->Get(NVSDK_NGX_Parameter_Width, &renderW) ==
            NVSDK_NGX_Result_Success &&
        params->Get(NVSDK_NGX_Parameter_Height, &renderH) ==
            NVSDK_NGX_Result_Success &&
        renderW && renderH)
    {
        a.inW = renderW;
        a.inH = renderH;
    }
    unsigned int subW = 0, subH = 0;
    if (params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
                    &subW) == NVSDK_NGX_Result_Success &&
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
                    &subH) == NVSDK_NGX_Result_Success &&
        subW && subH && subW <= a.inW && subH <= a.inH)
    {
        a.inW = subW;
        a.inH = subH;
    }

    // (R95 audit: Input_Color_Subrect_Base X/Y no longer read — dead since
    //  the seal copies the whole colour resource)
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
                &a.motionBaseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,
                &a.motionBaseY);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
                &a.depthBaseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
                &a.depthBaseY);

    float mvScaleX = 1.f, mvScaleY = 1.f;
    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX) ==
        NVSDK_NGX_Result_Success)
        a.mvScaleX = mvScaleX;
    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY) ==
        NVSDK_NGX_Result_Success)
        a.mvScaleY = mvScaleY;
    params->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &a.jitterX);
    params->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &a.jitterY);
    // R82g.7 diagnosis: is the game's reported jitter actually moving?
    // (throttled ~5 s — a constant 0 means RDR2 jitters internally and the
    // NGX input color is already accumulated/de-jittered).
    {
        static unsigned jLogSeq = 0;
        if ((jLogSeq++ % 300) == 0)
            sli::Log("host: jitter (%.4f, %.4f)", a.jitterX, a.jitterY);
    }
    params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &a.preExposure);
    if (a.preExposure <= 0.f)
        a.preExposure = 1.f;
    unsigned int gameReset = 0;
    if (params->Get(NVSDK_NGX_Parameter_Reset, &gameReset) ==
        NVSDK_NGX_Result_Success)
        a.gameReset = gameReset != 0;
    unsigned int createFlags = 0;
    if (params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
                    &createFlags) == NVSDK_NGX_Result_Success)
        a.createFlags = createFlags;

    g_lastArgs = a;
    host::Evaluate(a);
}

// ---------------------------------------------------------------------------
// The exports
// ---------------------------------------------------------------------------

// Forward ONCE to the real core; cache the result; answer every further
// init with the cached value. The driver must see the core's own result —
// but the retry storm the POC's first cut produced (1.2M calls) is gone.
static NVSDK_NGX_Result DoInit(unsigned long long app, const wchar_t* path,
                               ID3D12Device* dev, NVSDK_NGX_Version sdk,
                               const NVSDK_NGX_FeatureCommonInfo* fcInfo)
{
    static bool coreInited = false;
    static NVSDK_NGX_Result initResult =
        NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    if (coreInited)
        return initResult;  // "already inited" branch

    OnInit(dev);

    NVSDK_NGX_FeatureCommonInfo local {};
    if (fcInfo != nullptr)
        memcpy(&local, fcInfo, sizeof(local));
    const auto fn = (CoreInitExt) Real("NVSDK_NGX_D3D12_Init_Ext");
    if (fn == nullptr)
    {
        Log("nvngx: real core unavailable - init fails honestly");
        coreInited = true;  // stop retrying forever
        return initResult;
    }
    initResult = fn(app, path, dev, sdk, fcInfo != nullptr ? &local : nullptr);
    coreInited = true;
    Log("nvngx: core Init_Ext(app %llu sdk %u) -> 0x%X", app,
        (unsigned) sdk, (unsigned) initResult);
    return initResult;
}

extern "C" {

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Init_Ext(
    unsigned long long InApplicationId, const wchar_t* InApplicationDataPath,
    ID3D12Device* InDevice, NVSDK_NGX_Version InSDKVersion,
    const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo)
{
    return DoInit(InApplicationId, InApplicationDataPath, InDevice,
                  InSDKVersion, InFeatureInfo);
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Init(
    unsigned long long InApplicationId, const wchar_t* InApplicationDataPath,
    ID3D12Device* InDevice,
    const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo,
    NVSDK_NGX_Version InSDKVersion)
{
    return DoInit(InApplicationId, InApplicationDataPath, InDevice,
                  InSDKVersion, InFeatureInfo);
}

static NVSDK_NGX_Result InitProjectID(
    const char* InProjectId, NVSDK_NGX_EngineType InEngineType,
    const char* InEngineVersion, const wchar_t* InApplicationDataPath,
    ID3D12Device* InDevice, const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo,
    NVSDK_NGX_Version InSDKVersion)
{
    OnInit(InDevice);
    Log("nvngx: D3D12_Init_ProjectID (%hs) sdk %u",
        InProjectId != nullptr ? InProjectId : "?", (unsigned) InSDKVersion);
    const auto fn = (NVSDK_NGX_Result(NVSDK_CONV*)(
        const char*, NVSDK_NGX_EngineType, const char*, const wchar_t*,
        ID3D12Device*, NVSDK_NGX_Version,
        const NVSDK_NGX_FeatureCommonInfo*)) Real(
            "NVSDK_NGX_D3D12_Init_ProjectID");
    if (fn == nullptr)
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    const NVSDK_NGX_Result r = fn(InProjectId, InEngineType, InEngineVersion,
                                  InApplicationDataPath, InDevice,
                                  InSDKVersion, InFeatureInfo);
    Log("nvngx: core Init_ProjectID -> 0x%X", (unsigned) r);
    return r;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Init_ProjectID(
    const char* InProjectId, NVSDK_NGX_EngineType InEngineType,
    const char* InEngineVersion, const wchar_t* InApplicationDataPath,
    ID3D12Device* InDevice, const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo,
    NVSDK_NGX_Version InSDKVersion)
{
    return InitProjectID(InProjectId, InEngineType, InEngineVersion,
                         InApplicationDataPath, InDevice, InFeatureInfo,
                         InSDKVersion);
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Init_with_ProjectID(
    const char* InProjectId, NVSDK_NGX_EngineType InEngineType,
    const char* InEngineVersion, const wchar_t* InApplicationDataPath,
    ID3D12Device* InDevice, const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo,
    NVSDK_NGX_Version InSDKVersion)
{
    return InitProjectID(InProjectId, InEngineType, InEngineVersion,
                         InApplicationDataPath, InDevice, InFeatureInfo,
                         InSDKVersion);
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_CreateFeature(
    ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Feature InFeatureID,
    NVSDK_NGX_Parameter* InParameters, NVSDK_NGX_Handle** OutHandle)
{
    if (InFeatureID == NVSDK_NGX_Feature_SuperSampling &&
        InParameters != nullptr)
    {
        // R83 diag: what render size does the game DECLARE at create?
        // (if it declares explicit sizes here, it picks them from its own
        // menu table and the callback route may never fire)
        {
            unsigned cw = 0, ch = 0;
            if (InParameters->Get(NVSDK_NGX_Parameter_Width, &cw) ==
                    NVSDK_NGX_Result_Success &&
                InParameters->Get(NVSDK_NGX_Parameter_Height, &ch) ==
                    NVSDK_NGX_Result_Success)
                Log("nvngx: create block declares render %ux%u", cw, ch);
        }
    }
    const NVSDK_NGX_Result r =
        Forward<NVSDK_NGX_Result>("NVSDK_NGX_D3D12_CreateFeature", InCmdList,
                                  InFeatureID, InParameters, OutHandle);
    if (r == NVSDK_NGX_Result_Success && OutHandle != nullptr &&
        InFeatureID == NVSDK_NGX_Feature_SuperSampling)
    {
        g_ssHandle = *OutHandle;
        Log("nvngx: SuperSampling feature created (handle %p)",
            (void*) g_ssHandle);
    }
    return r;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature(
    ID3D12GraphicsCommandList* InCmdList,
    const NVSDK_NGX_Handle* InFeatureHandle,
    NVSDK_NGX_Parameter* InParameters,
    PFN_NVSDK_NGX_ProgressCallback InCallback)
{
    OfferFrameToSession(InCmdList, InFeatureHandle, InParameters);
    // The delta composition no longer happens here: R79 moved it to the
    // game's Present call (host::PresentGate, driven by the dxgi proxy).
    const NVSDK_NGX_Result r =
        Forward<NVSDK_NGX_Result>("NVSDK_NGX_D3D12_EvaluateFeature", InCmdList,
                                  InFeatureHandle, InParameters, InCallback);
    // R82h: this records the SR-output colour copy into the same list,
    // GPU-ordered after the native passes. ONLY the SuperSampling feature
    // may seal: any other evaluate (a future RDR2 NGX feature) must not
    // re-copy the STALE last-SS frame onto the transport (one-frame ghost
    // of old content — the class of bug the mirror sizing bug taught us).
    if (InFeatureHandle == g_ssHandle)
        host::SealColorPost(g_lastArgs);
    return r;
}

// The _C variant differs only in the callback's bool passing (bool* vs
// bool&) — identical at the ABI level; one implementation (RDR2 uses this).
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature_C(
    ID3D12GraphicsCommandList* InCmdList,
    const NVSDK_NGX_Handle* InFeatureHandle,
    const NVSDK_NGX_Parameter* InParameters,
    PFN_NVSDK_NGX_ProgressCallback_C InCallback)
{
    if (InParameters != nullptr)
        OfferFrameToSession(InCmdList, InFeatureHandle,
                            const_cast<NVSDK_NGX_Parameter*>(InParameters));
    const NVSDK_NGX_Result r =
        Forward<NVSDK_NGX_Result>("NVSDK_NGX_D3D12_EvaluateFeature", InCmdList,
                                  InFeatureHandle, InParameters,
                                  (PFN_NVSDK_NGX_ProgressCallback) InCallback);
    if (InFeatureHandle == g_ssHandle)  // SS-only seal (see the C variant above)
        host::SealColorPost(g_lastArgs);
    return r;
}

// ---- the Present gate trampolines (called by sli_dxgi's swapchain proxy;
// resolved with GetProcAddress, so plain C names + stdcall-on-x64) ----

extern "C" __declspec(dllexport)
long WINAPI sli_PresentGate(IDXGISwapChain* real, unsigned sync, unsigned flags)
{
    return host::PresentGate(real, sync, flags);
}

extern "C" __declspec(dllexport)
long WINAPI sli_PresentGate1(IDXGISwapChain* real, unsigned sync,
                             unsigned flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    return host::PresentGate1(real, sync, flags, pp);
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Shutdown(void)
{
    host::Shutdown();
    return Forward<NVSDK_NGX_Result>("NVSDK_NGX_D3D12_Shutdown");
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Shutdown1(ID3D12Device* InDevice)
{
    return Forward<NVSDK_NGX_Result>("NVSDK_NGX_D3D12_Shutdown1", InDevice);
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_ReleaseFeature(
    NVSDK_NGX_Handle* InHandle)
{
    if (g_ssHandle == InHandle)
        g_ssHandle = nullptr;
    return Forward<NVSDK_NGX_Result>("NVSDK_NGX_D3D12_ReleaseFeature",
                                     InHandle);
}

// ---- plain forwarders ----

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_GetCapabilityParameters(
    NVSDK_NGX_Parameter** OutParameters)
{
    return Forward<NVSDK_NGX_Result>(
        "NVSDK_NGX_D3D12_GetCapabilityParameters", OutParameters);
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_GetParameters(
    NVSDK_NGX_Parameter** OutParameters)
{
    return Forward<NVSDK_NGX_Result>("NVSDK_NGX_D3D12_GetParameters",
                                     OutParameters);
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_AllocateParameters(
    NVSDK_NGX_Parameter** OutParameters)
{
    return Forward<NVSDK_NGX_Result>("NVSDK_NGX_D3D12_AllocateParameters",
                                     OutParameters);
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_DestroyParameters(
    NVSDK_NGX_Parameter* InParameters)
{
    return Forward<NVSDK_NGX_Result>("NVSDK_NGX_D3D12_DestroyParameters",
                                     InParameters);
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_GetScratchBufferSize(
    NVSDK_NGX_Feature InFeatureId, const NVSDK_NGX_Parameter* InParameters,
    size_t* OutSizeInBytes)
{
    return Forward<NVSDK_NGX_Result>("NVSDK_NGX_D3D12_GetScratchBufferSize",
                                     InFeatureId, InParameters,
                                     OutSizeInBytes);
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_GetFeatureRequirements(
    IDXGIAdapter* Adapter,
    const NVSDK_NGX_FeatureDiscoveryInfo* FeatureDiscoveryInfo,
    NVSDK_NGX_FeatureRequirement* OutSupported)
{
    return Forward<NVSDK_NGX_Result>(
        "NVSDK_NGX_D3D12_GetFeatureRequirements", Adapter,
        FeatureDiscoveryInfo, OutSupported);
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_UpdateFeature(
    const NVSDK_NGX_Application_Identifier* ApplicationId,
    NVSDK_NGX_Feature FeatureID)
{
    return Forward<NVSDK_NGX_Result>("NVSDK_NGX_UpdateFeature", ApplicationId,
                                     FeatureID);
}

// ---- stub: CUDA / VULKAN / D3D11 / OTA ----
// This stack is D3D12-only; those families just need the export NAMES to
// resolve. The stub logs loudly the first time one is actually called.

NVSDK_NGX_Result NVSDK_CONV Host_Unsupported_Family(void)
{
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("nvngx: stubbed CUDA/VULKAN/D3D11/OTA export called "
            "(returning failure)");
    }
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

} // extern "C"
