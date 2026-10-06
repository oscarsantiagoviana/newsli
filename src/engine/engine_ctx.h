// engine_ctx.h — internal context shared by the engine's translation units.
// Minimal port of the POC's coproc_engine.h: the CPU echo bridge
// (readback/upload/badge buffers) is gone — the engine's only output is the
// display-domain gain, and the only fallback is UNITY (1.0).
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <string>

#include "shared/abi.h"

using Microsoft::WRL::ComPtr;

struct EngineArgs
{
    unsigned long long luidLo = 0, luidHi = 0;
    unsigned w = 0, h = 0;      // render dims (the transport is symmetric)
    unsigned cf = 0;            // color DXGI format (raw, as the game uses)
    std::wstring mapName;       // named frame mapping (Local\sli_frame_<pid>)
    // NT handle values duplicated by the host, read from the handshake:
    unsigned long long hic = 0, hoc = 0;    // color-in / delta-out buffers
    unsigned long long hpf = 0, hdf = 0;    // produce / done shared fences
};

// nr_vendor.h must precede EngineCtx (NrVendor member).
#include "engine/nr_vendor.h"

struct EngineCtx
{
    EngineArgs args;

    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> localFence;
    UINT64 localFenceVal = 0;
    HANDLE localFenceEvt = nullptr;

    // Shared transport — cross-adapter placed BUFFERS (the shape the MGPU
    // bridge validated on this machine; shared placed TEXTURES are rejected
    // by this driver with E_INVALIDARG).
    // R90 (#11): the OUT side is a 3-slot ring on ONE shared heap (offsets =
    // k * RoundUp64K(nrBytes), the abi.h contract) — the engine no longer
    // serializes frame N+1 behind the host's compose of N. The IN side
    // stays single-buffered: the host writes N+1 only after produce N was
    // consumed anyway (gate ordering), and color-in is read exactly once.
    ComPtr<ID3D12Heap> heapInColor, heapOut;
    ComPtr<ID3D12Resource> bufInColor;
    ComPtr<ID3D12Resource> bufOut[sli::kOutRingSlots];  // the out ring slots
    UINT64 outSlotStride = 0;                       // RoundUp64K(nrBytes)
    ComPtr<ID3D12Resource> texInColor;  // sealed frame N (render dims)
    ComPtr<ID3D12Resource> texOut;      // delta output (render dims, UAV)

    // Zero-delta source: a persistent upload buffer the size of the output,
    // filled with UNITY (see loop.cpp top: multiplicative gain, 1.0 =
    // identity). Every disarmed path (no vendor, nrOn=0, DRS bypass, dead
    // streak, broken frame) delivers bufOut <- bufZero: no textures, no
    // state assumptions, compose changes nothing.
    ComPtr<ID3D12Resource> bufZero;
    UINT64 nrBytes = 0;

    // Real game guides (depth/MV), triple-buffered: the host seals frame
    // N's guide into hgd/hgm[N % 3] together with produce N; the engine
    // evaluates frame N reading bufGuideX[sealedFrame % 3] — sealedFrame
    // is the produce-fence value captured after the wait, so the slot
    // ALWAYS matches the sealed frame even when the game outproduces the
    // engine (a private counter would drift and read the wrong slot —
    // the R90 #5 mixing class).
    // (R82l doc fix: an older comment claimed a deliberate one-behind lag
    // "fork parity"; the code never had it — in-process forks feed colour N
    // AND guides N in the same evaluate, and so do we.)
    ComPtr<ID3D12Heap> heapGuideD[3], heapGuideM[3];
    ComPtr<ID3D12Resource> bufGuideD[3], bufGuideM[3];
    ComPtr<ID3D12Resource> texGuideDepth, texGuideMV; // local (raw game fmt)
    unsigned guideDepthFmt = 0, guideMvFmt = 0;
    unsigned guideDepthPitch = 0, guideMvPitch = 0;
    unsigned guideW = 0, guideH = 0;
    unsigned depthInverted = 0;

    NrVendor vendor;                      // in-process vendor NR backend
    ComPtr<ID3D12Fence> produceFence, doneFence; // (ready = handshake magic)
    HANDLE map = nullptr;
    sli::FrameScalars* scalars = nullptr;
    sli::Handshake* hs = nullptr;
};

// loop.cpp
bool OpenSharedTransport(EngineCtx& c);
int RunFrameLoop(EngineCtx& c);
