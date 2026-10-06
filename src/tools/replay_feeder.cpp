// replay_feeder.cpp — offline harness (F2 gate): feeds REAL captures through
// the shared ABI into a locally spawned sli_engine, no game required.
//
// Usage: replay_feeder <captures_dir> <engine_exe>
//   <captures_dir> must contain cap_params.txt + cap_*.col raw dumps
//   (POC capture format: raw rows, pitch = (W*bpp + 255) & ~255).
//
// What it does:
//  1. Creates the frame mapping "Local\sli_frame_<our pid>" and every shared
//     resource the host would (color in/out, guide bufs, produce/done fences)
//     — using the WARP adapter if no NVIDIA GPU is free. NOTE (R95 audit,
//     doc honesty): WARP is NOT a full end-to-end run — the engine treats
//     a {0,0} LUID as "no adapter" and exits; the identity gate needs a
//     real NVIDIA adapter to back the shared heaps (see the R89d NOTE at
//     the spawn site). WARP only proves the feeder side.
//  2. Spawns the engine with --luid/--w/--h/--cf/--map like the host does
//     (suspended, duplicated handles into the handshake, resume).
//  3. Per capture frame: writes the color bytes into the shared input,
//     bumps FrameScalars, signals produce.
//  4. Collects done fences, reads the delta buffer out, prints stats
//     (mean/std/%nonzero) and saves the first frames as .rgba16f.
//
// Gates it proves:
//   G1 identity: with the vendor unable to arm (WARP or missing runtime),
//      every delta must be ALL ZEROS (fail-safe = untouched frame).
//   G2 content: with a real GPU + vendor runtime present, deltas must show
//      content (nonzero fraction > 2% on a real capture).
//   G3 stability: N frames without the engine dying (done fences arrive).
#include "shared/abi.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace sli;

namespace {

struct Params
{
    // (R89b: df/mf/ow/oh/flags/mw/mh were parsed and never read — the feeder
    //  drives w/h/cf and the engine's guide dims come from the handshake.)
    unsigned w = 0, h = 0, cf = 0;
};

bool ReadParams(const char* dir, Params& p)
{
    std::string path = std::string(dir) + "\\cap_params.txt";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "r") != 0 || !f) return false;
    char line[256];
    while (fgets(line, sizeof line, f))
    {
        char k[64]; unsigned v = 0;
        if (sscanf_s(line, "%63[^=]=%u", k, (unsigned) sizeof k, &v) == 2)
        {
            if      (!strcmp(k, "w")) p.w = v;
            else if (!strcmp(k, "h")) p.h = v;
            else if (!strcmp(k, "cf")) p.cf = v;
        }
    }
    fclose(f);
    return p.w && p.h && p.cf;
}

unsigned BppFor(unsigned fmt)
{
    switch (fmt)
    {
        case 1: case 2: case 3: case 4: return 16;
        case 5: case 6: case 7: case 8:
        case 9: case 10: case 11: case 12: case 13: case 14: case 15:
        case 16: case 17: case 18: case 19:
        case 20: case 21: case 22: case 23: return 8;
        case 56: case 57: case 58: case 59: return 2;
        default: return 4;
    }
}

unsigned PitchFor(unsigned w, unsigned bpp) { return (w * bpp + 255u) & ~255u; }

std::vector<BYTE> LoadRaw(const char* dir, const char* name)
{
    std::string p = std::string(dir) + "\\" + name;
    FILE* f = nullptr;
    std::vector<BYTE> out;
    if (fopen_s(&f, p.c_str(), "rb") != 0 || !f) return out;
    // R82l: size the buffer from the FILE, not from a caller hint — the
    // caller passed 1 GiB per file (3 kinds x 8 frames = up to 24 GiB of
    // commit before a single byte was read; machines without the commit
    // charge died in LoadFrameSet with a misleading "no captures").
    if (_fseeki64(f, 0, SEEK_END) != 0) { fclose(f); return out; }
    const long long fsz = (long long) _ftelli64(f);
    if (fsz <= 0) { fclose(f); return out; }
    _fseeki64(f, 0, SEEK_SET);
    out.resize((size_t) fsz);
    size_t got = fread(out.data(), 1, out.size(), f);
    fclose(f);
    out.resize(got);
    return out;
}

// run1-style capture set: frames suffixed a..e, files cap_<kind><suffix>.raw
struct FrameSet
{
    std::vector<BYTE> color, depth, motion;
};

bool LoadFrameSet(const char* dir, char suf, FrameSet& fs)
{
    char nm[64];
    _snprintf_s(nm, 64, _TRUNCATE, "cap_color%c.raw", suf);
    fs.color = LoadRaw(dir, nm);
    _snprintf_s(nm, 64, _TRUNCATE, "cap_depth%c.raw", suf);
    fs.depth = LoadRaw(dir, nm);
    _snprintf_s(nm, 64, _TRUNCATE, "cap_motion%c.raw", suf);
    fs.motion = LoadRaw(dir, nm);
    return !fs.color.empty() && !fs.depth.empty() && !fs.motion.empty();
}

float HalfToFloat(uint16_t h)
{
    uint32_t sign = (uint32_t)(h >> 15) << 31;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t man = h & 0x3FF;
    uint32_t bits;
    if (exp == 0 && man == 0) bits = sign;
    else if (exp == 0) { /* subnormal: rare in our data, approximate */ bits = sign; }
    else if (exp == 0x1F) bits = sign | 0x7F800000u | (man << 13); // Inf/NaN
    else bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

} // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (GetEnvironmentVariableA("SLI_FEEDER_SELFTEST", nullptr, 0))
    {
        std::printf("selftest reached main\n");
        return 0;
    }
    if (argc < 3)
    {
        std::printf("usage: replay_feeder <captures_dir> <engine_exe>\n");
        return 1;
    }
    std::printf("phase: start\n");
    Params p;
    if (!ReadParams(argv[1], p))
    {
        std::printf("cap_params.txt missing/invalid in %s\n", argv[1]);
        return 1;
    }
    std::printf("phase: params ok\n");
    const unsigned bpp = BppFor(p.cf);
    const unsigned pitch = PitchFor(p.w, bpp);
    const UINT64 bytes = (UINT64) pitch * p.h;
    std::printf("captures: %ux%u cf=%u bpp=%u pitch=%u (%llu B/frame)\n",
                p.w, p.h, p.cf, bpp, pitch, (unsigned long long) bytes);

    // golpe 2 — the depth PLANE the engine will consume (R32_FLOAT at its
    // own pitch). Modes (SLI_FEEDER_DEPTH): 'one' (default, the historical
    // flat near proxy), 'zero' (what the live host ships today), 'real'
    // (the capture's EVEN channel — RDR2's log depth, sky 0). The raw
    // capture is R32G32-like pairs at the COLOR pitch; only .x carries
    // depth (verified offline: even 0..0.063 log, odd is garbage).
    char depthMode[16] { 'o','n','e',0 };
    GetEnvironmentVariableA("SLI_FEEDER_DEPTH", depthMode, 16);
    if (!depthMode[0]) { depthMode[0]='o'; depthMode[1]='n'; depthMode[2]='e'; depthMode[3]=0; }
    const unsigned depthPitch = PitchFor(p.w, 4);
    const UINT64 depthBytes = (UINT64) depthPitch * p.h;
    std::printf("depth plane: mode=%s pitch=%u (%llu B)\n",
                depthMode, depthPitch, (unsigned long long) depthBytes);

    // --- device: prefer any NVIDIA; fall back to WARP (identity gate mode)
    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device> dev;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 2;
    LUID luid {};
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> ad;
        if (factory->EnumAdapters1(i, ad.ReleaseAndGetAddressOf()) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(ad->GetDesc1(&d)) && d.VendorId == 0x10DE)
        {
            if (SUCCEEDED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev))))
            {
                luid = d.AdapterLuid;
                break;
            }
        }
    }
    if (!dev)
    {
        ComPtr<IDXGIAdapter1> warpAd;
        factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAd));
        if (FAILED(D3D12CreateDevice(warpAd.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev))))
        {
            std::printf("no device (NVIDIA or WARP)\n");
            return 2;
        }
        std::printf("WARP device (feeder side only — engine will exit on "
                    "LUID {0,0}; identity gate needs a real adapter)\n");
    }

    std::printf("phase: device ok\n");
    // --- queue + shared resources (mirror of the host arm path)
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
        FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd))))
        return 2;
    cmd->Close();

    auto MakeSharedBuf = [&](UINT64 sz, ComPtr<ID3D12Heap>& heap,
                             ComPtr<ID3D12Resource>& buf) -> bool {
        D3D12_HEAP_DESC hd{};
        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.SizeInBytes = (sz + 65535) & ~65535ull;
        hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
        if (FAILED(dev->CreateHeap(&hd, IID_PPV_ARGS(&heap)))) return false;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = sz; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        return SUCCEEDED(dev->CreatePlacedResource(heap.Get(), 0, &d,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buf)));
    };
    auto MakeSharedFence = [&](ComPtr<ID3D12Fence>& f) -> bool {
        return SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED |
                   D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(&f)));
    };

    ComPtr<ID3D12Heap> hIn, hOut, hGd[3], hGm[3];
    ComPtr<ID3D12Resource> bIn, bOut[sli::kOutRingSlots], bGd[3], bGm[3];
    ComPtr<ID3D12Fence> fProduce, fDone;
    if (!MakeSharedBuf(bytes, hIn, bIn))
    {
        std::printf("shared bufs failed\n");
        return 3;
    }
    // R90 (#11): the out side mirrors the host's OutRing — one heap, three
    // placed slots at RoundUp64K(bytes) strides (the abi.h contract).
    {
        const UINT64 stride = (bytes + 65535) / 65536 * 65536;
        D3D12_HEAP_DESC hd{};
        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.SizeInBytes = stride * sli::kOutRingSlots;
        hd.Alignment = 65536;
        hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
        if (FAILED(dev->CreateHeap(&hd, IID_PPV_ARGS(&hOut)))) return 3;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        for (uint64_t k = 0; k < sli::kOutRingSlots; ++k)
            if (FAILED(dev->CreatePlacedResource(hOut.Get(), k * stride, &d,
                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                    IID_PPV_ARGS(&bOut[k]))))
                return 3;
    }
    for (int i = 0; i < 3; ++i)
        if (!MakeSharedBuf(depthBytes, hGd[i], bGd[i]) ||
            !MakeSharedBuf(bytes, hGm[i], bGm[i]))
            return 3;
    if (!MakeSharedFence(fProduce) || !MakeSharedFence(fDone)) return 3;

    std::printf("phase: shared resources ok\n");
    // --- mapping + handshake
    wchar_t mapName[64];
    FrameMapName(GetCurrentProcessId(), mapName, 64);
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                    0, (DWORD) MAP_SIZE, mapName);
    if (!map) return 4;
    auto* scalars = (FrameScalars*) MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, MAP_SIZE);
    if (!scalars) return 4;
    auto* hs = (Handshake*)((uint8_t*) scalars + HANDSHAKE_OFFSET);
    memset((void*) hs, 0, sizeof *hs);
    hs->magic = HANDSHAKE_MAGIC;
    // Shared NT handles are created on the HEAP (POC-proven pattern), then
    // duplicated into the child; the engine opens them on ITS device.
    auto NtHeap = [&](ID3D12Heap* hp) -> uint64_t {
        HANDLE h{};
        if (FAILED(dev->CreateSharedHandle(hp, nullptr, GENERIC_ALL, nullptr, &h)))
            return 0;
        return (uint64_t)(uintptr_t) h;
    };
    auto NtFence = [&](ID3D12Fence* f) -> uint64_t {
        // ID3D12Device::CreateSharedHandle works for fences (POC pattern).
        HANDLE h{};
        if (FAILED(dev->CreateSharedHandle(f, nullptr, GENERIC_ALL, nullptr, &h)))
            return 0;
        return (uint64_t)(uintptr_t) h;
    };
    hs->hic = NtHeap(hIn.Get());
    hs->hoc = NtHeap(hOut.Get());
    hs->hpf = NtFence(fProduce.Get());
    hs->hdf = NtFence(fDone.Get());
    for (int i = 0; i < 3; ++i)
    {
        hs->hgd[i] = NtHeap(hGd[i].Get());
        hs->hgm[i] = NtHeap(hGm[i].Get());
    }
    hs->guideDepthFmt = 41;  // R32_FLOAT
    hs->guideMvFmt = 34;     // R16G16_FLOAT
    hs->guideDepthPitch = depthPitch;
    hs->guideMvPitch = pitch;
    // golpe 2 CORRECTION: the capture's own geometry decides the labelling,
    // not a guess. Measured plane: sky = 0.0, near geometry = LARGER
    // (0..0.063) — near-is-large = INVERTED labelling, matching the game's
    // own create-flags bit 3 (live log: depthInverted=1). Parity with the
    // references = the game's content WITH the game's flag: real -> 1.
    // SLI_FEEDER_DEPTH_INV=0 A/Bs the other labelling if ever needed.
    {
        char inv[8] {};
        const DWORD nInv = GetEnvironmentVariableA("SLI_FEEDER_DEPTH_INV", inv, 8);
        hs->depthInverted = (nInv > 0 && inv[0] == '0') ? 0u : 1u;
    }
    hs->guideW = p.w;
    hs->guideH = p.h;
    hs->version = ABI_VERSION;

    auto* tuning = (Tuning*)((uint8_t*) scalars + TUNING_OFFSET);
    memset((void*) tuning, 0, sizeof *tuning);
    tuning->magic = TUNING_MAGIC;
    // (R89b: offloadOn/gpuIndex writes removed — the ENGINE never reads
    //  them; they are host-owned transport decisions.)
    tuning->nrOn = GetEnvironmentVariableA("SLI_FEEDER_NR_OFF", nullptr, 0)
                       ? 0u : 1u; // G1 arm: identity must be bit-exact zero
    tuning->nrStyle = 0;
    tuning->nrWorkScale = 1.0f;
    // golpe 1b: PQV + ratio valve, A/B-able offline like BOOST/TINT.
    {
        char b[16] {};
        const DWORD n = GetEnvironmentVariableA("SLI_FEEDER_PQV", b, 16);
        tuning->nrPerfQuality = (n > 0 && n < 16) ? (float) atof(b) : 2.0f;
    }
    {
        char b[16] {};
        const DWORD n = GetEnvironmentVariableA("SLI_FEEDER_RATIO_PIN", b, 16);
        // default OPEN (0.0) — the whole point of golpe 1b
        tuning->nrRatioPin = (n > 0 && n < 16) ? (float) atof(b) : 0.0f;
    }
    {
        // A/B knob like SLI_FEEDER_TINT: the gain exponent (1 = exact).
        char b[16] {};
        const DWORD n = GetEnvironmentVariableA("SLI_FEEDER_BOOST", b, 16);
        tuning->nrBoost = (n > 0 && n < 16) ? (float) atof(b) : 1.0f;
    }
    {
        // golpe 4 A/B: work scale (default 1.0 — residual is inert there;
        // 0.25 exercises the matched-residual path hard).
        char b[16] {};
        const DWORD n = GetEnvironmentVariableA("SLI_FEEDER_WORK", b, 16);
        tuning->nrWorkScale = (n > 0 && n < 16) ? (float) atof(b) : 1.0f;
    }
    {
        // golpe 4 A/B: matched-residual compose (default ON — the host's
        // structural default; 0 = classic rollback).
        char b[16] {};
        const DWORD n = GetEnvironmentVariableA("SLI_FEEDER_RESIDUAL", b, 16);
        tuning->nrResidual = (n > 0 && n < 16) ? (float) atof(b) : 1.0f;
    }
    tuning->nrTint = GetEnvironmentVariableA("SLI_FEEDER_TINT", nullptr, 0)
                        ? 1u : 0u; // tint gate: paint delta red/blue
    // R82g.6 root-cause probe: discard the runtime's temporal history EVERY
    // frame (SLI_FEEDER_RESET=1) — if the measured 5-state gain cycle dies
    // with this, the cycle's ORIGIN is the runtime's internal history.
    tuning->forceReset = GetEnvironmentVariableA("SLI_FEEDER_RESET", nullptr, 0)
                        ? 1u : 0u;
    // R82g.4: SLI_FEEDER_MODELH removed (answer fills the whole buffer).

    std::printf("phase: mapping ok\n");
    // --- spawn the engine (suspended) like the host does
    STARTUPINFOW si{}; si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    wchar_t cmdl[1024];
    wchar_t exeW[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, exeW, MAX_PATH);
    // The engine takes --luid <lo> <hi> (same order as the host's SpawnEngine).
    // NOTE (R89d): WARP mode passes LUID {0,0} — the engine's adapter pick
    // treats that as "no NVIDIA found" and exits; the identity gate works
    // only when a real (or LUID-able) adapter backs the shared heaps.
    // WARP is therefore NOT supported end-to-end despite the banner below.
    _snwprintf_s(cmdl, 1024, _TRUNCATE,
                 L"\"%s\" --luid %llu %llu --w %u --h %u --cf %u --map %s",
                 exeW, (unsigned long long)(uint32_t) luid.LowPart,
                 (unsigned long long)(uint32_t) luid.HighPart, p.w, p.h, p.cf, mapName);
    // cwd = engine's own directory: the vendor forwarder resolves
    // nvngx.dll_dlssnr.dll relative to it (the host does the same with the
    // game dir).
    wchar_t engDir[MAX_PATH];
    wcscpy_s(engDir, exeW);
    wchar_t* slash = wcsrchr(engDir, L'\\');
    if (slash) *slash = 0;
    if (!CreateProcessW(nullptr, cmdl, nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, engDir, &si, &pi))
    {
        std::printf("CreateProcess failed (%u)\n", GetLastError());
        return 5;
    }
    auto DupTo = [&](uint64_t src) -> uint64_t {
        HANDLE in = (HANDLE)(uintptr_t) src, out{};
        if (!DuplicateHandle(GetCurrentProcess(), in, pi.hProcess, &out, 0,
                             FALSE, DUPLICATE_SAME_ACCESS))
            return 0;
        return (uint64_t)(uintptr_t) out;
    };
    hs->hic = DupTo(hs->hic); hs->hoc = DupTo(hs->hoc);
    hs->hpf = DupTo(hs->hpf); hs->hdf = DupTo(hs->hdf);
    for (int i = 0; i < 3; ++i)
    {
        hs->hgd[i] = DupTo(hs->hgd[i]);
        hs->hgm[i] = DupTo(hs->hgm[i]);
    }
    std::printf("phase: spawned\n");
    ResumeThread(pi.hThread);
    std::printf("engine spawned pid %u — waiting READY...\n", pi.dwProcessId);

    for (int i = 0; i < 300 && hs->ready != READY_MAGIC; ++i) Sleep(100);
    if (hs->ready != READY_MAGIC)
    {
        std::printf("engine never READY (timeout 30 s) — GATE FAIL\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        return 6;
    }
    std::printf("engine READY\n");

    // --- load capture frames (run1 layout: cap_{color,depth,motion}{a..e}.raw)
    char loopsEnv[16] {};
    GetEnvironmentVariableA("SLI_FEEDER_LOOPS", loopsEnv, 16);
    const int loops = atoi(loopsEnv) > 0 ? atoi(loopsEnv) : 1;
    std::vector<FrameSet> frames;
    for (char suf : { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' })
    {
        FrameSet fs;
        if (!LoadFrameSet(argv[1], suf, fs)) break;
        frames.push_back(std::move(fs));
    }
    if (frames.empty())
    {
        std::printf("no cap_color*.raw captures found\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        return 1;
    }
    // R82g.7 probe: SLI_FEEDER_ONEFRAME repeats ONE capture frame every
    // iteration — with SLI_FEEDER_JITTER the ONLY varying input is the
    // jitter, so any answer oscillation is the model's internal recursion
    // reacting to the jittered input (nothing else can move).
    {
        char one[16] {};
        if (GetEnvironmentVariableA("SLI_FEEDER_ONEFRAME", one, 16) && atoi(one) != 0)
        {
            frames.resize(1);
            std::printf("ONEFRAME: repeating cap frame 'a' every iteration\n");
        }
    }
    std::printf("%zu capture frames loaded\n", frames.size());

    // host-side staging (upload + readback)
    ComPtr<ID3D12Resource> upload, readback;
    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
        if (FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &d,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
            return 7;
        D3D12_HEAP_PROPERTIES rp{}; rp.Type = D3D12_HEAP_TYPE_READBACK;
        if (FAILED(dev->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &d,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
            return 7;
    }
    ComPtr<ID3D12Fence> localFence;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&localFence));
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 localVal = 0;

    int contentFrames = 0;

    std::printf("loops: %d\n", loops);
    for (size_t n = 0; n < frames.size() * (size_t) loops; ++n)
    {
        const size_t fn = n % frames.size();
        // capture bytes → shared inputs (color + depth + motion)
        {
            uint8_t* dst = nullptr; D3D12_RANGE rd{};
            upload->Map(0, &rd, (void**) &dst);
            const size_t maxUp = (size_t) bytes;
            memcpy(dst, frames[fn].color.data(),
                   frames[fn].color.size() < maxUp ? frames[fn].color.size() : maxUp);
            upload->Unmap(0, nullptr);
            if (FAILED(cmd->Reset(alloc.Get(), nullptr))) return 8;
            cmd->CopyBufferRegion(bIn.Get(), 0, upload.Get(), 0,
                                  frames[fn].color.size() < bytes
                                      ? frames[fn].color.size() : bytes);
            // guides: same upload staging, sequential copies
            // golpe 2: build the depth PLANE per mode (maxUp covers both
            // pitches: color bytes >= depthBytes always here)
            upload->Map(0, &rd, (void**) &dst);
            if (depthMode[0] == 'r')
            {
                // real: pull the even float of each 8-byte pair, row by row
                // (capture pitch = color pitch; plane pitch = depthPitch)
                const BYTE* src = frames[fn].depth.data();
                const size_t srcPitch = (size_t) pitch;
                for (unsigned y = 0; y < p.h; ++y)
                {
                    const size_t srcRow = y * srcPitch;
                    const size_t dstRow = (size_t) y * depthPitch;
                    for (unsigned x = 0; x < p.w; ++x)
                    {
                        float v = 0.0f;
                        if (srcRow + (size_t) x * 8 + 4 <= frames[fn].depth.size())
                            memcpy(&v, src + srcRow + (size_t) x * 8, 4);
                        memcpy(dst + dstRow + (size_t) x * 4, &v, 4);
                    }
                }
            }
            else
            {
                const float fill = (depthMode[0] == 'z') ? 0.0f : 1.0f;
                const size_t nF = (size_t) depthPitch * p.h / 4;
                float* f = (float*) dst;
                for (size_t k = 0; k < nF && k * 4 < maxUp; ++k) f[k] = fill;
            }
            upload->Unmap(0, nullptr);
            // R82l slot parity: the engine reads bufGuideX[frame % 3] for
            // frame n+1 (its counter is 1-based after ++frame). Writing the
            // CURRENT capture to (n+2)%3 == (n-1)%3 fed the guides TWO
            // frames behind live (live: same-frame) — every offline probe
            // ran with temporal coherence the live host never has.
            cmd->CopyBufferRegion(bGd[(n + 1) % 3].Get(), 0, upload.Get(), 0,
                                  depthBytes);
            upload->Map(0, &rd, (void**) &dst);
            memcpy(dst, frames[fn].motion.data(),
                   frames[fn].motion.size() < maxUp ? frames[fn].motion.size() : maxUp);
            upload->Unmap(0, nullptr);
            cmd->CopyBufferRegion(bGm[(n + 1) % 3].Get(), 0, upload.Get(), 0,
                                  frames[fn].motion.size() < bytes
                                      ? frames[fn].motion.size() : bytes);
            cmd->Close();
            ID3D12CommandList* cl = cmd.Get();
            queue->ExecuteCommandLists(1, &cl);
        }
        scalars->frame = (uint64_t) n + 1;
        scalars->renderW = p.w; scalars->renderH = p.h;
        scalars->reset = (n == 0) ? 1 : 0;
        // R82g.3: MV (1,1)px desplaza el history del runtime 1 px por frame —
        // con frames congelados eso cicla el answer (medido, período 5).
        // MV cero = static scene honesta para el probe offline.
        scalars->mvScaleX = scalars->mvScaleY = 0.0f;
        // R82g.6 probe: feed a TAA-style jitter pattern (2x2 Bayer, in pixels)
        // when SLI_FEEDER_JITTER is present — does the model's answer track
        // the jitter (it does not compensate) or stay put (it does)?
        if (GetEnvironmentVariableA("SLI_FEEDER_JITTER", nullptr, 0))
        {
            static const float pat[4][2] = { {0.0f,0.0f}, {0.5f,0.5f}, {0.5f,0.0f}, {0.0f,0.5f} };
            scalars->jitterX = pat[n & 3][0];
            scalars->jitterY = pat[n & 3][1];
        }
        scalars->engineResult = 0;
        queue->Signal(fProduce.Get(), (uint64_t) n + 1);

        fDone->SetEventOnCompletion((uint64_t) n + 1, evt);
        if (WaitForSingleObject(evt, 30000) != WAIT_OBJECT_0)
        {
            std::printf("frame %zu: done timeout — GATE FAIL (G3)\n", n);
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            return 9;
        }

        // delta readback — from THIS frame's ring slot
        if (FAILED(cmd->Reset(alloc.Get(), nullptr))) return 8;
        cmd->CopyBufferRegion(readback.Get(), 0,
                              bOut[(uint64_t) n % sli::kOutRingSlots].Get(),
                              0, bytes);
        cmd->Close();
        {
            ID3D12CommandList* cl = cmd.Get();
            queue->ExecuteCommandLists(1, &cl);
            ++localVal;
            queue->Signal(localFence.Get(), localVal);
            localFence->SetEventOnCompletion(localVal, evt);
            WaitForSingleObject(evt, 5000);
        }

        D3D12_RANGE wr{};
        uint8_t* src = nullptr;
        readback->Map(0, &wr, (void**) &src);
        // Stats over the RGB channels ONLY — alpha is 1.0 in both the zero
        // pattern and the shader's float4(d,1) identity, so it says nothing.
        double sum = 0; size_t nz = 0, cnt = 0;
        if (bpp == 8)
        {
            const uint16_t* u = (const uint16_t*)(void*) src;
            for (unsigned y = 0; y < p.h; ++y)
            {
                const uint16_t* row = u + (size_t) y * (pitch / 2);
                for (unsigned x = 0; x < p.w * 4; ++x)
                {
                    if ((x & 3) == 3) continue; // skip alpha
                    uint16_t h = row[x];
                    sum += HalfToFloat(h);
                    ++cnt;
                    if (h != 0) ++nz;
                }
            }
        }
        double mean = cnt ? sum / cnt : 0;
        bool hasContent = cnt && 100.0 * nz / cnt > 1.0;
        if (hasContent) ++contentFrames;
        std::printf("frame %zu: res=%u | delta mean %+.5f nonzero %5.1f%%%s\n",
                    n, scalars->engineResult, mean,
                    cnt ? 100.0 * nz / cnt : 0.0, hasContent ? "  <- CONTENT" : "");
        {
            char outp[MAX_PATH];
            _snprintf_s(outp, MAX_PATH, _TRUNCATE, "feeder_delta_%zu.rgba16f", n);
            FILE* fo = nullptr;
            if (fopen_s(&fo, outp, "wb") == 0 && fo)
            {
                fwrite(src, 1, (size_t) pitch * p.h, fo);
                fclose(fo);
            }
        }
        readback->Unmap(0, nullptr);
    }

    if (contentFrames == 0)
        std::printf("G1 IDENTITY PASS (all deltas zero — fail-safe intact)\n");
    else
        std::printf("G2 CONTENT: %d/%zu frames with delta content\n",
                    contentFrames, frames.size() * (size_t) loops);
    std::printf("G3 STABILITY: %zu frames, engine alive\n", frames.size() * (size_t) loops);
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return 0;
}
