// offload_session.h — the host-side offload session (F3): transport, engine
// spawn, per-frame seal/compose and the session FSM from docs/fsm-design.md.
//
// The session REPLACES the POC's four ad-hoc hot re-arm paths with ONE state
// machine (IDLE -> ARMING -> LIVE -> DRAINING -> COOLDOWN). Hard rules the
// POC paid for (R69i/R69j/R70d/R70f/R70i), enforced structurally here:
//   - the game thread NEVER waits on the FSM: Evaluate checks one atomic and
//     either seals (LIVE) or forwards to native NGX (anything else);
//   - shared resources exist only while a session object exists (created in
//     ARMING on the session thread, destroyed in DRAINING's defined order);
//   - leaving LIVE kills every delivery flag, so a stale compose/history can
//     never freeze the picture;
//   - the engine is killed by PID (process handle), never by image name.
//
// The ONLY delivery path is post-SR: the native upscaler runs on the game's
// untouched inputs; the offload seals color/depth/MV to the engine on the
// second GPU, receives a display-domain multiplicative GAIN back, and
// composes it at the game's Present call (R79: same-frame, gain over the
// backbuffer — no temporal accumulator, no frame mixing).
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

struct NVSDK_NGX_Parameter;

namespace sli {
namespace host {

// One evaluate call, normalized from the game's NGX parameter block by the
// proxy (nvngx_host.cpp). All resources are the GAME's, valid only for the
// duration of the call — the session never stores them.
struct EvalArgs
{
    ID3D12Device* gameDev = nullptr;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> gameQueue; // observed submit queue
    ID3D12GraphicsCommandList* inCmdList = nullptr;       // the game's evaluate
                                                          // list: the seal is
                                                          // recorded HERE so the
                                                          // copies execute at the
                                                          // exact GPU point where
                                                          // native DLSS reads
    ID3D12Resource* srcColor = nullptr;
    ID3D12Resource* srcDepth = nullptr;   // golpe 2: the game's depth at
                                          // evaluate (null in menus)
    ID3D12Resource* srcMotion = nullptr;
    ID3D12Resource* dstOutput = nullptr;
    unsigned inW = 0, inH = 0;                    // render resolution this frame
    unsigned outW = 0, outH = 0;                  // output resolution
    // (R95 audit: colorBaseX/Y removed — the NGX subrect bases for colour
    //  were read but never consumed: the colour seal copies the whole
    //  resource. Depth/MV bases ARE live — used by the guide seals.)
    unsigned depthBaseX = 0, depthBaseY = 0;
    unsigned motionBaseX = 0, motionBaseY = 0;
    float mvScaleX = 1.f, mvScaleY = 1.f;
    float jitterX = 0.f, jitterY = 0.f;           // the game's DLSS jitter
    float preExposure = 1.f;                      // HDR: DLSS divides by it
    bool gameReset = false;                       // camera cut / history reset
    unsigned createFlags = 0;                     // DLSS create flags (bit 3
                                                  // = inverted depth; the
                                                  // engine needs it)
};

// Enable (>=0) / disable (-1). Decided once at NGX init time by the proxy.
void Configure(int adapterIndex);

// Per-frame entry from the proxy's EvaluateFeature interception. NEVER
// blocks, NEVER replaces the native evaluate: it seals the frame's inputs
// to the engine (when LIVE) and stages the delivered delta for the
// Present-gate compose. The caller always continues into the native SR
// evaluate.
void Evaluate(const EvalArgs& a);

// R82h: called by the proxy AFTER the native evaluate has been recorded into
// the same command list. Copies the game's SuperResolution OUTPUT
// (de-jittered, display res) into the transport. Recorded into the game's
// evaluate list, GPU-ordered after the native SR passes — same list, same
// submit, no extra flush.
void SealColorPost(const EvalArgs& a);

// (R82k: the NrDetail/NrColour/NrAutoSkin export trio was removed in R95 —
//  zero callers; the engine reads the live Tuning block from the shared
//  mapping, never these accessors.)

// GOLPE 5 (pre-SR real): when the session armed the Colour swap this
// frame (pre-SR real line), rewrites the evaluate block's Colour key to
// OUR texNr so the native SR consumes the NR'd colour. No-op otherwise.
// (R95: PreSrSwapColorIfArmed removed with the pre-SR mode.)

// The Present gate (R79): called by the dxgi swapchain proxy on the game
// thread, INSIDE the game's Present call. Waits (event-bounded, unbounded
// while the engine lives) for the engine's delivery of frame N and runs
// the named compose stage — a fullscreen-triangle draw of the
// delta over the backbuffer on the game's own queue (falling back to
// our direct queue when no game queue was ever observed) — then calls
// the real Present. Returns the real Present's HRESULT. When the session
// is not
// LIVE (or NR is off) it forwards untouched with zero overhead.
HRESULT PresentGate(IDXGISwapChain* real, UINT sync, UINT flags);

// Present1 variant: identical gate, with the game's DXGI_PRESENT_PARAMETERS
// passed through to the real call after the compose.
HRESULT PresentGate1(IDXGISwapChain* real, UINT sync, UINT flags,
                     const DXGI_PRESENT_PARAMETERS* params);

// Queue-observer callback (game's submit thread, AFTER the real
// ExecuteCommandLists): pushes the seal list behind the frame's list and
// GPU-orders the produce fence signal. Must never touch the FSM mutex.
void OnListSubmitted(void* list, ID3D12CommandQueue* queue);

// Game exit: synchronous DRAINING with a 1.5 s cap (best effort — the
// engine's own poison/quiet watch self-heals an orphan).
void Shutdown();

} // namespace host
} // namespace sli
