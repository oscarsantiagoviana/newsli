// sli_abi.h — the single-source contract between the host proxy (nvngx.dll,
// inside the game process) and the NR engine (sli_engine.exe, on the second
// GPU). Ported from the POC's coproc_common.h with the dead pre-SR delivery
// path removed and the delta-tint knob made a real ABI field.
//
// Design rules inherited from the POC (all paid for with live crashes):
//  - The ENGINE owns the only NGX session on the second GPU, in its own
//    process. The game's process must never run a second NGX session
//    (in-process attempts crashed RDR2 deterministically, five times).
//  - Transport is cross-adapter shared heaps opened cross-process via
//    DuplicateHandle + OpenSharedHandle; sync is two shared fences
//    (produce/done) plus this named file mapping for per-frame scalars.
//  - Float overrides use -1.0f = "the game wins". NR knobs are ALWAYS-VALUE
//    (fork value_or_default semantics): a knob is never "unset".
//  - Struct layout is ABI: any field change means rebuilding and redeploying
//    host AND engine together. Never one.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace sli {

// ---------------------------------------------------------------------------
// Versioning — every structure carries a version; readers reject mismatches
// instead of misinterpreting (the POC's copy-rot v2.1-vs-v2.5 ctl bug).
// ---------------------------------------------------------------------------
constexpr uint32_t ABI_VERSION = 3;

// ---------------------------------------------------------------------------
// Per-frame scalars: host -> engine, through a named file mapping.
// Written by the host BEFORE it signals the produce fence; read by the engine
// AFTER it observes that fence. Plain Win32, no D3D12 objects involved.
// ---------------------------------------------------------------------------
struct FrameScalars
{
    uint64_t frame;              // produce fence value this block belongs to
    float jitterX, jitterY;      // exactly as the game handed them to DLSS
                                 // (0 in POST-SR — the colour is de-jittered)
    float preExposure;           // HDR: DLSS divides color by pre-exposure
    float mvScaleX, mvScaleY;    // the game's MV_Scale (render-pixel units)
    uint32_t reset;              // game asked the upscaler to forget history
    uint32_t engineResult;       // engine -> host: 1 = delta computed AND
                                 // delivered this turn (0 = the model
                                 // threw; a 90 s zero streak re-arms)
    uint32_t renderW, renderH;   // this frame's render dims (DRS); 0 = args dims
    // HOST frame whose data the engine actually read (the produce value at
    // processing time). The done fence carries the ENGINE counter (starts at
    // 0 when the engine spawns, lags behind) — useless for ring lookups.
    // Written right before the done signal; the host reads it on delivery.
    uint64_t engineHostFrame;
};
static_assert(sizeof(FrameScalars) == 56, "FrameScalars layout is ABI (R82i.5: sharpness out, 4 B tail pad)");

// ---------------------------------------------------------------------------
// Handshake: the host duplicates the shared NT handles into the engine
// process AFTER CreateProcess(suspended) and writes their values here; the
// engine reads them before its first frame. Lives right after the scalars
// block in the same file mapping.
// ---------------------------------------------------------------------------
struct Handshake
{
    uint64_t magic;              // HANDSHAKE_MAGIC when written
    // (R89b: hid/him were dead 16 B here — the depth/motion guides are the
    //  triple-buffered hgd/hgm arrays below and always were; the single-
    //  buffer fields were never read by any engine. Layout note: the two
    //  removed fields stay as reserved so every later offset is unchanged.)
    uint64_t hic;                // color-in buf
    uint64_t reserved16, reserved24;
    uint64_t hoc;                // color-out HEAP (R90: one shared heap,
                                 //  THREE placed slots at kOutRingStride
                                 //  intervals — the engine writes slot
                                 //  [sealedFrame % 3], the host composes
                                 //  reading slot [N % 3]. The handle is the
                                 //  heap's, exactly as before (it always
                                 //  was); only the layout inside grew.
    uint64_t hpf, hdf;           // produce / done shared fences
    // Real guides, triple-buffered: the host seals frame N's guide into
    // hgd/hgm[N % 3] together with produce N; the engine evaluates frame N
    // reading slot [N % 3] — the SAME frame's guides (see engine_ctx.h;
    // the old "one behind" note here described a lag the code never had).
    uint64_t hgd[3];             // depth buffers (footprint row-major, cross-adapter)
    uint64_t hgm[3];             // motion buffers
    uint32_t guideDepthFmt;      // REAL game DXGI format (e.g. R32G32_SINT, raw)
    uint32_t guideMvFmt;
    uint32_t guideDepthPitch;    // footprint RowPitch (256-aligned) per row
    uint32_t guideMvPitch;
    uint32_t guideW, guideH;     // useful guide dims (= arm-time render dims)
    uint32_t depthInverted;      // bit 1<<3 of the game's DLSS create-flags
    uint32_t version;            // ABI_VERSION of the writer
    volatile uint64_t ready;     // engine writes READY_MAGIC when live
};
static_assert(sizeof(Handshake) == 144, "Handshake layout is ABI");
static_assert(offsetof(Handshake, ready) == 136, "ready offset is ABI");

// ---------------------------------------------------------------------------
// Live tuning block: host writes, engine reads each frame (offset 2048 in
// the same mapping). The panel talks to the HOST through the ctl mapping;
// the host relays authoritative values here. The engine never opens ctl.
// ---------------------------------------------------------------------------
struct Tuning
{
    volatile uint64_t magic;     // TUNING_MAGIC when initialized
    volatile float nrPerfQuality;// (was reserved8/seq — R90: the vendor's
                                 //  PerfQualityValue, 0=MaxPerf .. 4=MaxQuality.
                                 //  CREATE-time: a change rebuilds the feature
                                 //  (rides nrParamSeq, ~1 s). 2 = balanced,
                                 //  what always ran. The R82g accident proved
                                 //  the knob bites: a PQV that fell to MaxPerf
                                 //  answered at ~43% of the work height.)
    volatile uint32_t offloadOn; // 0 = pass frames to native this frame
    volatile float nrRatioPin;  // golpe 1b: DLSSNR.ScalingRatio pin. >0 =
                                // force that ratio at create (1.0 = the
                                // classic full-res pin); 0.0 = DON'T write
                                // it — the model computes its own from
                                // nrPerfQuality (the fork's create does
                                // exactly this). CREATE-time (nrParamSeq).
    volatile uint32_t forceReset;// 1 = discard model history EVERY frame
                                 //     (the per-frame contribution made
                                 //     visible; kills temporal smoothing)
    // (R82i.5 removed the dead game-override fields: sharpness, mvScale,
    //  jitter, preExposure, exposureScale, mvOffset, frameTimeDelta — none
    //  was ever consumed; the host feeds the engine the game's real values
    //  through FrameScalars. IDs stay holes in ctl_common.h.)
    // NR model knobs — CREATE-time strengths (evaluate-only strengths are
    // ignored by the runtime), so the engine REBUILDS the feature when
    // nrParamSeq changes. Always-value: <0 reads as the vendor default.
    volatile float nrIntensity;         // DLSSNR.Intensity
    volatile float nrLocalStructure;    // LocalStructureStrength
    volatile float nrLocalTone;         // LocalToneStrength
    volatile float nrSkinStructure;     // SkinStructureStrength
    volatile uint32_t nrOn;             // 1 = run NR this frame
    volatile uint32_t nrParamSeq;       // bumped when nr* strengths change
    volatile int32_t gpuIndex;          // -1 = auto (first non-game NVIDIA)
    volatile uint32_t nrStyle;          // DLSSNR.Style (0 default)
    volatile float nrWorkScale;         // model runs at render*scale (fork
                                        // DlssNrWorkingScale). 1.0 = exact.
    volatile float nrBoost;             // delta multiplier at composition
                                        // (1.0 = fork exact; 2-4 = visible
                                        // A/B — panel/host clamp at 4.0;
                                        // NOT a quality knob — amplifies
                                        // signal and noise alike).
    volatile uint32_t nrTint;           // binary view flag: 0 = quality
                                        // (transported gain), 1 = NR OUTPUT
                                        // VIEW — the screen shows the
                                        // model's pure output (opaque draw,
                                        // scissor-cut by the split seam).
                                        // Live, no re-arm. ABI: offset and
                                        // size unchanged (R82d rework).
    volatile uint32_t nrTestSplit;      // Present-gate test split (A/B), SEAM POSITION in
                                        // permille of screen width: 0 = off,
                                        // 500 = centre, 1..1000 = left of the
                                        // seam gets the delta, right keeps
                                        // the game's untreated output (fork
                                        // CompareSplit concept).
    // (R95: nrPreSr REMOVED — the pre-SR placement is dead; POST-SR is the
    //  only line. It sat here at offset 72; everything after it moved down
    //  4 bytes. Host+engine+panel always rebuild together, so there is no
    //  cross-version ABI to preserve.)
    // ---- R82k: compose strengths + auto skin mask (append-only tail) ----
    volatile float    nrDetail;         // DETAIL STRENGTH: how much of the
                                        // model's luminance verdict rides on
                                        // top of the frame. 1.0 (DEFAULT) =
                                        // the forks' transfer strength exact;
                                        // 0 = edit off; >1 amplifies.
    volatile float    nrColour;         // COLOUR STRENGTH: how much of the
                                        // MODEL'S OWN chroma shift passes into
                                        // the frame (0..2; 0.25 DEFAULT = the
                                        // forks ship ~0 — model colour shifts
                                        // are the part you do not want).
    volatile uint32_t nrAutoSkin;       // DLSSNR.UseAutoMask (R82k): 1
                                        // (DEFAULT, what we always sent) = the
                                        // model finds skin itself; 0 = treat
                                        // the frame uniformly. Live evaluate
                                        // arg — no re-arm.
    volatile float    nrGainBound;      // PER-PIXEL GAIN BOUND (R84, fork
                                        // MaxRatio; 2.0 DEFAULT): the
                                        // composition clamp on the luma
                                        // ratio, per pixel, 1..8. Replaces
                                        // the R82k highlight-knee (a global
                                        // patch; the fork ships no such
                                        // case — its guard :1016 is this
                                        // bound). Live decode arg.
    // ---- offset 88 RESERVED (R88.2/R95): was nrPresentStrideMs (R82m
    // present-pace probe, removed), later depthMode's home before the R95
    // compact — now nrDepthMode sits here and the reserved 4 bytes moved
    // out with nrPreSr's deletion. Ctl id 37 is a documented hole.
    volatile uint32_t nrDepthMode;   // golpe 2: 0 = REAL game depth via
                                     //  the clone seal (fallback 1.0 proxy
                                     //  when the frame carries no depth
                                     //  resource); 1 = ALWAYS flat proxy
                                     //  (A/B / rollback).
    volatile float    nrFlowReset;      // R84 HISTORY AUTO-RESET: % of the
                                        // frame height the scene may shift
                                        // (per-tile MV peak, 1-frame lag)
                                        // before the model history resets
                                        // before the decode. 6 DEFAULT
                                        // (R84 in-game calibration: a pan
                                        // at 8 fps shifts 10-12 %/frame —
                                        // the shipped 15 let half the pans
                                        // through); at 60 fps the
                                        // per-frame shift stays under it,
                                        // so the trigger is inert — fork
                                        // behavior. 0 = off (byte-identical
                                        // to pre-R84).
    // ---- R88: present-gate bounded wait (offset 100; R95 compact pins
    // every field — see the static_asserts at the end of this struct) ----
    volatile uint32_t nrGateWaitMs;     // 0 (DEFAULT) = UNBOUNDED gate wait:
                                        // the presented frame always carries
                                        // ITS OWN delta (same-frame R79 at
                                        // full fidelity — NR coverage is
                                        // total, fps floor is the engine's).
                                        // >0 = the gate waits at most this
                                        // many ms for delta N; if it has not
                                        // landed, the frame presents NATIVE
                                        // (never a stale composite) and the
                                        // session keeps living — the next
                                        // Present retries. Host-only
                                        // consumer (PresentGateInner). The
                                        // 10 s stuck-engine watchdog and the
                                        // engine-death latch stay regardless.
    // R92: nrDejitter REMOVED (was @104). De-jitter became STRUCTURAL:
    // the seal de-jitters COLOUR *and* the depth guide with the same -j
    // whenever the frame carries jitter (pre-SR), and every path
    // degenerates to bit-identical identity when jitter==0 (POST-SR) —
    // there is nothing left to decide per-frame, so the knob died
    // (user mandate: no debug flags for deterministic behavior).
    volatile float    nrResidual;       // golpe 4: matched-residual compose.
                                        // 1 (DEFAULT) = when the model runs
                                        // below render res the decode
                                        // subtracts the SMALL proxy from
                                        // the model's answer and adds the
                                        // edit onto the FULL-resolution
                                        // proxy — so the downsample blur
                                        // cancels instead of being read as
                                        // headroom (fork dlssnr.hlsl:860).
                                        // 0 = classic compose (rollback).
    // (R95: nrColorBack REMOVED — died with the pre-SR real line; the
    //  engine's payload selector is gone with its only consumer. It sat
    //  at offset 104 in the former tail padding.)
};  // R82g.4: nrModelH REMOVED (was a tail-padding knob; the answer fills
    // the whole work buffer — the decode maps 1:1, no fraction knob).
    // R84.1: srBypass/srRatio REMOVED (dead in RDR2 — the bypass guard
    // render==output never held with the game's DRS, and the ratio lived
    // behind the wrapped callback the game never calls; R85 supersedes
    // both with real SR backends). IDs 31/32 stay documented holes.
static_assert(offsetof(Tuning, nrResidual) == 100, "R95: residual at 100 (preSr+colorBack compacted out)");
// R95 sizeof note: nrPreSr (@72) and nrColorBack (@108 tail pad) removed —
// sizeof shrinks 112 -> 104. Host+engine+panel rebuilt together, so no
// cross-version ABI exists.
static_assert(sizeof(Tuning) == 104, "Tuning layout is ABI (R95: preSr/colorBack compacted; sizeof 104)");
// R82i.5: the dead-override floats (offsets 44..83) are GONE — every offset
// after them moved down by 40. Host+engine+panel rebuild together (deploy
// rule), so there is no cross-version ABI to preserve.
// R89b: seq/capture are reserved8/reserved16 now — the static_asserts below
// pin the layout the same way the fields did.
static_assert(offsetof(Tuning, nrPerfQuality) == 8, "nrPerfQuality (was reserved8) offset is ABI");
static_assert(offsetof(Tuning, offloadOn) == 12, "offloadOn offset is ABI");
static_assert(offsetof(Tuning, nrRatioPin) == 16, "nrRatioPin (was reserved16) offset is ABI");
static_assert(offsetof(Tuning, forceReset) == 20, "forceReset offset is ABI");
static_assert(offsetof(Tuning, nrTint) == 64, "nrTint offset is ABI");
static_assert(offsetof(Tuning, nrTestSplit) == 68, "nrTestSplit offset is ABI");
// (R95: nrPreSr's offset-72 assert gone with the field.)
static_assert(offsetof(Tuning, nrDetail) == 72, "nrDetail offset is ABI (R95: moved down from 76 with nrPreSr)");
static_assert(offsetof(Tuning, nrColour) == 76, "nrColour offset is ABI (R95 compact)");
static_assert(offsetof(Tuning, nrAutoSkin) == 80, "nrAutoSkin offset is ABI (R95 compact)");
static_assert(offsetof(Tuning, nrGainBound) == 84, "nrGainBound offset is ABI (R95 compact)");
static_assert(offsetof(Tuning, nrDepthMode) == 88, "nrDepthMode offset is ABI (R95 compact)");
static_assert(offsetof(Tuning, nrFlowReset) == 92, "nrFlowReset offset is ABI (R95 compact)");
static_assert(offsetof(Tuning, nrGateWaitMs) == 96, "gate wait offset is ABI (R95 compact)");

// ---------------------------------------------------------------------------
// Offsets and magics of the frame mapping (named, created by the host).
// Layout: [FrameScalars 0..256) [Handshake 256..368) [Tuning @2048)
// ---------------------------------------------------------------------------
constexpr size_t SCALARS_SIZE   = 256;
constexpr size_t MAP_SIZE       = 8192;
constexpr size_t HANDSHAKE_OFFSET = SCALARS_SIZE;
constexpr size_t TUNING_OFFSET  = 2048;

// R90 output ring: the out heap holds 3 slots. Slot k starts at
//   offset(k) = k * RoundUp64K(frameBytes)
// where frameBytes = PitchFor(w, bpp) * h — the SAME value host and engine
// already compute (MakeSharedBuf's `bytes` / the engine's `nrBytes`), so no
// new ABI field is needed; the layout rule is the contract. Placed-resource
// offsets on a shared cross-adapter heap must be heap-aligned (64K here),
// hence the rounding.
constexpr uint64_t kOutRingSlots = 3;

constexpr uint64_t HANDSHAKE_MAGIC = 0x534C4948414E4453ull; // "SLIHANDS"
constexpr uint64_t READY_MAGIC     = 0x534C495245414459ull; // "SLIREADY"
constexpr uint64_t TUNING_MAGIC    = 0x534C4954554E4947ull; // "SLITUNING"

// The named frame mapping: "Local\\sli_frame_<pid>" (host pid).
inline void FrameMapName(unsigned pid, wchar_t* out, size_t cch)
{
    _snwprintf_s(out, cch, _TRUNCATE, L"Local\\sli_frame_%u", pid);
}

} // namespace sli
