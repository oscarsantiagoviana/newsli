// dxgi_proxy — the load vector (ported from the POC, RDR2-proven):
// OptiScaler (26 MB) and ReShade both lived in the dxgi.dll slot on RDR2
// all week. Forwards every dxgi export to the real System32 dxgi and
// lazily loads our nvngx.dll (the NGX host) on the game's thread at the
// first dxgi call (EnsureHost — the old helper-thread preload was
// replaced by this lazy load).
//
// Why the early nvngx load: the NVIDIA driver preloads its DriverStore ngx
// core into the game process by absolute path at device creation, so a
// plain app-dir nvngx.dll never wins the name (observed live). The game
// loads dxgi BEFORE creating the D3D12 device — our thread claims the
// "nvngx.dll" name first, and the game's later resolution finds us.
//
// (The version.dll static-import slot does the same thing but RDR2's
// protection fast-fails on it: 0xC0000409 in ucrtbase, twice, gone the
// moment the file was removed. dxgi is dynamic and proven.)
//
// R79 — the swapchain Present hook: the factory creators hand out a thin
// IDXGISwapChain proxy (everything forwarded to the real object; only
// Present/Present1 route through the Present gate in the NGX host). The
// proxy pair is the structural hook point for future FG/SR composition
// backends (R79b).
#include <windows.h>
#include <dxgi1_6.h>
#include <new>
#include "shared/log.h"

// ---------------------------------------------------------------------------
// The Present gate lives in the NGX host (nvngx.dll). Resolved once at the
// first dxgi call and again lazily per Present: a missing host (plain
// game, load failed) means a pure pass-through, never a broken dxgi.
// ---------------------------------------------------------------------------
namespace sli_gate
{
using PFN_PresentGate = long(WINAPI*)(void*, unsigned, unsigned);
using PFN_PresentGate1 = long(WINAPI*)(void*, unsigned, unsigned, const void*);
static PFN_PresentGate g_gate = nullptr;
static PFN_PresentGate1 g_gate1 = nullptr;
static volatile long g_resolved = 0;

inline void Resolve()
{
    if (InterlockedCompareExchange(&g_resolved, 1, 0) != 0)
        return;
    const HMODULE host = GetModuleHandleW(L"nvngx.dll");
    if (host == nullptr)
    {
        sli::Log("dxgi_proxy: present gate: no nvngx host - pure forward");
        return;
    }
    g_gate = (PFN_PresentGate)(void(*)()) GetProcAddress(host, "sli_PresentGate");
    g_gate1 = (PFN_PresentGate1)(void(*)()) GetProcAddress(host, "sli_PresentGate1");
    sli::Log("dxgi_proxy: present gate %s",
             g_gate != nullptr ? "linked" : "exports NOT FOUND");
}
} // namespace sli_gate

static HMODULE g_real = nullptr;

static HMODULE Real()
{
    if (g_real != nullptr)
        return g_real;
    g_real = LoadLibraryW(L"C:\\Windows\\System32\\dxgi.dll");
    return g_real;
}

// Lazy, on the game's own thread: the first dxgi export call is the game
// (or an overlay) initializing graphics — comfortably before any D3D12
// device exists. Loading the host right there claims the "nvngx.dll" name
// without any helper thread or timer (the first cut used a CreateThread
// from DllMain + 50 ms; the game died with an access violation whose
// faulting frame was exactly that thread — gone).
// Claim the "nvngx.dll" name and install the OptiScaler-pattern load hook:
// the driver's nvngx load (by DriverStore absolute path) is redirected to
// OUR module, whose NGX exports transparently proxy the real core with the
// offload hooked at evaluate. "_nvngx.dll" is deliberately NOT claimed —
// the core itself loads it internally and must receive the real one.
void InstallNgxLoadHook(HMODULE self, HMODULE host);

static void EnsureHost()
{
    static LONG s_done = 0;
    if (InterlockedCompareExchange(&s_done, 1, 0) == 0)
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR) &EnsureHost, &self);
        // one extra ref: the caller's eventual FreeLibrary must not unload us
        const HMODULE host = LoadLibraryW(L"nvngx.dll");
        if (host != nullptr)
            LoadLibraryW(L"nvngx.dll");
        sli::Log("dxgi_proxy: nvngx.dll %s", host != nullptr
                                                 ? "preloaded at first dxgi call"
                                                 : "LOAD FAILED");
        if (host != nullptr)
            InstallNgxLoadHook(self, host);
        // the gate can bind right now; Present re-resolves if it could not
        sli_gate::Resolve();
    }
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE; // nothing: no threads, no loads, no loader-lock surface
}

// ---------------------------------------------------------------------------
// The swapchain proxy. Ownership model (COM-clean, leak-free across
// swapchain recreation):
//   - base: IDXGISwapChain1* holding the ONE reference the factory handed
//     out. The game exercises it through the proxy's forwarded
//     AddRef/Release; a Release that returns 0 destroys the underlying
//     object AND the proxy together (nobody can reach either anymore).
//   - wide: IDXGISwapChain4* cached RAW (QI + immediate Release): valid
//     exactly as long as base's object lives; used for every non-Present
//     forward so late-interface calls (ResizeBuffers1, SetHDRMetaData,
//     GetCurrentBackBufferIndex) hit the real object directly.
//   - QueryInterface serves the proxy for every interface level the proxy
//     type covers (COM identity preserved); exotic IIDs forward.
// ---------------------------------------------------------------------------
namespace sc_proxy
{

struct SwapChainProxy;
void UnregisterOne(SwapChainProxy* p);  // defined after the struct

struct SwapChainProxy final : public IDXGISwapChain4
{
    IDXGISwapChain1* base = nullptr;  // owns the factory's out reference
    IDXGISwapChain4* wide = nullptr;  // raw, non-owning (see above)

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr)
            return E_POINTER;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) ||
            IsEqualGUID(riid, __uuidof(IDXGIObject)) ||
            IsEqualGUID(riid, __uuidof(IDXGIDeviceSubObject)) ||
            IsEqualGUID(riid, __uuidof(IDXGISwapChain)) ||
            IsEqualGUID(riid, __uuidof(IDXGISwapChain1)) ||
            IsEqualGUID(riid, __uuidof(IDXGISwapChain2)) ||
            IsEqualGUID(riid, __uuidof(IDXGISwapChain3)) ||
            IsEqualGUID(riid, __uuidof(IDXGISwapChain4)))
        {
            *ppv = static_cast<IDXGISwapChain4*>(this);
            AddRef();
            return S_OK;
        }
        return base->QueryInterface(riid, ppv);  // exotic: real object
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return base->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        UnregisterOne(this);
        const ULONG r = base->Release();
        if (r == 0)
            delete this;  // the object died; the proxy dies with it
        return r;
    }

    // ---- IDXGIObject / IDXGIDeviceSubObject ----
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID n, UINT sz, const void* d) override
        { return wide->SetPrivateData(n, sz, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID n, const IUnknown* p) override
        { return wide->SetPrivateDataInterface(n, p); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID n, UINT* sz, void* d) override
        { return wide->GetPrivateData(n, sz, d); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppv) override
        { return wide->GetParent(riid, ppv); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppv) override
        { return wide->GetDevice(riid, ppv); }

    // ---- IDXGISwapChain: Present goes through the gate -----------------
    HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) override
        { return wide->GetBuffer(Buffer, riid, ppSurface); }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) override
        { return wide->SetFullscreenState(Fullscreen, pTarget); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) override
        { return wide->GetFullscreenState(pFullscreen, ppTarget); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) override
        { return wide->GetDesc(pDesc); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height,
                                            DXGI_FORMAT NewFormat, UINT SwapChainFlags) override
        { return wide->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags); }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) override
        { return wide->ResizeTarget(pNewTargetParameters); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput) override
        { return wide->GetContainingOutput(ppOutput); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override
        { return wide->GetFrameStatistics(pStats); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) override
        { return wide->GetLastPresentCount(pLastPresentCount); }

    // ---- IDXGISwapChain1: Present1 goes through the gate ---------------
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) override
        { return wide->GetDesc1(pDesc); }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) override
        { return wide->GetFullscreenDesc(pDesc); }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* pHwnd) override
        { return wide->GetHwnd(pHwnd); }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void** ppUnk) override
        { return wide->GetCoreWindow(refiid, ppUnk); }
    HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags,
                                       const DXGI_PRESENT_PARAMETERS* pPresentParameters) override;
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override
        { return wide->IsTemporaryMonoSupported(); }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) override
        { return wide->GetRestrictToOutput(ppRestrictToOutput); }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* pColor) override
        { return wide->SetBackgroundColor(pColor); }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* pColor) override
        { return wide->GetBackgroundColor(pColor); }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override
        { return wide->SetRotation(Rotation); }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* pRotation) override
        { return wide->GetRotation(pRotation); }

    // ---- IDXGISwapChain2 ----
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override
        { return wide->SetSourceSize(Width, Height); }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* pWidth, UINT* pHeight) override
        { return wide->GetSourceSize(pWidth, pHeight); }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override
        { return wide->SetMaximumFrameLatency(MaxLatency); }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override
        { return wide->GetMaximumFrameLatency(pMaxLatency); }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override
        { return wide->GetFrameLatencyWaitableObject(); }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) override
        { return wide->SetMatrixTransform(pMatrix); }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) override
        { return wide->GetMatrixTransform(pMatrix); }

    // ---- IDXGISwapChain3 ----
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override
        { return wide->GetCurrentBackBufferIndex(); }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                     UINT* pColorSpaceSupport) override
        { return wide->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport); }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override
        { return wide->SetColorSpace1(ColorSpace); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height,
                                             DXGI_FORMAT NewFormat, UINT SwapChainFlags,
                                             const UINT* pCreateNodeQueue,
                                             IUnknown* const* ppPresentQueue) override
        { return wide->ResizeBuffers1(BufferCount, Width, Height, NewFormat,
                                      SwapChainFlags, pCreateNodeQueue, ppPresentQueue); }

    // ---- IDXGISwapChain4 ----
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size,
                                              void* pMetaData) override
        { return wide->SetHDRMetaData(Type, Size, pMetaData); }
};

// weak registry: Present itself is stateless here (the gate owns all
// state), so this only keeps the live proxies countable; Release clears
// the entry — swapchain recreation is leak-safe by construction.
constexpr int kMaxProxies = 8;
SwapChainProxy* g_proxies[kMaxProxies] = {};
SRWLOCK g_lock = SRWLOCK_INIT;
unsigned g_live = 0;

void RegisterOne(SwapChainProxy* p);
void UnregisterOne(SwapChainProxy* p);

void RegisterOne(SwapChainProxy* p)
{
    AcquireSRWLockExclusive(&g_lock);
    int slot = -1;
    for (int i = 0; i < kMaxProxies; ++i)
        if (g_proxies[i] == nullptr) { slot = i; break; }
    if (slot < 0)
        slot = 0;  // 8 live swapchains is already exotic; oldest drops out
    g_proxies[slot] = p;
    ++g_live;
    ReleaseSRWLockExclusive(&g_lock);
}

void UnregisterOne(SwapChainProxy* p)
{
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < kMaxProxies; ++i)
        if (g_proxies[i] == p)
        {
            g_proxies[i] = nullptr;
            --g_live;
        }
    ReleaseSRWLockExclusive(&g_lock);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::Present(UINT SyncInterval, UINT Flags)
{
    sli_gate::Resolve();
    if (sli_gate::g_gate != nullptr)
        return sli_gate::g_gate((void*) base, SyncInterval, Flags);
    return base->Present(SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::Present1(UINT SyncInterval, UINT PresentFlags,
                                                   const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    sli_gate::Resolve();
    if (sli_gate::g_gate1 != nullptr)
        return sli_gate::g_gate1((void*) base, SyncInterval, PresentFlags,
                                 (const void*) pPresentParameters);
    return base->Present1(SyncInterval, PresentFlags, pPresentParameters);
}

// Wrap one freshly created swapchain. The proxy TAKES OVER the factory's
// out reference (base) and hands itself out as the caller's interface.
// Any failure path forwards the untouched real pointer — a missing hook
// must never break swapchain creation.
HRESULT Wrap(IDXGISwapChain1* created, REFIID riidOut, void** ppOut)
{
    if (created == nullptr || ppOut == nullptr)
        return E_POINTER;
    IDXGISwapChain4* wide = nullptr;
    if (FAILED(created->QueryInterface(__uuidof(IDXGISwapChain4),
                                       (void**) &wide)) ||
        wide == nullptr)
    {
        // no SC4 on this object (ancient dxgi — pre-1703; a D3D12+NGX game
        // never lands here): pass the creation reference through unwrapped
        return created->QueryInterface(riidOut, ppOut) == S_OK
            ? (created->Release(), S_OK)
            : E_NOINTERFACE;
    }
    wide->Release();  // NON-owning alias (see ownership model above): base
                      // stays the ONLY owner so Release-to-0 on it still
                      // destroys the real object
    auto* p = new (std::nothrow) SwapChainProxy();
    if (p == nullptr)
    {
        // OOM: hand the real object out unwrapped (the creation ref moves)
        const HRESULT hr = created->QueryInterface(riidOut, ppOut);
        created->Release();
        return hr;
    }
    p->base = created;  // owns the creation reference from here on
    p->wide = wide;     // raw alias, valid exactly while base's object lives
    RegisterOne(p);
    if (g_live == 1)
        sli::Log("dxgi_proxy: swapchain wrapped (%p)", (void*) created);
    *ppOut = p;  // the proxy answers every swapchain IID (see QI)
    return S_OK;
}

} // namespace sc_proxy

// ---------------------------------------------------------------------------
// The factory proxy: forwards every IDXGIFactory..7 method; the swapchain
// creators hand out sc_proxy::SwapChainProxy instead. Same ownership model
// (creation reference in `real`; Release-returning-0 deletes the wrapper).
// ---------------------------------------------------------------------------
namespace fac_proxy
{

struct FactoryProxy : public IDXGIFactory7
{
    IDXGIFactory7* real = nullptr;  // owns the factory's creation reference

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr)
            return E_POINTER;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) ||
            IsEqualGUID(riid, __uuidof(IDXGIObject)) ||
            IsEqualGUID(riid, __uuidof(IDXGIFactory)) ||
            IsEqualGUID(riid, __uuidof(IDXGIFactory1)) ||
            IsEqualGUID(riid, __uuidof(IDXGIFactory2)) ||
            IsEqualGUID(riid, __uuidof(IDXGIFactory3)) ||
            IsEqualGUID(riid, __uuidof(IDXGIFactory4)) ||
            IsEqualGUID(riid, __uuidof(IDXGIFactory5)) ||
            IsEqualGUID(riid, __uuidof(IDXGIFactory6)) ||
            IsEqualGUID(riid, __uuidof(IDXGIFactory7)))
        {
            *ppv = static_cast<IDXGIFactory7*>(this);
            AddRef();
            return S_OK;
        }
        return real->QueryInterface(riid, ppv);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return real->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = real->Release();
        if (r == 0)
            delete this;
        return r;
    }

    // ---- IDXGIObject ----
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID n, UINT sz, const void* d) override
        { return real->SetPrivateData(n, sz, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID n, const IUnknown* p) override
        { return real->SetPrivateDataInterface(n, p); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID n, UINT* sz, void* d) override
        { return real->GetPrivateData(n, sz, d); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppv) override
        { return real->GetParent(riid, ppv); }

    // ---- IDXGIFactory ----
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter, IDXGIAdapter** ppAdapter) override
        { return real->EnumAdapters(Adapter, ppAdapter); }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle, UINT Flags) override
        { return real->MakeWindowAssociation(WindowHandle, Flags); }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* pWindowHandle) override
        { return real->GetWindowAssociation(pWindowHandle); }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                              IDXGISwapChain** ppSwapChain) override;
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter** ppAdapter) override
        { return real->CreateSoftwareAdapter(Module, ppAdapter); }

    // ---- IDXGIFactory1 ----
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter, IDXGIAdapter1** ppAdapter) override
        { return real->EnumAdapters1(Adapter, ppAdapter); }
    BOOL STDMETHODCALLTYPE IsCurrent() override
        { return real->IsCurrent(); }

    // ---- IDXGIFactory2 ----
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override
        { return real->IsWindowedStereoEnabled(); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* pDevice, HWND hWnd,
                                                     const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                                     IDXGIOutput* pRestrictToOutput,
                                                     IDXGISwapChain1** ppSwapChain) override;
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* pDevice, IUnknown* pWindow,
                                                           const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                           IDXGIOutput* pRestrictToOutput,
                                                           IDXGISwapChain1** ppSwapChain) override;
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource, LUID* pLuid) override
        { return real->GetSharedResourceAdapterLuid(hResource, pLuid); }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle, UINT wMsg,
                                                         DWORD* pdwCookie) override
        { return real->RegisterStereoStatusWindow(WindowHandle, wMsg, pdwCookie); }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override
        { return real->RegisterStereoStatusEvent(hEvent, pdwCookie); }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) override
        { real->UnregisterStereoStatus(dwCookie); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND WindowHandle, UINT wMsg,
                                                            DWORD* pdwCookie) override
        { return real->RegisterOcclusionStatusWindow(WindowHandle, wMsg, pdwCookie); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override
        { return real->RegisterOcclusionStatusEvent(hEvent, pdwCookie); }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) override
        { real->UnregisterOcclusionStatus(dwCookie); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* pDevice,
                                                            const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                            IDXGIOutput* pRestrictToOutput,
                                                            IDXGISwapChain1** ppSwapChain) override;

    // ---- IDXGIFactory3..7 ----
    UINT STDMETHODCALLTYPE GetCreationFlags() override
        { return real->GetCreationFlags(); }
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID AdapterLuid, REFIID riid, void** ppvAdapter) override
        { return real->EnumAdapterByLuid(AdapterLuid, riid, ppvAdapter); }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void** ppvAdapter) override
        { return real->EnumWarpAdapter(riid, ppvAdapter); }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE Feature, void* pFeatureSupportData,
                                                  UINT FeatureSupportDataSize) override
        { return real->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize); }
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT Adapter, DXGI_GPU_PREFERENCE GpuPreference,
                                                         REFIID riid, void** ppvAdapter) override
        { return real->EnumAdapterByGpuPreference(Adapter, GpuPreference, riid, ppvAdapter); }
    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE hEvent, DWORD* pdwCookie) override
        { return real->RegisterAdaptersChangedEvent(hEvent, pdwCookie); }
    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD dwCookie) override
        { return real->UnregisterAdaptersChangedEvent(dwCookie); }
};

HRESULT STDMETHODCALLTYPE FactoryProxy::CreateSwapChain(IUnknown* pDevice,
                                                        DXGI_SWAP_CHAIN_DESC* pDesc,
                                                        IDXGISwapChain** ppSwapChain)
{
    if (ppSwapChain == nullptr)
        return E_INVALIDARG;
    IDXGISwapChain* raw = nullptr;
    HRESULT hr = real->CreateSwapChain(pDevice, pDesc, &raw);
    if (FAILED(hr) || raw == nullptr)
    {
        *ppSwapChain = nullptr;
        return hr;
    }
    // normalize to SC1 for the wrap (every SC is at least a swapchain1 in
    // practice; the QI keeps the same refcount semantics)
    IDXGISwapChain1* sc1 = nullptr;
    if (FAILED(raw->QueryInterface(__uuidof(IDXGISwapChain1), (void**) &sc1)) ||
        sc1 == nullptr)
    {
        *ppSwapChain = raw;  // exotic object: pass through untouched
        return hr;
    }
    raw->Release();  // the creation ref now lives in sc1
    void* out = nullptr;
    const HRESULT whr = sc_proxy::Wrap(sc1, __uuidof(IDXGISwapChain), &out);
    if (FAILED(whr) || out == nullptr)
    {
        *ppSwapChain = sc1;  // wrap declined (sc1 keeps the creation ref)
        return hr;
    }
    *ppSwapChain = (IDXGISwapChain*) out;
    return hr;
}

// shared finisher for the swapchain1 creators: wrap the raw result or
// pass the untouched creation reference through.
HRESULT FinishCreate1(HRESULT hr, IDXGISwapChain1* raw,
                      IDXGISwapChain1** ppSwapChain)
{
    if (FAILED(hr) || raw == nullptr)
    {
        *ppSwapChain = nullptr;
        return hr;
    }
    void* out = nullptr;
    const HRESULT whr = sc_proxy::Wrap(raw, __uuidof(IDXGISwapChain1), &out);
    if (FAILED(whr) || out == nullptr)
    {
        *ppSwapChain = raw;  // untouched creation ref passes through
        return hr;
    }
    *ppSwapChain = (IDXGISwapChain1*) out;
    return hr;
}

HRESULT STDMETHODCALLTYPE FactoryProxy::CreateSwapChainForHwnd(
    IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    if (ppSwapChain == nullptr)
        return E_INVALIDARG;
    IDXGISwapChain1* raw = nullptr;
    HRESULT hr = real->CreateSwapChainForHwnd(pDevice, hWnd, pDesc,
                                              pFullscreenDesc,
                                              pRestrictToOutput, &raw);
    return FinishCreate1(hr, raw, ppSwapChain);
}

HRESULT STDMETHODCALLTYPE FactoryProxy::CreateSwapChainForCoreWindow(
    IUnknown* pDevice, IUnknown* pWindow, const DXGI_SWAP_CHAIN_DESC1* pDesc,
    IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    if (ppSwapChain == nullptr)
        return E_INVALIDARG;
    IDXGISwapChain1* raw = nullptr;
    HRESULT hr = real->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc,
                                                    pRestrictToOutput, &raw);
    return FinishCreate1(hr, raw, ppSwapChain);
}

HRESULT STDMETHODCALLTYPE FactoryProxy::CreateSwapChainForComposition(
    IUnknown* pDevice, const DXGI_SWAP_CHAIN_DESC1* pDesc,
    IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    if (ppSwapChain == nullptr)
        return E_INVALIDARG;
    IDXGISwapChain1* raw = nullptr;
    HRESULT hr = real->CreateSwapChainForComposition(pDevice, pDesc,
                                                     pRestrictToOutput, &raw);
    return FinishCreate1(hr, raw, ppSwapChain);
}

// Wrap a factory result (the fwd_CreateDXGIFactory* exports call this).
// The proxy takes over the creation reference; the game only ever holds
// the proxy. Unwrappable results pass through untouched.
HRESULT WrapFactory(HRESULT hr, void** ppFactory)
{
    if (FAILED(hr) || ppFactory == nullptr || *ppFactory == nullptr)
        return hr;
    IDXGIFactory7* f7 = nullptr;
    if (FAILED(((IUnknown*) *ppFactory)->QueryInterface(__uuidof(IDXGIFactory7),
                                                        (void**) &f7)) ||
        f7 == nullptr)
        return hr;  // very old dxgi: forward unwrapped
    auto* p = new (std::nothrow) FactoryProxy();
    if (p == nullptr)
    {
        f7->Release();
        return hr;  // OOM: unwrapped
    }
    p->real = f7;      // the proxy now owns the QI'd reference
    ((IUnknown*) *ppFactory)->Release();  // drop the creation reference
    *ppFactory = p;
    sli::Log("dxgi_proxy: factory wrapped (%p)", (void*) p->real);
    return hr;
}

} // namespace fac_proxy

// ---- the exports a D3D12 game actually uses: exact signatures ----

extern "C" {

__declspec(dllexport) long WINAPI fwd_CreateDXGIFactory(const void* riid, void** factory)
{
    EnsureHost();
    auto f = (long(WINAPI*)(const void*, void**)) GetProcAddress(Real(), "CreateDXGIFactory");
    const long hr = f ? f(riid, factory) : 0x80004001L /*E_NOTIMPL*/;
    return fac_proxy::WrapFactory(hr, factory);
}

__declspec(dllexport) long WINAPI fwd_CreateDXGIFactory1(const void* riid, void** factory)
{
    EnsureHost();
    auto f = (long(WINAPI*)(const void*, void**)) GetProcAddress(Real(), "CreateDXGIFactory1");
    const long hr = f ? f(riid, factory) : 0x80004001L;
    return fac_proxy::WrapFactory(hr, factory);
}

__declspec(dllexport) long WINAPI fwd_CreateDXGIFactory2(unsigned int flags, const void* riid, void** factory)
{
    EnsureHost();
    auto f = (long(WINAPI*)(unsigned int, const void*, void**)) GetProcAddress(Real(), "CreateDXGIFactory2");
    const long hr = f ? f(flags, riid, factory) : 0x80004001L;
    return fac_proxy::WrapFactory(hr, factory);
}

__declspec(dllexport) long WINAPI fwd_DXGIDeclareAdapterRemovalSupport(void)
{
    EnsureHost();
    auto f = (long(WINAPI*)(void)) GetProcAddress(Real(), "DXGIDeclareAdapterRemovalSupport");
    return f ? f() : 0x80004001L;
}

__declspec(dllexport) long WINAPI fwd_DXGIGetDebugInterface1(unsigned int flags, const void* riid, void** debug)
{
    EnsureHost();
    auto f = (long(WINAPI*)(unsigned int, const void*, void**)) GetProcAddress(Real(), "DXGIGetDebugInterface1");
    return f ? f(flags, riid, debug) : 0x80004001L;
}

// ---- the rest (D3D10 layering, PIX capture, internal compat shims):
// faithful pass-through. Six-slot forwarding is ABI-safe for any arity up
// to six on x64 — the callee ignores slots beyond its own signature.

__declspec(dllexport) long WINAPI fwd_ApplyCompatResolutionQuirking(const void* a, const void* b, const void* c, const void* d, const void* e, const void* f2)
{
    auto fn = (long(WINAPI*)(const void*, const void*, const void*, const void*, const void*, const void*)) GetProcAddress(Real(), "ApplyCompatResolutionQuirking");
    return fn ? fn(a, b, c, d, e, f2) : 0x80004001L;
}

__declspec(dllexport) long WINAPI fwd_CompatString(const void* a, const void* b, const void* c, const void* d, const void* e, const void* f2)
{
    auto fn = (long(WINAPI*)(const void*, const void*, const void*, const void*, const void*, const void*)) GetProcAddress(Real(), "CompatString");
    return fn ? fn(a, b, c, d, e, f2) : 0x80004001L;
}

__declspec(dllexport) long WINAPI fwd_CompatValue(const void* a, const void* b, const void* c, const void* d, const void* e, const void* f2)
{
    auto fn = (long(WINAPI*)(const void*, const void*, const void*, const void*, const void*, const void*)) GetProcAddress(Real(), "CompatValue");
    return fn ? fn(a, b, c, d, e, f2) : 0x80004001L;
}

__declspec(dllexport) long WINAPI fwd_DXGID3D10CreateDevice(const void* a, const void* b, const void* c, const void* d, const void* e, const void* f2)
{
    auto fn = (long(WINAPI*)(const void*, const void*, const void*, const void*, const void*, const void*)) GetProcAddress(Real(), "DXGID3D10CreateDevice");
    return fn ? fn(a, b, c, d, e, f2) : 0x80004001L;
}

__declspec(dllexport) long WINAPI fwd_DXGID3D10CreateLayeredDevice(const void* a, const void* b, const void* c, const void* d, const void* e, const void* f2)
{
    auto fn = (long(WINAPI*)(const void*, const void*, const void*, const void*, const void*, const void*)) GetProcAddress(Real(), "DXGID3D10CreateLayeredDevice");
    return fn ? fn(a, b, c, d, e, f2) : 0x80004001L;
}

__declspec(dllexport) long WINAPI fwd_DXGID3D10GetLayeredDeviceSize(const void* a, const void* b, const void* c, const void* d, const void* e, const void* f2)
{
    auto fn = (long(WINAPI*)(const void*, const void*, const void*, const void*, const void*, const void*)) GetProcAddress(Real(), "DXGID3D10GetLayeredDeviceSize");
    return fn ? fn(a, b, c, d, e, f2) : 0x80004001L;
}

__declspec(dllexport) long WINAPI fwd_DXGID3D10RegisterLayers(const void* a, const void* b, const void* c, const void* d, const void* e, const void* f2)
{
    auto fn = (long(WINAPI*)(const void*, const void*, const void*, const void*, const void*, const void*)) GetProcAddress(Real(), "DXGID3D10RegisterLayers");
    return fn ? fn(a, b, c, d, e, f2) : 0x80004001L;
}

__declspec(dllexport) long WINAPI fwd_DXGIDumpJournal(const void* a, const void* b, const void* c, const void* d, const void* e, const void* f2)
{
    auto fn = (long(WINAPI*)(const void*, const void*, const void*, const void*, const void*, const void*)) GetProcAddress(Real(), "DXGIDumpJournal");
    return fn ? fn(a, b, c, d, e, f2) : 0x80004001L;
}

__declspec(dllexport) long WINAPI fwd_DXGIReportAdapterConfiguration(const void* a, const void* b, const void* c, const void* d, const void* e, const void* f2)
{
    auto fn = (long(WINAPI*)(const void*, const void*, const void*, const void*, const void*, const void*)) GetProcAddress(Real(), "DXGIReportAdapterConfiguration");
    return fn ? fn(a, b, c, d, e, f2) : 0x80004001L;
}

__declspec(dllexport) long WINAPI fwd_PIXBeginCapture(const void* a, const void* b, const void* c, const void* d, const void* e, const void* f2)
{
    auto fn = (long(WINAPI*)(const void*, const void*, const void*, const void*, const void*, const void*)) GetProcAddress(Real(), "PIXBeginCapture");
    return fn ? fn(a, b, c, d, e, f2) : 0x80004001L;
}

__declspec(dllexport) long WINAPI fwd_PIXEndCapture(const void* a, const void* b, const void* c, const void* d, const void* e, const void* f2)
{
    auto fn = (long(WINAPI*)(const void*, const void*, const void*, const void*, const void*, const void*)) GetProcAddress(Real(), "PIXEndCapture");
    return fn ? fn(a, b, c, d, e, f2) : 0x80004001L;
}

__declspec(dllexport) long WINAPI fwd_PIXGetCaptureStatus(const void* a, const void* b, const void* c, const void* d, const void* e, const void* f2)
{
    auto fn = (long(WINAPI*)(const void*, const void*, const void*, const void*, const void*, const void*)) GetProcAddress(Real(), "PIXGetCaptureStatus");
    return fn ? fn(a, b, c, d, e, f2) : 0x80004001L;
}

__declspec(dllexport) long WINAPI fwd_SetAppCompatStringPointer(const void* a, const void* b, const void* c, const void* d, const void* e, const void* f2)
{
    auto fn = (long(WINAPI*)(const void*, const void*, const void*, const void*, const void*, const void*)) GetProcAddress(Real(), "SetAppCompatStringPointer");
    return fn ? fn(a, b, c, d, e, f2) : 0x80004001L;
}

__declspec(dllexport) long WINAPI fwd_UpdateHMDEmulationStatus(const void* a, const void* b, const void* c, const void* d, const void* e, const void* f2)
{
    auto fn = (long(WINAPI*)(const void*, const void*, const void*, const void*, const void*, const void*)) GetProcAddress(Real(), "UpdateHMDEmulationStatus");
    return fn ? fn(a, b, c, d, e, f2) : 0x80004001L;
}

} // extern "C"
