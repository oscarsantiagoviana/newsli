// load_hook v4 — faithful to OptiScaler's working architecture, assembled
// from the pieces read line by line (hooks/Kernel_Hooks.cpp,
// hooks/LibraryLoad_Hooks.cpp, proxies/Ntdll_Proxy.h):
//
//   - LoadLibraryExW/ExA/W detoured in kernel32 AND kernelbase.
//   - ONLY the bare name "nvngx.dll" is redirected (to our nvngx.dll
//     module). "_nvngx.dll" is NEVER touched — it is the core's parameter
//     -provider sibling loaded internally by the core itself; feeding it a
//     foreign module is what dead-locked our earlier attempt (the core
//     handed itself our stub). OptiScaler excludes it the same way.
//   - The requester on this game is the DRIVER (nvwgf2umx resolves NGX for
//     RDR2 through its D3D integration; the game never loads nvngx
//     itself). Redirecting the driver's load is exactly how OptiScaler
//     engages — its NGX exports then serve the driver, and through the
//     driver, the game.
//   - Loads of anything else pass through; ngx-shaped ones are logged as
//     evidence.
//
// The real core is NOT loaded here — the ngx host loads it itself through
// the LdrLoadDll direct path (OptiScaler's NtdllProxy::LoadLibraryExW_Ldr)
// when the first NGX init arrives, so the loader sees it exactly once and
// outside any redirected call chain.
#include <windows.h>
#include <intrin.h>
#include "detours.h"
#include "shared/log.h"
#include <string>

// ---- the loader bypass, OptiScaler's NtdllProxy::LoadLibraryExW_Ldr ----
using NtStatus = long;
struct NgxUnicodeString
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};
using FnRtlInitUnicodeString = void(NTAPI*)(NgxUnicodeString*, PWSTR);
using FnLdrLoadDll = NtStatus(NTAPI*)(PWSTR, PULONG, NgxUnicodeString*, PHANDLE);

// (R95 audit: CoprocLdrLoadExW — a non-static, never-called duplicate of
//  the LdrLoadDll bypass nvngx_host.cpp implements privately — removed.
//  The load path that ships is CoreLoadDirect + the kernel32 detours.)

namespace {

HMODULE g_host = nullptr; // our nvngx.dll module — the redirect target
HMODULE g_self = nullptr; // this dxgi proxy

using FnLoadExW = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using FnLoadW = HMODULE(WINAPI*)(LPCWSTR);
FnLoadExW o_k32ExW = nullptr;
FnLoadExW o_kbExW = nullptr;
FnLoadW o_k32W = nullptr;

bool OurCaller()
{
    HMODULE m = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (LPCWSTR) _ReturnAddress(),
                              &m) &&
           (m == g_self || m == g_host);
}

// OptiScaler's hook-list semantic: bare basename "nvngx.dll" ONLY.
bool IsNvngxName(const wchar_t* name)
{
    if (name == nullptr || name[0] == 0)
        return false;
    const wchar_t* base = name;
    for (const wchar_t* p = name; *p != 0; ++p)
        if (*p == L'\\' || *p == L'/')
            base = p + 1;
    wchar_t buf[MAX_PATH] = {};
    wcsncpy_s(buf, MAX_PATH, base, _TRUNCATE);
    _wcslwr_s(buf, MAX_PATH);
    return wcscmp(buf, L"nvngx.dll") == 0;
}

HMODULE WINAPI hk_k32ExW(LPCWSTR n, HANDLE f, DWORD fl)
{
    if (!OurCaller())
    {
        if (IsNvngxName(n))
            return g_host;
        if (n != nullptr && wcsstr(n, L"ngx") != nullptr)
            sli::Log("dxgi_proxy: [k32ExW] load (passthrough): %ls", n);
    }
    return o_k32ExW(n, f, fl);
}

HMODULE WINAPI hk_kbExW(LPCWSTR n, HANDLE f, DWORD fl)
{
    if (!OurCaller())
    {
        if (IsNvngxName(n))
            return g_host;
        if (n != nullptr && wcsstr(n, L"ngx") != nullptr)
            sli::Log("dxgi_proxy: [kbExW] load (passthrough): %ls", n);
    }
    return o_kbExW(n, f, fl);
}

HMODULE WINAPI hk_k32W(LPCWSTR n)
{
    if (!OurCaller())
    {
        if (IsNvngxName(n))
            return g_host;
        if (n != nullptr && wcsstr(n, L"ngx") != nullptr)
            sli::Log("dxgi_proxy: [k32W] load (passthrough): %ls", n);
    }
    return o_k32W(n);
}

} // namespace

void InstallNgxLoadHook(HMODULE self, HMODULE host)
{
    if (o_k32ExW != nullptr)
        return; // already installed
    g_self = self;
    g_host = host;

    const HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    const HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    if (k32 != nullptr)
    {
        o_k32ExW = (FnLoadExW) GetProcAddress(k32, "LoadLibraryExW");
        o_k32W = (FnLoadW) GetProcAddress(k32, "LoadLibraryW");
    }
    if (kb != nullptr)
        o_kbExW = (FnLoadExW) GetProcAddress(kb, "LoadLibraryExW");

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (o_k32ExW != nullptr)
        DetourAttach(&(PVOID&) o_k32ExW, hk_k32ExW);
    if (o_k32W != nullptr)
        DetourAttach(&(PVOID&) o_k32W, hk_k32W);
    if (o_kbExW != nullptr)
        DetourAttach(&(PVOID&) o_kbExW, hk_kbExW);
    const LONG err = DetourTransactionCommit();
    sli::Log("dxgi_proxy: nvngx load redirect %s (detours err %d)",
            err == NO_ERROR ? "installed" : "INSTALL FAILED", (int) err);
}
