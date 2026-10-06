// nr_vendor.h — vendor DLSS-NR model as the engine's in-process NR backend.
//
// Stub-validated recipe (POC R66):
//   core (_nvngx.dll from the registry) Init_Ext(app 101616311, cwd, dev, 0x15)
//   -> GetCapabilityParameters (the driver's block, not AllocateParameters)
//   -> discover_float_slot (driver-specific vtable slot)
//   -> forwarder nvngx.dll_dlssnr.dll: dlssnr_call_create(...)  [the
//      forwarder runs snippet Init_Ext(0x24480451) + create(18); the
//      snippet's caller-gate checks the CALLING MODULE, whose path contains
//      "nvngx.dll" — the exe name is irrelevant]
//   -> dlssnr_call_evaluate_v2(cmd, feature, P, colorBGRA8, depth, mvRG16,
//      outRGBA16F, ...)
//
// Formats validated on sm_86 (RTX 3060): BGRA8 sRGB color input YES;
// RGBA16F input CRASHES (0xC0000409); output BGRA8 = bit-exact passthrough;
// output RGBA16F in display domain (same encoding as the input) is valid.
// No depth = Copy; depth 0.5 + DepthInverted=1 = red running. Zero MVs OK.
//
// GPU codec (nr_encode.hlsl / nr_delta.hlsl): encode HDR->BGRA8, decode to
// the display-domain DELTA — the engine's only output. Exposure =
// 0.18/median(lum) from the PREVIOUS frame's tile grid (1-frame lag, fork
// style: exposure drifts slowly, the lag is invisible and avoids a
// per-frame round-trip). One full GPU frame in a single command-list
// session: encode -> evaluate -> decode-delta -> tiles->readback. No CPU
// pixel traffic on the vendor path.
//
// Ported from the POC's nr_vendor.h with the forensics deleted: frame-240
// dumps, stub guides A/B, and the full-frame decode PSO (psoDec). The
// delta is ALWAYS the output — a disarmed vendor means a zero delta, never
// an echo frame.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "shared/abi.h"
#include "shared/log.h"

using Microsoft::WRL::ComPtr;

// Embedded shader CSOs (CMake embed_shader(): dxc -T cs_6_0).
#include "g_nrEncodeCso_cso.h"
#include "g_nrDeltaCso_cso.h"
#include "g_nrStabDepthCso_cso.h"

// ---- param-block ABI (x64 vtable: this in RCX, as the stub found it) ----
using PFN_SetULL   = void(__thiscall*)(void*, const char*, unsigned long long);
using PFN_SetFloat = void(__thiscall*)(void*, const char*, float);
using PFN_SetUInt  = void(__thiscall*)(void*, const char*, unsigned int);
using PFN_GetFloat = int(__thiscall*)(void*, const char*, float*);
using PFN_GetCap   = int(__cdecl*)(void**);
using PFN_InitExt  = int(__cdecl*)(unsigned long long, const wchar_t*,
                                   ID3D12Device*, int, const void*);
using PFN_FwdQueryRatio = int(__cdecl*)(const wchar_t*, void*,
                                             unsigned int, float*);
using PFN_FwdCreate = void*(__cdecl*)(const wchar_t*, const wchar_t*, ID3D12Device*,
                                      ID3D12GraphicsCommandList*, void*, unsigned int,
                                      unsigned int, int, float, int, float, float,
                                      float, int, int);
using PFN_FwdEval   = int(__cdecl*)(ID3D12GraphicsCommandList*, void*, void*,
                                    ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
                                    ID3D12Resource*, unsigned int, unsigned int,
                                    unsigned int, unsigned int, unsigned int, unsigned int,
                                    unsigned int, unsigned int, unsigned int, unsigned int,
                                    int, int, float, int, float, float, float, int,
                                    float, float, float, float);
using PFN_FwdRelease = void(__cdecl*)(void*);

// f16 -> float (for the CPU tile-median)
inline float nr_half_to_float(unsigned h)
{
    const unsigned sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    if (exp == 0)
        return (man ? 6.103515625e-05f * (float)man : 0.f) * (sign ? -1.f : 1.f);
    if (exp == 31)
        return man ? 0.f : (sign ? -1e30f : 1e30f);
    return ldexpf((float)(1024 + man), (int)exp - 25) * (sign ? -1.f : 1.f);
}

struct NrVendor
{
    bool ok = false;
    HMODULE core = nullptr;
    HMODULE fwd = nullptr;
    void* P = nullptr;          // the driver's capability block
    int fslot = -1;             // vtable slot of Set(float)
    void* feature = nullptr;
    PFN_FwdEval fEval = nullptr;
    PFN_FwdQueryRatio fQueryRatio = nullptr;  // golpe 1b: read-only ratio probe
    PFN_FwdRelease fRel = nullptr;  // resolved once at forwarder load (R89b:
                                    //  was GetProcAddress at each release)
    unsigned seq = 0;           // evaluates sent (reset forces on the 1st)
    unsigned w = 0, h = 0;      // render dims (transport/orig/delta)
    // nrWorkScale: the model runs at work dims; the encode does an exact
    // coverage box src full -> work, the decode integrates work -> render.
    float workScale = 1.0f;
    unsigned workW = 0, workH = 0;

    // model textures (local, on the second GPU)
    ComPtr<ID3D12Resource> texColor;    // BGRA8 sRGB  (model input)
    ComPtr<ID3D12Resource> texOutModel; // RGBA16F     (model output)
    // real game guides — engine textures (raw fmt), reloaded per frame
    // from the shared triple buffer.
    ID3D12Resource* texGuideDepth = nullptr;
    ID3D12Resource* texGuideMV = nullptr;
    unsigned guideW = 0, guideH = 0;
    unsigned depthInverted = 0;
    unsigned rawDepthFmt = 0, rawMvFmt = 0; // the game's raw formats
    // CREATE-time knobs (moved slider -> rebuild via nrParamSeq)
    float kIntensity = 1.0f, kLocalStructure = 1.0f, kLocalTone = 1.0f,
          kSkinStructure = -1.0f;
    float pqvAtCreate = 2.0f;  // R90 probe: what PQV the feature got
    float activeRatio = 1.0f;   // golpe 1b: ratio in the block at create
    unsigned kStyle = 0;
    unsigned lastParamSeq = 0;
    // live tuning access for boost/tint inside ProcessFrame
    const volatile sli::Tuning* tuning = nullptr;
    // context saved for RebuildKnobs (captured in Init)
    void* fCreatePtr = nullptr;
    ID3D12Device* devPtr = nullptr;
    ID3D12GraphicsCommandList* cmdPtr = nullptr;
    std::wstring gameDirW;

    // GPU codec
    ComPtr<ID3D12RootSignature> rsEnc, rsDec, rsStab;
    ComPtr<ID3D12PipelineState> psoEnc, psoDelta, psoStab;
    ComPtr<ID3D12DescriptorHeap> heapCodec;
    UINT heapInc = 0;
    // R92: de-jittered DEPTH guide (same -j shift the colour seal gets).
    // Created lazily at first jittered frame; UAV R32_FLOAT guide dims.
    ComPtr<ID3D12Resource> texGuideDepthStab;
    // (R95 audit: stabDepthBound removed — written-only residue of the
    //  dead R92 stab path.)
    ComPtr<ID3D12Resource> bufTiles;     // RWStructuredBuffer<uint> (f16 bits)
    ComPtr<ID3D12Resource> rbTiles;      // tile-grid readback
    ComPtr<ID3D12Resource> bufFlow;      // R84: per-tile peak |MV| (uint2 f16)
    ComPtr<ID3D12Resource> rbFlow;       // R84: flow readback
    uint32_t* rbFlowPtr = nullptr;       // mapped for the frame loop
    uint32_t* rbTilesPtr = nullptr;
    unsigned tilesX = 0, tilesY = 0;   // WORK grid (encode luma tiles)
    unsigned gtX = 0, gtY = 0;         // RENDER grid (decode gain/flow tiles)
    float expoScale = 1.0f;              // RECONSTRUCTION exposure (R92f a):
                                         // the game's held preExposure — the
                                         // decode's E; cancels in the ratio
    float whitePoint = 1.0f;             // ENCODE white point (R92f b): the
                                         // adaptive meter 0.18/median(RAW
                                         // luma) — what the model SEES
    bool exposureHeld = false;           // a game value has been seen (R82j)
    std::vector<float> tileLums;         // CPU median (~5k values)
    // R90 (#6): preallocated readback scratch (the per-frame
    // std::vector<float> churn in FlowPeakPx/UpdateGainNorm was ~500 KB
    // of heap traffic on the frame-loop thread every frame).
    std::vector<float> flowScratch;      // FlowPeakPx percentile input
    std::vector<float> gainScratch[3];   // UpdateGainNorm channel inputs
    // R82c: per-channel gain-median tiles (frame normalisation); R82e:
    // uint2 tiles carry packed f16 RGB, normR is per-channel
    ComPtr<ID3D12Resource> bufGainTiles, rbGainTiles;
    uint32_t* rbGainTilesPtr = nullptr;
    struct F3 { float x, y, z; };
    F3 normR = { 1, 1, 1 };               // prev frame's per-channel medians
    unsigned gnDbg = 0;                   // gainnorm log throttle

private:
    // ---- param-block ABI helpers ----
    static void set_uint(void* p, const char* n, unsigned v)
    {
        void** vt = *(void***)p;
        ((PFN_SetUInt)vt[3])(p, n, v);
    }
    static void set_res(void* p, const char* n, void* r)
    {
        void** vt = *(void***)p;
        ((PFN_SetULL)vt[0])(p, n, (unsigned long long)r);
    }
    static void set_flt(void* p, int slot, const char* n, float v)
    {
        if (slot < 0) return;
        void** vt = *(void***)p;
        ((PFN_SetFloat)vt[slot])(p, n, v);
    }
    // The driver's param block has SetFloat at a driver-specific vtable
    // slot; probe slots 1..7 with a round-trip Get and take the first that
    // echoes the value back.
    // Float slot (R82f, deterministic — no blind sweeps: one earlier
    // 0..23 sweep called garbage past the vtable end and AV'd inside the
    // core). Facts: the header layout holds for the GETTERS (the fork's
    // working path reads floats with the typed Get, header slot 9) but
    // NOT for the setters (their probe finds the driver block's SetFloat
    // at 6, not 1). A missing key answers FAIL (-1), so the getter probe
    // must run on a PLANTED key. And a fake "echo" can come from the
    // resource pair (5/13) carrying the float's BITS as a pointer — so
    // the setter hunt compares only against a getter that answered a
    // float-typed plant.
    static int discover_float_slot(void* p)
    {
        void** vt = *(void***)p;
        using PFN_SetF = void (__thiscall*)(void*, const char*, float);
        using PFN_GetF = int (__thiscall*)(void*, const char*, float*);
        // (1) plant the probe key as a float through BOTH setter
        //     candidates (header 1, fork's driver answer 6); a wrong
        //     typed setter just stores it as another type, harmless.
        const char* k = "DLSSNR.FloatSlotProbe";
        ((PFN_SetF)vt[1])(p, k, 47.5f);
        ((PFN_SetF)vt[6])(p, k, 47.5f);
        // (2) the float-typed getter: header slot 9 first; if it does not
        //     answer the plant, hunt the bounded getter window 8..16.
        int g = -1;
        static const int kGet[] = { 9, 8, 10, 11, 12, 13, 14, 15, 16 };
        for (int gi : kGet)
        {
            float back = -9999.0f;
            if (((PFN_GetF)vt[gi])(p, k, &back) == 1 &&
                (back - 47.5f) < 0.01f && (47.5f - back) < 0.01f)
            {
                g = gi;
                break;
            }
        }
        if (g < 0) return -1;
        // (3) the setter whose float lands on that getter. Distinct key
        //     per candidate: no stale echoes from the plant.
        static const int kSet[] = { 1, 6, 2, 5, 7, 4, 3, 0 };
        for (int s : kSet)
        {
            const float want = 100.0f + (float) s * 0.25f;
            char key[40];
            sprintf_s(key, "DLSSNR.FloatSlotProbe%d", s);
            ((PFN_SetF)vt[s])(p, key, want);
            float back = -9999.0f;
            if (((PFN_GetF)vt[g])(p, key, &back) == 1 &&
                (back - want) < 0.001f && (want - back) < 0.001f)
                return s;
        }
        return -1;
    }
    static bool core_path(wchar_t out[MAX_PATH])
    {
        out[0] = L'\0';
        HKEY key;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"System\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore",
                          0, KEY_READ, &key) != ERROR_SUCCESS)
            return false;
        DWORD sz = MAX_PATH * sizeof(wchar_t);
        const LSTATUS st = RegQueryValueExW(key, L"NGXPath", nullptr, nullptr,
                                            (LPBYTE)out, &sz);
        RegCloseKey(key);
        return st == ERROR_SUCCESS && out[0] != L'\0';
    }

    static ComPtr<ID3D12Resource> make_tex(ID3D12Device* dev, DXGI_FORMAT fmt,
                                           unsigned w, unsigned h,
                                           D3D12_RESOURCE_FLAGS flags,
                                           D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = fmt; d.SampleDesc.Count = 1; d.Flags = flags;
        ComPtr<ID3D12Resource> tex;
        HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                  state, nullptr,
                                                  IID_PPV_ARGS(&tex));
        return SUCCEEDED(hr) ? tex : nullptr;
    }
    static ComPtr<ID3D12Resource> make_buf(ID3D12Device* dev, UINT64 bytes,
                                           D3D12_HEAP_TYPE type,
                                           D3D12_RESOURCE_STATES state,
                                           bool uav = false, const char* what = "")
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = type;
        D3D12_RESOURCE_DESC d {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = DXGI_FORMAT_UNKNOWN; d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (uav) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> buf;
        HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                  state, nullptr,
                                                  IID_PPV_ARGS(&buf));
        if (FAILED(hr))
            sli::Log("vendor: make_buf(%s) failed 0x%08X (dev removed: %d)",
                     what, (unsigned) hr,
                     (int) dev->GetDeviceRemovedReason());
        return SUCCEEDED(hr) ? buf : nullptr;
    }
    static void barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* tex,
                        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.pResource = tex;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        cmd->ResourceBarrier(1, &b);
    }
    static void uavBarrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* tex)
    {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = tex;
        cmd->ResourceBarrier(1, &b);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle(unsigned i) const
    {
        D3D12_GPU_DESCRIPTOR_HANDLE g =
            heapCodec->GetGPUDescriptorHandleForHeapStart();
        g.ptr += (SIZE_T)i * heapInc;
        return g;
    }

    // CPU handle for a heap slot (view writes) — valid once heapCodec
    // exists (InitCompute or later; the R92 lazy stab bind uses it).
    D3D12_CPU_DESCRIPTOR_HANDLE SlotCPU(unsigned i) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE hh =
            heapCodec->GetCPUDescriptorHandleForHeapStart();
        hh.ptr += (SIZE_T)i * heapInc;
        return hh;
    }

    // ---- GPU codec: pipelines + tiles + views ----
    // heap slots (R84 FINAL): 0=SRV texIn 1=SRV texGuideMV 2=UAV texColor
    //   3=UAV bufTiles 4=UAV bufFlow | 5=SRV texOutModel 6=SRV texIn
    //   7=UAV texOut 8=UAV bufGainTiles 9=UAV bufFlow(decode table ride)
    bool InitCompute(ID3D12Device* dev, ID3D12Resource* texInColorEngine,
                     ID3D12Resource* texOutEngine)
    {
        // encode root sig: [0] 9 consts, [1] SRV table (t0,t1=MV R84),
        //                  [2] UAV table (u0,u1,u2=flow R84)
        {
            D3D12_DESCRIPTOR_RANGE srv[2] = {
                { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
                  D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
                { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0,
                  D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },   // t1 = MV (R84)
            };
            D3D12_DESCRIPTOR_RANGE uav[3] = {
                { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0,
                  D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
                { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0,
                  D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
                { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2, 0,
                  D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },   // u2 = flow (R84)
            };
            D3D12_ROOT_PARAMETER rp[3] = {};
            rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rp[0].Constants.Num32BitValues = 11;  // w,h,tx,ty,expo,srcW,srcH,
            // mvW,mvH + jx,jy — R92: structural de-jitter (no flag; 0-j
            // frames take the plain Load path). MUST cover the whole
            // cbuffer (R82e trap: constants past this count read as
            // zero/garbage; the 3b flash bug was EXACTLY this).
            rp[0].Constants.ShaderRegister = 0;
            rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[1].DescriptorTable.NumDescriptorRanges = 2;  // t0,t1 — the
            // count MUST match the array (the decode-side short-count trap)
            rp[1].DescriptorTable.pDescriptorRanges = srv;
            rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[2].DescriptorTable.NumDescriptorRanges = 3;  // u0,u1,u2
            rp[2].DescriptorTable.pDescriptorRanges = uav;
            D3D12_ROOT_SIGNATURE_DESC rs {};
            rs.NumParameters = 3;
            rs.pParameters = rp;
            ComPtr<ID3DBlob> sig;
            if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
                                                   &sig, nullptr)) ||
                FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(),
                                                sig->GetBufferSize(),
                                                IID_PPV_ARGS(&rsEnc))))
            {
                sli::Log("vendor: enc root sig failed");
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC ps {};
            ps.pRootSignature = rsEnc.Get();
            ps.CS.pShaderBytecode = g_nrEncodeCso;
            ps.CS.BytecodeLength = sizeof(g_nrEncodeCso);
            if (FAILED(dev->CreateComputePipelineState(&ps, IID_PPV_ARGS(&psoEnc))))
            {
                sli::Log("vendor: enc pso failed");
                return false;
            }
        }
        // delta-decode root sig: [0] 12 consts, [1] SRVs t0+t1 (model output,
        // ORIGINAL hdr), [2] UAV (u0,u1)
        {
            D3D12_DESCRIPTOR_RANGE srv[2] {};
            srv[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
                       D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
            srv[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0,
                       D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
            D3D12_DESCRIPTOR_RANGE uav[2] = {
                { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0,
                  D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },  // u0 = heap slot 5
                { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, 1 }, // u1 = +1 = slot 6
            };
            // TRAMPA R82e: el OffsetInDescriptorsFromTableStart es RELATIVO
            // A LA TABLA (que arranca en el slot 5 del heap), NO al heap:
            // el 6 explicito apuntaba al slot 11, fuera del heap de 8 — los
            // writes a gGainTiles se peredian en silencio, el readback daba
            // ceros y la normalizacion por mediana fue un NO-OP desde R82c
            // (la "convergencia 0.94" era el gain crudo). El fail-soft de
            // D3D12 no valida: solo el log de gainnorm lo delato.
            D3D12_ROOT_PARAMETER rp[3] = {};
            rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rp[0].Constants.Num32BitValues = 18;  // w,h,expo,mw,mh,boost,tint,
            // probe, nR,nG,nB (float3 aligned), detail,colour,hiGuard,
            // residual, jX,jY (R92b proxy-domain de-jitter). MUST cover
            // the whole cbuffer (constants past this count read as zero;
            // the R82e/3b trap). Declared == pushed.
            rp[0].Constants.ShaderRegister = 0;
            rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[1].DescriptorTable.NumDescriptorRanges = 2;
            rp[1].DescriptorTable.pDescriptorRanges = srv;
            rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[2].DescriptorTable.NumDescriptorRanges = 2;  // u0,u1 — the
            // count MUST match the array size: a short count lies to the
            // runtime, the decode PSO fails and NR silently degrades to
            // zero-delta (R82d live finding).
            rp[2].DescriptorTable.pDescriptorRanges = uav;
            D3D12_ROOT_SIGNATURE_DESC rs {};
            rs.NumParameters = 3;
            rs.pParameters = rp;
            ComPtr<ID3DBlob> sig;
            if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
                                                   &sig, nullptr)) ||
                FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(),
                                                sig->GetBufferSize(),
                                                IID_PPV_ARGS(&rsDec))))
            {
                sli::Log("vendor: dec root sig failed");
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC ps {};
            ps.pRootSignature = rsDec.Get();
            ps.CS.pShaderBytecode = g_nrDeltaCso;
            ps.CS.BytecodeLength = sizeof(g_nrDeltaCso);
            if (FAILED(dev->CreateComputePipelineState(&ps, IID_PPV_ARGS(&psoDelta))))
                return false;
        }
        // stab-depth root sig (R92): [0] 4 consts (w,h,jx,jy),
        // [1] SRV t0 = depth raw, [2] UAV u0 = stab out
        {
            D3D12_DESCRIPTOR_RANGE srv1[1] = {
                { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
                  D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
            };
            D3D12_DESCRIPTOR_RANGE uav1[1] = {
                { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0,
                  D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
            };
            D3D12_ROOT_PARAMETER rp[3] = {};
            rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rp[0].Constants.Num32BitValues = 4;  // w,h,jx,jy
            rp[0].Constants.ShaderRegister = 0;
            rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[1].DescriptorTable.NumDescriptorRanges = 1;
            rp[1].DescriptorTable.pDescriptorRanges = srv1;
            rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[2].DescriptorTable.NumDescriptorRanges = 1;
            rp[2].DescriptorTable.pDescriptorRanges = uav1;
            D3D12_ROOT_SIGNATURE_DESC rs {};
            rs.NumParameters = 3;
            rs.pParameters = rp;
            ComPtr<ID3DBlob> sig;
            if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
                                                   &sig, nullptr)) ||
                FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(),
                                                sig->GetBufferSize(),
                                                IID_PPV_ARGS(&rsStab))))
            {
                sli::Log("vendor: stab root sig failed");
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC ps {};
            ps.pRootSignature = rsStab.Get();
            ps.CS.pShaderBytecode = g_nrStabDepthCso;
            ps.CS.BytecodeLength = sizeof(g_nrStabDepthCso);
            if (FAILED(dev->CreateComputePipelineState(&ps, IID_PPV_ARGS(&psoStab))))
            {
                sli::Log("vendor: stab pso failed");
                return false;
            }
        }
        // tile grid + readback. The encode dispatch runs at WORK dims and
        // writes gTiles[gid.y*gTilesX + gid.x] for gid < work grid, so the
        // buffer must be sized to WORK — a render-sized grid leaves the tail
        // unwritten (DEFAULT heap, never zeroed) and the CPU median then
        // reads garbage VRAM whenever nrWorkScale < 1 (audit 2026-09-29).
        tilesX = (workW + 7) / 8;
        tilesY = (workH + 7) / 8;
        bufTiles = make_buf(dev, (UINT64)tilesX * tilesY * 4,
                            D3D12_HEAP_TYPE_DEFAULT,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        rbTiles = make_buf(dev, (UINT64)tilesX * tilesY * 4,
                           D3D12_HEAP_TYPE_READBACK,
                           D3D12_RESOURCE_STATE_COPY_DEST);
        // R82c/R82e: gain-tile grid (uint2 per tile = 8 B, packed f16 RGB).
        // R95 audit BLOCKER fix: the DECODE writes these tiles from its
        // RENDER-dim dispatch (w,h = render dims), so the buffer MUST be
        // sized to the RENDER grid — the old work-grid sizing wrote ~4x
        // past the end at ws=0.5 (OOB UAV writes onto adjacent DEFAULT-
        // heap allocations) and misaligned every CPU median readback.
        gtX = (w + 7) / 8;
        gtY = (h + 7) / 8;
        bufGainTiles = make_buf(dev, (UINT64)gtX * gtY * 8,
                                D3D12_HEAP_TYPE_DEFAULT,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        rbGainTiles = make_buf(dev, (UINT64)gtX * gtY * 8,
                               D3D12_HEAP_TYPE_READBACK,
                               D3D12_RESOURCE_STATE_COPY_DEST);
        // R84: per-tile flow peaks (RENDER grid, same as the gain tiles —
        // the DECODE writes both from its render-dim dispatch) — read
        // back with the SAME copy the gain tiles already need (zero
        // extra commands: one CopyBufferRegion more inside the same
        // group).
        bufFlow = make_buf(dev, (UINT64)gtX * gtY * 8,
                           D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        rbFlow = make_buf(dev, (UINT64)gtX * gtY * 8,
                          D3D12_HEAP_TYPE_READBACK,
                          D3D12_RESOURCE_STATE_COPY_DEST);
        {
            D3D12_RANGE all {};
            void* p = nullptr;
            if (SUCCEEDED(rbGainTiles->Map(0, &all, &p)))
                rbGainTilesPtr = (uint32_t*) p;   // persists; never unmapped
        }
        {
            D3D12_RANGE allf {};
            void* pf = nullptr;
            if (SUCCEEDED(rbFlow->Map(0, &allf, &pf)))
                rbFlowPtr = (uint32_t*) pf;       // persists; never unmapped
        }
        if (!bufTiles || !rbTiles)
        {
            sli::Log("vendor: tiles buffers failed");
            return false;
        }
        D3D12_RANGE rd {};
        if (FAILED(rbTiles->Map(0, &rd, (void**)&rbTilesPtr)))
            rbTilesPtr = nullptr;
        tileLums.resize((size_t)tilesX * tilesY);

        // descriptor heap + views
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 12;  // R84 layout: encode t=[0,1] u=[2,3,4],
                                 // decode t=[5,6] u=[7,8,9]; R92 stab:
                                 // t=10 (depth SRV) u=11 (stab UAV)
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heapCodec))))
        {
            sli::Log("vendor: codec heap failed");
            return false;
        }
        heapInc = dev->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto slot = [&](UINT i) {
            D3D12_CPU_DESCRIPTOR_HANDLE hh =
                heapCodec->GetCPUDescriptorHandleForHeapStart();
            hh.ptr += (SIZE_T)i * heapInc;
            return hh;
        };
        D3D12_SHADER_RESOURCE_VIEW_DESC sv {};
        sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;   // 0 = invalid desc -> deferred device removal
        dev->CreateShaderResourceView(texInColorEngine, &sv, slot(0));
        if (FAILED(dev->GetDeviceRemovedReason()))
        {
            sli::Log("vendor: device removed after SRV in");
            return false;
        }
        // BGRA8 UAV support via CheckFeatureSupport
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs {};
        fs.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                               &fs, sizeof(fs))))
            sli::Log("vendor: BGRA8 UAV support=%d", (int) fs.Support1);
        // R84 FINAL HEAP LAYOUT (descriptor tables bind CONTIGUOUS slots —
        // the R82e offset-is-table-relative trap; every table below is a
        // contiguous run):
        //   encode t=[0..1]  : 0=SRV texInColor(16f), 1=SRV texGuideMV(raw)
        //   encode u=[2..4]  : 2=UAV texColor(BGRA8), 3=UAV bufTiles(4B),
        //                      4=UAV bufFlow(8B, per-tile peak |MV|)
        //   decode t=[5..6]  : 5=SRV texOutModel(RGBA8 view over BGRA8),
        //                      6=SRV texInColor(16f)
        //   decode u=[7..9]  : 7=UAV texOutEngine(16f), 8=UAV bufGainTiles(8B),
        //                      9=UAV bufFlow — the decode shader declares no
        //                      u2; the extra range is legal (root sig may
        //                      cover more than the shader references) and
        //                      keeps ONE uav-table shape for both passes.
        // The MV SRV is a VIEW on the game's texture: no transition, no copy
        // (fork lesson 7.5: never transition someone else's resource); it is
        // read in COMMON. Format = the RAW family the arm reported.
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC svm {};
            svm.Format = (DXGI_FORMAT) rawMvFmt;
            svm.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            svm.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            svm.Texture2D.MipLevels = 1;
            dev->CreateShaderResourceView(texGuideMV, &svm, slot(1));
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv8 {};
        uv8.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        uv8.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(texColor.Get(), nullptr, &uv8, slot(2));
        if (FAILED(dev->GetDeviceRemovedReason()))
        {
            sli::Log("vendor: device removed after encode views");
            return false;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uvb {};
        uvb.Format = DXGI_FORMAT_UNKNOWN;   // RWStructuredBuffer: format comes
                                            // from the shader — R32_UINT here
                                            // is an invalid view and the driver
                                            // can remove the device deferred
        uvb.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uvb.Buffer.NumElements = tilesX * tilesY;
        uvb.Buffer.StructureByteStride = 4; // no stride = invalid too
        dev->CreateUnorderedAccessView(bufTiles.Get(), nullptr, &uvb, slot(3));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uvf = uvb;
        uvf.Buffer.StructureByteStride = 8; // uint2 per tile (f16 x2)
        dev->CreateUnorderedAccessView(bufFlow.Get(), nullptr, &uvf, slot(4));
        D3D12_SHADER_RESOURCE_VIEW_DESC sv8 = sv;
        // R8G8B8A8 over the BGRA8 resource = channel-swap view. The R82f
        // A/B settled the runtime writes RGBA8 component ORDER into its
        // 8-bit output (SRV as BGRA8 showed magenta person / green tents
        // = swapped); reading it as RGBA8 restores the true colors while
        // the resource stays in the swapchain family the model accepts.
        sv8.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dev->CreateShaderResourceView(texOutModel.Get(), &sv8, slot(5));
        dev->CreateShaderResourceView(texInColorEngine, &sv, slot(6));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv16 {};
        uv16.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;  // u0 = texOutEngine:
        // the DECODE's own output (gain/view payload), render dims — this
        // is NOT the model output; texOutModel is read via slot 5 (RGBA8)
        uv16.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(texOutEngine, nullptr, &uv16, slot(7));
        // slot 8: gGainTiles (R82c) — uint2 per tile (packed f16 RGB),
        // RENDER grid (the decode's dispatch dims — R95 BLOCKER fix)
        D3D12_UNORDERED_ACCESS_VIEW_DESC uvg = uvf;
        uvg.Buffer.NumElements = gtX * gtY;
        dev->CreateUnorderedAccessView(bufGainTiles.Get(), nullptr, &uvg,
                                       slot(8));
        // slot 9: the same flow buffer, riding the decode's uav table
        dev->CreateUnorderedAccessView(bufFlow.Get(), nullptr, &uvf, slot(9));
        const HRESULT drr2 = dev->GetDeviceRemovedReason();
        if (FAILED(drr2))
            sli::Log("vendor: device removed AFTER views (0x%08X)", (unsigned) drr2);
        return SUCCEEDED(drr2);
    }

public:
    // begin = reset the cmd to recording; flush = close+submit+wait.
    // texInColor/texOut = the engine's TRANSPORT textures: the GPU codec
    // uses them directly as input/output (zero CPU copies).
    // tuningIn: live ABI block — nrBoost/nrTint are read per ProcessFrame.
    bool Init(ID3D12Device* dev, ID3D12GraphicsCommandList* cmd,
              const std::wstring& gameDir, unsigned width, unsigned height,
              ID3D12Resource* texInColorEngine, ID3D12Resource* texOutEngine,
              ID3D12Resource* texGuideDepthIn, ID3D12Resource* texGuideMvIn,
              unsigned guideWIn, unsigned guideHIn, unsigned depthInvIn,
              unsigned guideDepthFmtIn, unsigned guideMvFmtIn,
              float kIntensityIn, float kLocalStructureIn, float kLocalToneIn,
              float kSkinStructureIn, unsigned kStyleIn, float workScaleIn,
              const volatile sli::Tuning* tuningIn,
              const std::function<bool()>& begin,
              const std::function<bool(const char*)>& flush)
    {
        w = width; h = height;
        tuning = tuningIn;
        // clamp to the fork range [0.25, 2.0]
        workScale = workScaleIn < 0.25f ? 0.25f
                  : (workScaleIn > 2.0f ? 2.0f : workScaleIn);
        workW = (unsigned) ((float) w * workScale + 0.5f);
        workH = (unsigned) ((float) h * workScale + 0.5f);
        if (workW < 64)  workW = 64;
        if (workH < 64)  workH = 64;
        if (workW > w)   workW = w;    // guides are full-res: no supersample
        if (workH > h)   workH = h;
        kIntensity = kIntensityIn; kLocalStructure = kLocalStructureIn;
        kLocalTone = kLocalToneIn; kSkinStructure = kSkinStructureIn;
        kStyle = kStyleIn;
        texGuideDepth = texGuideDepthIn;
        texGuideMV = texGuideMvIn;
        guideW = guideWIn; guideH = guideHIn;
        depthInverted = depthInvIn;
        rawDepthFmt = guideDepthFmtIn;   // raw (bits as-is)
        rawMvFmt = guideMvFmtIn;

        // (1) driver core (registry) + Init_Ext + capability block
        wchar_t cpath[MAX_PATH];
        if (!core_path(cpath))
        {
            sli::Log("vendor: NGXCore registry path missing");
            return false;
        }
        std::wstring coreDll = std::wstring(cpath) + L"\\_nvngx.dll";
        core = LoadLibraryExW(coreDll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!core)
        {
            sli::Log("vendor: core load failed (%ls)", coreDll.c_str());
            return false;
        }
        auto initExt = (PFN_InitExt)GetProcAddress(core, "NVSDK_NGX_D3D12_Init_Ext");
        auto getCap = (PFN_GetCap)GetProcAddress(core,
                                                 "NVSDK_NGX_D3D12_GetCapabilityParameters");
        if (!initExt || !getCap)
        {
            sli::Log("vendor: core exports missing");
            return false;
        }
        // the real game's app-id (the one every winning stub A/B used);
        // the forwarder uses its own (0x24480451) for the snippet.
        volatile int rc = initExt(101616311ull, gameDir.c_str(), dev, 0x15, nullptr);
        if (rc != 1)
        {
            sli::Log("vendor: core Init_Ext -> %d", (int)rc);
            return false;
        }
        if (getCap(&P) != 1 || !P)
        {
            sli::Log("vendor: capability block failed");
            return false;
        }
        fslot = discover_float_slot(P);
        sli::Log("vendor: core init ok, capability block, float slot %d", fslot);


        // Core Init_Ext can leave the device REMOVED without failing
        // explicitly (zombie-engine lesson) — check before creating
        // anything else, or everything fails in cascade and the destructor
        // crashes.
        const HRESULT drr = dev->GetDeviceRemovedReason();
        if (FAILED(drr))
        {
            sli::Log("vendor: device removed after core init (0x%08X) - "
                     "engine restart needed", (unsigned) drr);
            return false;
        }

        // (2) model textures (formats validated in the stub)
        sli::Log("vendor: creating model textures...");
        sli::Log("vendor: work %ux%u (scale %.2f, render %ux%u)",
                 workW, workH, workScale, w, h);
        texColor = make_tex(dev, DXGI_FORMAT_B8G8R8A8_UNORM, workW, workH,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // R82f: the model's output texture carries the SAME format as its
        // color input (fork pattern: g_nr.output = CreateScratch(desc.Format)
        // of the swapchain target). RGBA16F made the runtime write a
        // packed non-display pattern (R/B ramp with G exactly 0 — the
        // R82e probe "synthetic" finding); with input/output in the same
        // 8-bit family the model writes a real display picture (the fork's
        // captures prove the path). The decode samples it as SRV.
        texOutModel = make_tex(dev, DXGI_FORMAT_R8G8B8A8_TYPELESS, workW, workH,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!texColor || !texOutModel)
        {
            sli::Log("vendor: texture creation failed");
            return false;
        }
        // no real guides, no model (synthetic guides deleted for good)
        if (!texGuideDepth || !texGuideMV)
        {
            sli::Log("vendor: no real guides - not arming (zero-delta fallback)");
            return false;
        }

        // (3) GPU codec (pipelines + tiles + views over the engine textures)
        sli::Log("vendor: init compute...");
        if (!InitCompute(dev, texInColorEngine, texOutEngine))
            return false;
        sli::Log("vendor: compute ready");

        // (4) real guides — leave them in SRV (the per-frame copy works
        //     SRV<->COPY_DEST; the evaluate consumes SRV).
        if (!begin()) return false;
        barrier(cmd, texGuideDepth, D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(cmd, texGuideMV, D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!flush("vendor-guides-init"))
            return false;

        // (5) tuning on the block BEFORE create (the model reads it ONCE;
        //     evaluate-only values are ignored — forwarder L870-879)
        set_uint(P, "CreationNodeMask", 1);
        set_uint(P, "VisibilityNodeMask", 1);
        set_uint(P, "DLSSNR.Enabled", 1);
        set_uint(P, "DLSSNR.Width", w);
        set_uint(P, "DLSSNR.Height", h);
        // PerfQualityValue is NVSDK_NGX_PerfQuality_Value = a FLOAT in NGX.
        // It was pushed through SetUInt — the runtime's float reader never
        // saw it, fell back to its default (MaxPerf), and the model answered
        // at ~43% of the work height (the R82g "answer occupies the top
        // 43%" measurement). Send it through the DISCOVERED float slot;
        // NVSDK_NGX_PerfQuality_Value: 0=MaxPerf .. 4=MaxQuality+ (DLAA-ish).
        // R90 (#1): the value is the nrPerfQuality tuning knob (2 =
        // MaxQuality, what always ran; the panel exposes 0..4 with the
        // quality trade spelled out). NOTE: the ratio valve is settled
        // LATER (post-probe, pre-create) — see the golpe 1b block.
        const float pqv = (tuning && tuning->nrPerfQuality >= 0.0f &&
                           tuning->nrPerfQuality <= 4.0f)
                              ? tuning->nrPerfQuality : 2.0f;
        pqvAtCreate = pqv;
        set_flt(P, fslot, "PerfQualityValue", pqv);
        set_uint(P, "PerfQualityValue", (unsigned) pqv); // uint shadow for
                                                         // uint readers
        set_uint(P, "DLSSNR.Hint.Render.Preset", 0);
        // golpe 1b: the ratio is the model's own price dial. 1.0 pinned
        // (the R82g-era fix) neutralized PerfQualityValue entirely — the
        // fork's create does NOT write it and lets the model compute it
        // from PQV. nrRatioPin>0 keeps the classic pin (rollback/debug);
        // 0.0 leaves the valve to the model.
        if (tuning && tuning->nrRatioPin > 0.0f)
            set_flt(P, fslot, "DLSSNR.ScalingRatio", tuning->nrRatioPin);
        set_uint(P, "DLSSNR.UICorrection", 1);
        set_flt(P, fslot, "DLSSNR.Intensity", 1.0f);
        set_uint(P, "DLSSNR.Style", 0);
        set_flt(P, fslot, "DLSSNR.LocalStructureStrength", 1.0f);
        set_flt(P, fslot, "DLSSNR.LocalToneStrength", 1.0f);
        set_flt(P, fslot, "DLSSNR.SkinStructureStrength", -1.0f);
        set_res(P, "DLSSNR.ControlMask", nullptr);
        set_uint(P, "DLSSNR.UseAutoMask", 1);
        set_res(P, "DLSSNR.UI", nullptr);
        set_res(P, "DLSSNR.UIAlpha", nullptr);
        set_res(P, "DLSSNR.Backbuffer", nullptr);
        set_uint(P, "DLSSNR.Reset", 1);

        // R82f self-check: what did the writes ACTUALLY store? Read the
        // tuning keys through the three typed getters (rc in hex) plus
        // the discover plant — the rc pattern tells the stored variant
        // type (missing / type-mismatch / ok).
        {
            void** vt = *(void***) P;
            using PFN_GetF = int (__thiscall*)(void*, const char*, float*);
            using PFN_GetUI = int (__thiscall*)(void*, const char*, unsigned*);
            using PFN_GetULL = int (__thiscall*)(void*, const char*,
                                                 unsigned long long*);
            struct KV { const char* k; float f; unsigned u;
                        unsigned long long ll; };
            KV rows[5] = {
                { "PerfQualityValue", -1, 0, 0 },
                { "DLSSNR.Intensity", -1, 0, 0 },
                { "DLSSNR.MVecScaleX", -1, 0, 0 },
                { "DLSSNR.LocalToneStrength", -1, 0, 0 },
                { "DLSSNR.FloatSlotProbe6", -1, 0, 0 },
            };
            for (auto& r : rows)
            {
                const int rf = ((PFN_GetF)vt[9])(P, r.k, &r.f);
                const int ru = ((PFN_GetUI)vt[12])(P, r.k, &r.u);
                const int rl = ((PFN_GetULL)vt[8])(P, r.k, &r.ll);
                sli::Log("vendor: block [%s] f=%.4f(rc %08X) ui=%u(%08X) "
                         "ll=%llu(%08X)",
                         r.k, r.f, (unsigned) rf & 0xFFFFFFFFu, r.u,
                         (unsigned) ru & 0xFFFFFFFFu, r.ll,
                         (unsigned) rl & 0xFFFFFFFFu);
            }
        }

        // (6) forwarder + create(18)
        if (!begin()) return false;
        std::wstring fwdDll = gameDir + L"\\nvngx.dll_dlssnr.dll";
        fwd = LoadLibraryExW(fwdDll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!fwd)
        {
            sli::Log("vendor: forwarder load failed (%ls)", fwdDll.c_str());
            return false;
        }
        auto fCreate = (PFN_FwdCreate)GetProcAddress(fwd, "dlssnr_call_create");
        fEval = (PFN_FwdEval)GetProcAddress(fwd, "dlssnr_call_evaluate_v2");
        fRel = (PFN_FwdRelease)GetProcAddress(fwd, "dlssnr_call_release");
        if (!fCreate || !fEval || !fRel)
        {
            sli::Log("vendor: forwarder exports missing");
            return false;
        }
        // the forwarder writes floats through its OWN slot (default 1);
        // tell it the real one (fork L717 does the same)
        if (fslot >= 0)
        {
            using PFN_SetSlot = void(__cdecl*)(int);
            if (auto setSlot = (PFN_SetSlot)GetProcAddress(
                    fwd, "dlssnr_call_set_float_slot"))
                setSlot(fslot);
        }
        std::wstring snip = gameDir + L"\\nvngx_dlssnr.dll";
        fCreatePtr = (void*) fCreate;
        devPtr = dev; cmdPtr = cmd; gameDirW = gameDir;

        // R90 golpe 1b (paso 1): the model's OWN scaling ratios, asked the
        // way NVIDIA asks (fork DlssNr_Dx12.cpp:1100). Read-only: no feature
        // exists yet. rc 1 = ratio written; 0 = callback not published;
        // -1 = published but refused that quality level.
        if (fQueryRatio == nullptr)
            fQueryRatio = (PFN_FwdQueryRatio) GetProcAddress(
                fwd, "dlssnr_query_scaling_ratio");
        if (fQueryRatio != nullptr)
        {
            static const char* qn[6] = { "MaxPerf", "Balanced", "MaxQuality",
                                         "UltraPerf", "UltraQual", "DLAA" };
            char line[512] = {};
            size_t used = 0;
            bool any = false;
            for (unsigned q = 0; q < 6; ++q)
            {
                float ratio = -1.0f;
                const int qrc = fQueryRatio(snip.c_str(), P, q, &ratio);
                int written = 0;
                if (qrc == 1)
                {
                    any = true;
                    written = _snprintf_s(line + used, sizeof(line) - used,
                                          _TRUNCATE, "%s=%.4f ", qn[q], ratio);
                }
                else if (qrc == -1)
                {
                    any = true;
                    written = _snprintf_s(line + used, sizeof(line) - used,
                                          _TRUNCATE, "%s=refused ", qn[q]);
                }
                if (written > 0) used += (size_t) written;
            }
            if (any)
                sli::Log("vendor: model scaling ratios: %s", line);
            else
                sli::Log("vendor: ratio callback not published (stage see "
                         "dlssnr_last_ratio_stage)");
        }
        else
            sli::Log("vendor: forwarder lacks query_scaling_ratio (old?)");

        // golpe 1b: the probe above left PQV/ratio at DLAA's values —
        // settle OUR pqv and the ratio valve NOW, right before create.
        set_flt(P, fslot, "PerfQualityValue", pqv);
        set_uint(P, "PerfQualityValue", (unsigned) pqv);
        if (tuning && tuning->nrRatioPin > 0.0f)
        {
            set_flt(P, fslot, "DLSSNR.ScalingRatio", tuning->nrRatioPin);
            activeRatio = tuning->nrRatioPin;
        }
        else if (fQueryRatio != nullptr)
        {
            float r = -1.0f;
            const int vrc = fQueryRatio(snip.c_str(), P,
                                        (unsigned) (pqv + 0.5f), &r);
            if (vrc == 1 && r > 0.0f)
            {
                activeRatio = r;  // callback wrote DLSSNR.ScalingRatio+PQV
            }
            else
            {
                set_flt(P, fslot, "DLSSNR.ScalingRatio", 1.0f);
                activeRatio = 1.0f;
                sli::Log("vendor: ratio valve not reachable (rc %d) - "
                         "pinned 1.0", vrc);
            }
        }
        sli::Log("vendor: create with pqv %.1f, ratio %.4f%s",
                 (double) pqv, (double) activeRatio,
                 (tuning && tuning->nrRatioPin > 0.0f) ? " (pinned)" : "");

        feature = fCreate(snip.c_str(), gameDir.c_str(), dev, cmd, P, w, h,
                          0 /*preset*/, kIntensity, kStyle, kLocalStructure,
                          kLocalTone, kSkinStructure,
                          1 /*autoMask*/, 1 /*uiCorrection*/);
        if (!flush("vendor-create") || !feature)
        {
            sli::Log("vendor: create(18) failed");
            return false;
        }
        // golpe 1b: what ratio did the model actually take? (float getter,
        // slot 9; missing key = rc BAD00010 — the model never wrote one)
        {
            float got = -1.0f;
            void** vt = *(void***) P;
            const int prc = ((PFN_GetFloat)vt[9])(P, "DLSSNR.ScalingRatio", &got);
            sli::Log("vendor: post-create ratio %.4f (rc %08X, pqv %.1f)",
                     (double) got, (unsigned) prc & 0xFFFFFFFFu,
                     (double) pqvAtCreate);
        }
        sli::Log("vendor: feature 18 created (%ux%u) - vendor NR backend live",
                 w, h);
        ok = true;
        return true;
    }

    // Live tuning helpers — read straight from the ABI mapping every call
    // (volatile read, no caching: tint toggles take effect next frame).
    float tuningBoost() const
    {
        return tuning ? tuning->nrBoost : 1.0f;
    }
    // R82k compose strengths (live, no re-arm):
    float tuningDetail() const
    {
        float d = tuning ? tuning->nrDetail : 1.0f;
        if (d < 0.0f) d = 0.0f;
        if (d > 4.0f) d = 4.0f;
        return d;
    }
    float tuningColour() const
    {
        float c = tuning ? tuning->nrColour : 0.25f;
        if (c < 0.0f) c = 0.0f;
        if (c > 2.0f) c = 2.0f;
        return c;
    }
    float tuningGainBound() const
    {
        float g = tuning ? tuning->nrGainBound : 2.0f;
        if (g < 1.0f) g = 1.0f;
        if (g > 8.0f) g = 8.0f;
        return g;
    }
    // golpe 4: matched-residual compose (live, no re-arm). Default ON —
    // only fires in the shader when the model actually ran small.
    float tuningResidual() const
    {
        float r = tuning ? tuning->nrResidual : 1.0f;
        if (r < 0.0f) r = 0.0f;
        if (r > 1.0f) r = 1.0f;
        return r;
    }
    unsigned tuningAutoSkin() const
    {
        return tuning ? (tuning->nrAutoSkin ? 1u : 0u) : 1u;
    }
    unsigned tuningTint() const
    {
        // R82d: the tint FLAG is binary — 0 = quality (transported gain),
        // 1 = VIEW (the screen shows the model's pure output). Anything
        // non-zero is a view: the host picks the OPAQUE PSO for tint!=0,
        // so an engine-side collapse to 0 would compose the gain
        // additively = white wash (R82d live finding).
        return (tuning && tuning->nrTint != 0) ? 1u : 0u;
    }

    // ---- full frame in ONE GPU session ----
    // Pre: texInColor (engine) in COMMON with sealed frame N; the caller
    // already did begin(). Records copy-guides -> encode -> evaluate ->
    // decode-delta -> tiles copy; the caller flushes. On return: texOut
    // (engine) in UAV with the display-domain delta; texInColor back to
    // COMMON. Guide bufs come from the caller (ring index (frame-1)%3).
    bool ProcessFrame(ID3D12GraphicsCommandList* cmd, bool forceReset,
                      ID3D12Resource* texInColorEngine,
                      ID3D12Resource* bufGuideDepthN,
                      ID3D12Resource* bufGuideMvN,
                      unsigned guideDepthPitchN, unsigned guideMvPitchN,
                      float mvScaleX, float mvScaleY,
                      float jitterX, float jitterY)
    {
        if (!ok || !feature || !fEval) return false;

        // (0) reload real guides (frame N-1) from the shared triple buffer
        //     into the local textures. COMMON/SRV -> COPY_DEST -> back (the
        //     evaluate consumes SRV). The footprint carries the game's RAW
        //     format (R32G32_SINT bits identical over TYPELESS = a valid
        //     copy; the runtime doesn't accept the SINT form in dispatch —
        //     the host delivers R32_FLOAT-converted content).
        {
            auto copyGuide = [&](ID3D12Resource* buf, unsigned pitch,
                                 unsigned fmtRaw, ID3D12Resource* tex) {
                barrier(cmd, tex,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION src { buf,
                    D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                src.PlacedFootprint.Footprint.Format = (DXGI_FORMAT) fmtRaw;
                src.PlacedFootprint.Footprint.Width = guideW;
                src.PlacedFootprint.Footprint.Height = guideH;
                src.PlacedFootprint.Footprint.Depth = 1;
                src.PlacedFootprint.Footprint.RowPitch = pitch;
                D3D12_TEXTURE_COPY_LOCATION dst { tex,
                    D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                barrier(cmd, tex, D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            };
            copyGuide(bufGuideDepthN, guideDepthPitchN, rawDepthFmt, texGuideDepth);
            copyGuide(bufGuideMvN, guideMvPitchN, rawMvFmt, texGuideMV);
        }
        if (seq < 3) sli::Log("vendor [%u]: guides copied", seq);

        // R92 STRUCTURAL DE-JITTER: whenever the frame carries jitter
        // (pre-SR), the DEPTH guide gets the SAME -j shift the colour seal
        // gets — the colour/depth PAIR stays aligned to first order (both
        // planes share the parallax error; it cancels between them). The
        // MV guide is untouched (NGX MV contract excludes jitter). At
        // jitter==0 (POST-SR) nothing runs: no dispatch, no transitions,
        // bit-identical to before.
        const bool jittered = (jitterX != 0.0f) || (jitterY != 0.0f);
        ID3D12Resource* depthForModel = texGuideDepth;
        if (jittered)
        {
            if (!texGuideDepthStab)
            {
                texGuideDepthStab = make_tex(devPtr, DXGI_FORMAT_R32_FLOAT,
                                             guideW, guideH,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (texGuideDepthStab &&
                    SUCCEEDED(devPtr->GetDeviceRemovedReason()))
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC svd {};
                    svd.Format = DXGI_FORMAT_R32_FLOAT;
                    svd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    svd.Shader4ComponentMapping =
                        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    svd.Texture2D.MipLevels = 1;
                    devPtr->CreateShaderResourceView(texGuideDepth, &svd,
                                                    SlotCPU(10));
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uvd {};
                    uvd.Format = DXGI_FORMAT_R32_FLOAT;
                    uvd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    devPtr->CreateUnorderedAccessView(
                        texGuideDepthStab.Get(), nullptr, &uvd, SlotCPU(11));
                    sli::Log("vendor: depth-stab armed (%ux%u, R92)",
                             guideW, guideH);
                }
                else
                    sli::Log("vendor: depth-stab alloc FAILED - raw depth");
            }
            if (texGuideDepthStab)
            {
                // texGuideDepth is SRV now (copyGuide left it there) — the
                // stab reads it via slot 10 and writes the stab UAV (slot
                // 11). Then the evaluate consumes the STAB as its depth.
                ID3D12DescriptorHeap* hh[] = { heapCodec.Get() };
                cmd->SetDescriptorHeaps(_countof(hh), hh);
                cmd->SetPipelineState(psoStab.Get());
                cmd->SetComputeRootSignature(rsStab.Get());
                struct StabCb { unsigned w, h; float jx, jy; };
                const StabCb scb = { guideW, guideH, jitterX, jitterY };
                cmd->SetComputeRoot32BitConstants(0, 4, &scb, 0);
                cmd->SetComputeRootDescriptorTable(1, GPUHandle(10));
                cmd->SetComputeRootDescriptorTable(2, GPUHandle(11));
                cmd->Dispatch((guideW + 7) / 8, (guideH + 7) / 8, 1);
                uavBarrier(cmd, texGuideDepthStab.Get());
                depthForModel = texGuideDepthStab.Get();
            }
        }

        // (1) encode: texInColor(COMMON) -> SRV; writes texColor(BGRA8) + tiles
        barrier(cmd, texInColorEngine, D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ID3D12DescriptorHeap* heaps[] = { heapCodec.Get() };
        cmd->SetDescriptorHeaps(_countof(heaps), heaps);
        cmd->SetPipelineState(psoEnc.Get());
        cmd->SetComputeRootSignature(rsEnc.Get());
        struct EncCb
        {
            unsigned w, h, tx, ty;
            float expo;
            unsigned srcW, srcH;
            unsigned mvW, mvH;   // R84: MV texture dims (flow stats)
            float jx, jy;        // golpe 3b/R92: render-px jitter — the
                                 // de-jittered sample (-j) applies whenever
                                 // NON-ZERO (structural; 0 = identity)
        };
        const EncCb cb = { workW, workH, tilesX, tilesY,
                           whitePoint,   // R92f: the ENCODE's white point is
                                         // the adaptive meter, NOT the game's
                                         // held preExposure (RDR2 sends 1.0
                                         // constant — night reached the model
                                         // near-black; the meter lifts it)
                           w, h,
                           guideW, guideH,
                           jitterX, jitterY };
        // R92: 11 constants (dej flag removed — structural now). Declared
        // AND pushed must match (the 3b flash lesson).
        cmd->SetComputeRoot32BitConstants(0, 11, &cb, 0);
        cmd->SetComputeRootDescriptorTable(1, GPUHandle(0));  // t0=colour t1=MV
        cmd->SetComputeRootDescriptorTable(2, GPUHandle(2));  // u0..u2
        cmd->Dispatch((workW + 7) / 8, (workH + 7) / 8, 1);
        uavBarrier(cmd, texColor.Get());

        // (2) model evaluate (the runtime records its own commands)
        const bool first = seq == 0;
        const int reset = (first || forceReset) ? 1 : 0;
        if (seq < 3) sli::Log("vendor [%u]: encode done, calling evaluate "
                              "(mv %f,%f depthInv %u gw %ux%u)",
                              seq, mvScaleX, mvScaleY, depthInverted,
                              guideW, guideH);
        // Real game guides — own guide dims, subrect origin, real
        // depthInverted, this frame's mvScale (the game's vectors are
        // scaled to full-frame pixels: raw passthrough, fork pattern).
        // Color/out at WORK dims, guides full-res; MVs full-frame -> work
        // (fork mvToWork).
        volatile int rc = fEval(cmd, feature, P, texColor.Get(),
                                depthForModel, texGuideMV,
                                texOutModel.Get(),
                                workW, workH, guideW, guideH, guideW, guideH,
                                0, 0, 0, 0,
                                (int) depthInverted, reset,
                                kIntensity, (int) kStyle, kLocalStructure,
                                kLocalTone, kSkinStructure,
                                (int) tuningAutoSkin(),
                                // MV scale = the game's own value scaled
                                // by the dispatch/MV-texture ratio (fork
                                // DlssNr_Dx12.cpp:3622 guideMvScaleX *
                                // mvToWork, mvToWork = work/input; the
                                // core does NOT re-scale by subrect — its
                                // MVecScale is the DLSS-style converter).
                                // The MVs arrive in RENDER px (guideW),
                                // the dispatch is WORK (2534): one MV px
                                // is workW/guideW dispatch px. R82k.4
                                // briefly passed the raw game scale — that
                                // misread the fork (their "passed through"
                                // means don't INVENT the value, not don't
                                // scale it); the ahead-ghost it chased was
                                // the compose confound R82k.3 already
                                // fixed (full-strength verdict at boost=0).
                                mvScaleX * (float) workW / (float) guideW,
                                mvScaleY * (float) workH / (float) guideH,
                                // jitter: R92 — the planes reaching the
                                // model are ALWAYS stabilized (colour seal
                                // + depth stab apply -j whenever the frame
                                // carries jitter); declaring the game's
                                // jitter against a stabilized plane makes
                                // the reprojection double-correct. 0
                                // unconditionally.
                                0.0f, 0.0f);
        if (seq < 3) sli::Log("vendor [%u]: evaluate returned", seq);
        if (rc != 1)
        {
            sli::Log("vendor: evaluate #%u -> %d", seq, (int)rc);
            // restore texInColor to COMMON even if the evaluate failed:
            // the caller's zero-delta path assumes COMMON (state desync
            // otherwise)
            barrier(cmd, texInColorEngine,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COMMON);
            // the runtime may leave internal resources transitioned (the
            // 0xBAD00002 E_INVALIDARG close saga): making the list close
            // is the CALLER's job (FlushSeg), which has the catch-all.
            return false;
        }

        // (3) decode-delta: t0 = model display output (slot 3), t1 =
        // ORIGINAL HDR (slot 4), u0 = texOut (slot 5). Identity model =>
        // exact zero delta => composed base+0 = untouched game frame.
        barrier(cmd, texOutModel.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // the runtime's evaluate can have changed the cmd's heaps:
        // re-bind before the decode
        cmd->SetDescriptorHeaps(_countof(heaps), heaps);
        cmd->SetPipelineState(psoDelta.Get());
        cmd->SetComputeRootSignature(rsDec.Get());
        // delta dst = RENDER dims; the model output is WORK dims.
        // DecCb mirrors shaders/nr_delta.hlsl cbuffer layout: the float3
        // is 16-byte aligned in HLSL, so it sits at offset 32 (after a
        // pad) — 12 root constants (48 B).
        struct DecCb { unsigned w, h; float expo; unsigned mw, mh;
                       float modelTop, boost; unsigned tint; float probe;
                       float nR, nG, nB;
                       float detail, colour, hiGuard;
                       float residual;   // golpe 4
                       float jX, jY; };  // R92b: proxy-domain de-jitter
        static_assert(sizeof(DecCb) == 72, "DecCb must mirror nr_delta.hlsl (R92b: +jX/jY at tail)");
        // R82g.4 GEOMETRY OF THE ANSWER (in-game verified): the answer fills
        // the WHOLE work buffer; the decode maps it 1:1, no knob.
        const unsigned modelLiveW = workW;
        const unsigned modelLiveH = workH;
        const unsigned modelTop = 0u;
        // R92: no landing shift — the model's answer is already in the
        // canonical (un-jittered) domain because its INPUT was (the seal
        // and the depth stab put it there). The old -j sample would
        // double-correct.
        const DecCb cd = { w, h,
                           whitePoint,   // R92f: the SAME white point the
                                         // encode used — the proxies and the
                                         // model's answer must share the
                                         // domain or the ratio breaks. The
                                         // game's preExposure (expoScale)
                                         // never enters here: the ratio is
                                         // exposure-invariant and the game's
                                         // own tonemap runs after us.
                           modelLiveW, modelLiveH,
                           (float) modelTop,
                           tuningBoost(), tuningTint(),
                           0.0f,   // R89a: the R82e probe (dProbe=1 on frame
                                   // 3 of every arm/rebuild, skipping the
                                   // gain-tile store to stash forensics
                                   // sentinels) ran in PRODUCTION — the
                                   // tiles readback of that frame fed
                                   // UpdateGainNorm poisoned values. The
                                   // probe is done; 0 unconditional.
                           normR.x, normR.y, normR.z,
                           tuningDetail(), tuningColour(), tuningGainBound(),
                           tuningResidual(),   // golpe 4
                           // R92b: the decode's proxy/denominator samples
                           // at -j — same canonical domain the model's
                           // input lives in. 0 in POST-SR (identity).
                           jitterX, jitterY };
        // R92b: 18 constants (jX/jY back at the tail for the PROXY side —
        // the answer landing itself stays un-shifted). Declared AND
        // pushed must match (the 3b flash lesson).
        cmd->SetComputeRoot32BitConstants(0, 18, &cd, 0);
        cmd->SetComputeRootDescriptorTable(1, GPUHandle(5));  // t0(5) t1(6)
        cmd->SetComputeRootDescriptorTable(2, GPUHandle(7));  // u0(7) u1(8)
        cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

        // (4) tiles -> readback
        // R92f: UNGATED — this readback is the METER (UpdateExposure's
        // median of RAW luma). R91b gated it as "dead" because
        // exposureHeld latched, but held only means the RECONSTRUCTION
        // value is the game's; the meter must keep breathing or the
        // encode's white point freezes (the night near-black bug).
        uavBarrier(cmd, nullptr);
        barrier(cmd, bufTiles.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(rbTiles.Get(), 0, bufTiles.Get(), 0,
                              (UINT64)tilesX * tilesY * 4);
        barrier(cmd, bufTiles.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // R82c: gain tiles -> readback (next frame's normalisation; 8 B/tile)
        barrier(cmd, bufGainTiles.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(rbGainTiles.Get(), 0, bufGainTiles.Get(), 0,
                              (UINT64)gtX * gtY * 8);
        barrier(cmd, bufGainTiles.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // R84: flow peaks -> readback (NEXT frame's history-reset signal;
        // 8 B/tile, RENDER grid like the gain tiles)
        barrier(cmd, bufFlow.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(rbFlow.Get(), 0, bufFlow.Get(), 0,
                              (UINT64)gtX * gtY * 8);
        barrier(cmd, bufFlow.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // (5) texInColor back to COMMON (next frame's seal)
        barrier(cmd, texInColorEngine,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COMMON);
        ++seq;
        return true;
    }

    // R84 HISTORY-RESET SIGNAL: the peak per-tile |MV| (WORK px) of the
    // PREVIOUS frame — 1-frame lag, in-band on the flow readback. Per tile
    // the LARGER axis component (a vertical pan must trip it exactly like
    // a horizontal one — v1 read X only and vertical pans never fired);
    // then the 85th percentile of those tile peaks, not the global max:
    // one extrapolating tile must not reset the model every frame.
    // FALL-SOFT: format families that fold to 0 (unorm MV) or a failed map
    // read as "no flow" — the reset never fires (never a false reset, only
    // a missing one; the knob at 0 is the diagnostic off).
    float FlowPeakPx() const
    {
        if (rbFlowPtr == nullptr || gtX == 0 || gtY == 0) return 0.0f;
        const size_t n = (size_t) gtX * gtY;
        // R90 (#6): mutable scratch — the vector is a pure scratch input
        // to nth_element; reusing it kills the per-frame heap churn.
        std::vector<float>& fx = const_cast<std::vector<float>&>(flowScratch);
        fx.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            const float ax = nr_half_to_float(
                (unsigned short)(rbFlowPtr[i * 2] & 0xFFFF));
            const float ay = nr_half_to_float(
                (unsigned short)(rbFlowPtr[i * 2] >> 16));
            fx[i] = max(ax, ay);
        }
        auto m = fx.begin() + (n * 85) / 100;
        std::nth_element(fx.begin(), m, fx.end());
        return *m;
    }

    // Exposure anchor (R82): the game's OWN preExposure (what its DLSS
    // evaluate published this frame) is the primary source — the fork and
    // AMDNR both anchor on it (the meter-measured median chases itself:
    // AMDNR measured 57 >1.5x jumps in one frame with a self-reading
    // meter). Fallback to the tile median (0.18/median, 1-frame lag) when
    // the game does not publish one.
    // R82c: LUMA-gain median from the PRE-normalisation tile readback —
    // next frame divides all channels by it (uniform scale: kills global
    // brightness drift, preserves chroma; 1-frame lag like the exposure
    // median). Tiles carry pre-norm luma: normalising an already-normalised
    // gain compounds (measured 1.5x runaway before this). R82e: the
    // medians are PER CHANNEL (packed f16 RGB in the uint2 tiles) — the
    // luma-only anchor let the model's global blue bias through (B 0.85).
    void UpdateGainNorm()
    {
        if (!rbGainTilesPtr) return;
        const size_t n = (size_t)gtX * gtY;
        // (R89a: the R82e PROBE block is gone with the dProbe flag — the
        // DEADBEEF/CAFEBABE sentinel check ran the shader path that skips
        // the gain-tile store, poisoning this median with raw texel words
        // on frame 3 of every arm. Forensics completed; nothing reads it.)
        // R90 (#6): channel vectors are preallocated members (gainScratch)
        // — the 3x43200-float churn left the per-frame path.
        // R91b: SAMPLED median — every 8th tile. The median of a ~1.0-
        // clustered population is not a precision instrument (the log
        // shows 1.004/1.000/0.987 frame after frame); 1/8 of the tiles
        // pins it equally well and the unpack+nth_element cost drops 8x.
        std::vector<float>* ch[3] = { &gainScratch[0], &gainScratch[1],
                                      &gainScratch[2] };
        // R91b BUGFIX: the resize must happen BEFORE the writes — the
        // original resized per call (no-op once stable) and my sampled
        // rewrite moved it after the loop, writing ~5400 floats into
        // EMPTY vectors on every fresh engine -> heap corruption ->
        // device removed -> poisoned fence -> re-arm loop every ~9 s.
        const size_t sn = (n + 7) / 8;
        for (int c = 0; c < 3; ++c) ch[c]->resize(sn);
        for (size_t i = 0, k = 0; i < n; i += 8, ++k)
        {
            const uint32_t x = rbGainTilesPtr[i * 2], y = rbGainTilesPtr[i * 2 + 1];
            (*ch[0])[k] = nr_half_to_float((unsigned short)(x & 0xFFFF));
            (*ch[1])[k] = nr_half_to_float((unsigned short)(x >> 16));
            (*ch[2])[k] = nr_half_to_float((unsigned short)(y & 0xFFFF));
        }
        float norm[3] = { 1, 1, 1 };
        // R82f FINAL: the runtime's answer is full-height (the "second
        // half" is its history copy) — the median spans ALL tiles again.
        for (int c = 0; c < 3; ++c)
        {
            auto m = ch[c]->begin() + ch[c]->size() / 2;
            std::nth_element(ch[c]->begin(), m, ch[c]->end());
            if (*m > 0.5f && *m < 2.0f) norm[c] = *m;
        }
        // R82g.3 ANTI-FLICKER (measured 5-state gain cycle on frozen input):
        // the median normalisation forms a lag-1 feedback loop — tiles(N)
        // come from gain(N) which was divided by tiles(N-1) — and with the
        // model's history in the loop the medians oscillate (measured mean
        // gain 0.0157 -> 0.0252 = +60% frame to frame on IDENTICAL input).
        // An EMA on the norm breaks the loop: the effective loop gain drops
        // below 1 and the cycle damps out. EMA is unbiased — the average
        // edit is unchanged; only the frame-to-frame oscillation dies.
        const float a = 0.2f;   // EMA weight of the fresh median
        normR = { normR.x * (1.f - a) + norm[0] * a,
                  normR.y * (1.f - a) + norm[1] * a,
                  normR.z * (1.f - a) + norm[2] * a };
        // R82e diagnosis: the normalisation was a silent no-op for three
        // iterations (raw gain shipped everywhere) — log the medians AND
        // the raw first-tile words rarely, so the engine log shows which
        // link of the readback chain is dead (write / copy / map)
        if (((++gnDbg) % 64) == 1)
            sli::Log("vendor: gainnorm med R %.3f G %.3f B %.3f (n %zu) "
                     "raw %08X %08X %08X %08X | tiles0 %08X (gain stride 8 vs 4)",
                     normR.x, normR.y, normR.z, n,
                     rbGainTilesPtr[0], rbGainTilesPtr[1],
                     rbGainTilesPtr[2 * (n / 2)], rbGainTilesPtr[2 * (n / 2) + 1],
                     rbTilesPtr ? rbTilesPtr[0] : 0xDEADBEEDu);
    }

    // R92f: TWO SEPARATE CONCEPTS —
    //  (a) RECONSTRUCTION exposure: the game's preExposure (what its own
    //      pipeline will divide by). We HOLD it across gaps (R82j — the
    //      per-frame fallback flicker source). This is expoScale: the
    //      value the decode's E cancels against in the ratio.
    //  (b) ENCODE white point: the ADAPTIVE METER (0.18/median of RAW
    //      luma, 1-frame lag — fork dlssnr meter pattern). R82j reused
    //      preExposure for this too, but RDR2 sends a CONSTANT 1.0: the
    //      meter was silently dead and night frames reached the model as
    //      near-black sRGB (raw luma ~0.005) — the model's answer was
    //      pure noise and the NR view painted almost black ("la vista
    //      está muy oscura", user 2026-10-04 night). The tiles measure
    //      PRE-E luma (encode writes luma before scaling by gExpoScale),
    //      so this loop has NO feedback: E(meter) never touches what
    //      the tiles read.
    void UpdateExposure(float gamePreExposure)
    {
        if (gamePreExposure > 0.001f && gamePreExposure < 4096.f)
        {
            expoScale = gamePreExposure;
            exposureHeld = true;
        }
        else if (!exposureHeld)
            expoScale = 1.0f;   // nothing spoken yet: neutral, hold semantics
        // (b) ALWAYS runs: the meter feeds the ENCODE, not the ratio.
        if (!rbTilesPtr) return;
        const size_t n = (size_t)tilesX * tilesY;
        const size_t sn = (n + 7) / 8;
        tileLums.resize(sn);
        for (size_t i = 0, k = 0; i < n; i += 8, ++k)
            tileLums[k] = nr_half_to_float(rbTilesPtr[i]);
        auto mid = tileLums.begin() + sn / 2;
        std::nth_element(tileLums.begin(), mid, tileLums.end());
        const float med = *mid;
        if (med > 1e-4f)
            whitePoint = 0.18f / med;
    }

    void Shutdown()
    {
        if (fwd && feature)
        {
            if (fRel) fRel(feature);
        }
        feature = nullptr;
        ok = false;
    }

    // Knobs are CREATE-time -> moving a panel slider means release +
    // re-create of the feature with the new values.
    bool RebuildKnobs(float kIntensityIn, float kLocalStructureIn,
                      float kLocalToneIn, float kSkinStructureIn,
                      unsigned kStyleIn,
                      const std::function<bool()>& begin,
                      const std::function<bool(const char*)>& flush)
    {
        if (!fwd || !fCreatePtr || !fEval) return false;
        if (feature)
        {
            if (fRel) fRel(feature);
            feature = nullptr;
        }
        kIntensity = kIntensityIn; kLocalStructure = kLocalStructureIn;
        kLocalTone = kLocalToneIn; kSkinStructure = kSkinStructureIn;
        kStyle = kStyleIn;
        // new knobs on the block (create reads them from here)
        set_flt(P, fslot, "DLSSNR.Intensity", kIntensity);
        set_uint(P, "DLSSNR.Style", kStyle);
        set_flt(P, fslot, "DLSSNR.LocalStructureStrength", kLocalStructure);
        set_flt(P, fslot, "DLSSNR.LocalToneStrength", kLocalTone);
        set_flt(P, fslot, "DLSSNR.SkinStructureStrength", kSkinStructure);
        // R90 (#1): PQV is create-time too — a panel change rides the same
        // nrParamSeq rebuild (float slot + uint shadow, the R82f lesson).
        {
            const float pqv = (tuning && tuning->nrPerfQuality >= 0.0f &&
                               tuning->nrPerfQuality <= 4.0f)
                                  ? tuning->nrPerfQuality : 2.0f;
            pqvAtCreate = pqv;
            set_flt(P, fslot, "PerfQualityValue", pqv);
            set_uint(P, "PerfQualityValue", (unsigned) pqv);
        }
        // golpe 1b: same valve as Init — settle ratio AFTER any probe
        // (query writes PQV+ratio into the block; rc!=1 -> classic 1.0 pin)
        if (tuning && tuning->nrRatioPin > 0.0f)
        {
            set_flt(P, fslot, "DLSSNR.ScalingRatio", tuning->nrRatioPin);
            activeRatio = tuning->nrRatioPin;
        }
        else if (fQueryRatio != nullptr)
        {
            float r = -1.0f;
            std::wstring snip0 = gameDirW + L"\\nvngx_dlssnr.dll";
            const int vrc = fQueryRatio(snip0.c_str(), P,
                                        (unsigned) (pqvAtCreate + 0.5f), &r);
            if (vrc == 1 && r > 0.0f)
            {
                activeRatio = r;  // the callback wrote DLSSNR.ScalingRatio
            }
            else
            {
                set_flt(P, fslot, "DLSSNR.ScalingRatio", 1.0f);
                activeRatio = 1.0f;
                sli::Log("vendor: ratio valve not reachable (rc %d) - "
                         "pinned 1.0", vrc);
            }
        }
        if (!begin()) return false;
        std::wstring snip = gameDirW + L"\\nvngx_dlssnr.dll";
        auto fCreate = (PFN_FwdCreate) fCreatePtr;
        feature = fCreate(snip.c_str(), gameDirW.c_str(), devPtr, cmdPtr, P,
                          w, h, 0 /*preset*/, kIntensity, kStyle,
                          kLocalStructure, kLocalTone, kSkinStructure,
                          1 /*autoMask*/, 1 /*uiCorrection*/);
        if (!flush("vendor-rebuild") || !feature)
        {
            sli::Log("vendor: rebuild with new knobs FAILED");
            ok = false;
            return false;
        }
        seq = 0; // first evaluate after create = reset
        sli::Log("vendor: feature rebuilt (intensity %.2f structure %.2f "
                 "tone %.2f skin %.2f style %u pqv %.1f)",
                 kIntensity, kLocalStructure, kLocalTone, kSkinStructure,
                 kStyle, pqvAtCreate);
        return true;
    }
};
