// sli_ctl.h — the ONE definition of the live control protocol between the
// host (nvngx.dll inside the game) and the panel/CLI tools. Ported from the
// POC's coproc_ctl_common.h (v2.5) as v3: versioned magic, batched applies,
// mirror-by-field, and the delta-tint field. Both peers include THIS file;
// the layout is enforced by static_assert so the copy-rot that killed the
// POC's coproc_ctl.exe (v2.1 magic vs v2.5 host) can never happen again.
//
// Mapping: Local\sli_ctl_v3 (host creates it; tools open it).
// Protocol: verbs get/set/reset/set-batch/subscribe. The writer bumps seq
// per request; the host echoes it in ack. The host rewrites the state
// mirror and the heartbeat after each poll (~25 ms).
//
// Forward compatibility: fields are appended at the tail only, never
// reordered; a panel that sees a higher version than its own reads the
// fields it knows and displays "host protocol vN" in the About tab.
#pragma once
#include <cstdint>

namespace sli {

constexpr uint64_t CTL_MAGIC       = 0x534C4943544C7633ull; // "SLICTLv3"
constexpr uint32_t CTL_VERSION     = 3;
constexpr wchar_t CTL_MAP_NAME[]   = L"Local\\sli_ctl_v3";

// Max pairs per set-batch message (Apply of the whole panel state in 1-3
// messages instead of ~23 single sets).
constexpr uint32_t CTL_MAX_BATCH = 8;

struct CtlMsg
{
    // --- header (request/ack) ---
    volatile uint64_t magic;   // CTL_MAGIC
    volatile uint32_t version; // CTL_VERSION of the writer
    volatile uint32_t verb;    // 1=get 2=set 3=reset 4=set-batch 5=subscribe
    volatile uint32_t field;   // CTL_FIELD_* (single-set/get/reset)
    volatile float    fval;    // value for single set
    volatile uint32_t seq;     // writer bumps per request
    volatile uint32_t ack;     // host echoes the writer's seq
    // --- set-batch payload (verb 4): up to CTL_MAX_BATCH field/value pairs ---
    volatile uint32_t batchCount;                   // 0..CTL_MAX_BATCH
    volatile uint32_t batchField[CTL_MAX_BATCH];    // CTL_FIELD_*
    volatile float    batchValue[CTL_MAX_BATCH];
    // --- state mirror (host writes after each request; tools display it) ---
    // Index by field id directly (array covers 1..CTL_FIELD_COUNT-1).
    // Sized by COUNT — it stayed [32] when the ids reached 36 and
    // mirror[33..36] trampled mirrorSource[1..4] (the "? 0.000" dumps,
    // R82k.7); R82m grows it again for id 37. EDIT PEERS IN THE SAME COMMIT.
    volatile float    mirror[45];                   // float view of every field
    volatile uint32_t mirrorSource[45];             // 0 = game wins, 1 = panel
                                                    // (overrides vs always-value)
    volatile uint32_t mirrorGameLuidLo, mirrorGameLuidHi; // game adapter LUID:
                                                    // panels skip it in combos
    volatile uint32_t enginePid;                    // live engine process id (0 = none)
    volatile uint32_t hb;      // host heartbeat (GetTickCount lo32)
    volatile uint32_t pad;     // 8-byte friendliness
};
static_assert(sizeof(CtlMsg) == 480, "ctl msg layout v3 (golpe 4: mirror 45)");

// Field ids — stable once published; APPEND ONLY (never reorder, never reuse).
constexpr uint32_t CTL_FIELD_OFFLOAD    = 1,
                   // (3 was CAPTURE — REMOVED R89b: no consumer; hole)
                   // (2 reserved)
                   // (4 was SHARPNESS — REMOVED R82i.5: never consumed; the
                   //  game's sharpness applies in its native SR pass)
                   CTL_FIELD_FORCERESET = 12,
                   // (7,8 were MVSCALE_X/Y, 9,10 JITTER_X/Y — REMOVED R82i.5:
                   //  overrides never consumed, the host always feeds the
                   //  game's values; 11 EXPOSCALE — REMOVED: the vendor uses
                   //  the game's preExposure or its own auto-median; 13,14
                   //  MVOFFSET_X/Y — REMOVED: never read nor applied; 15 FTD
                   //  — REMOVED: never wired)
                   CTL_FIELD_NR_INT     = 16,
                   CTL_FIELD_NR_LS      = 17,
                   CTL_FIELD_NR_LT      = 18,
                   CTL_FIELD_NR_SS      = 19,
                   CTL_FIELD_NR_ON      = 20,
                   CTL_FIELD_GPU_INDEX  = 21,
                   CTL_FIELD_NR_STYLE   = 22,
                   CTL_FIELD_NR_WORKSCALE = 24, // (23 was NR_STAGE — REMOVED with pre-SR;
                                                //  id not reused by policy)
                   CTL_FIELD_NR_BOOST   = 25,
                   CTL_FIELD_NR_TINT    = 26,  // delta-tint visual verification
                   // (27 was PREEXPOSURE — HOLE since R89d: declared live
                    //  by relocation history but the host never had a case
                    //  for it (pre-exposure rides FrameScalars); never
                    //  reuse the id)
                   CTL_FIELD_NR_SPLIT   = 28, // test-split A/B at Present:
                                               //  delta on the left half only
                   // (29 was NR_MODELH — REMOVED R82g.4: the answer fills the
                   //  whole work buffer, no fraction knob; id not reused by policy)
                   CTL_FIELD_SR_BYPASS  = 31, // HOLE (R84.1: srBypass knob
                                              //  removed — dead in RDR2;
                                              //  never reuse the id)
                   CTL_FIELD_SR_RATIO   = 32, // HOLE (R84.1: srRatio knob
                                              //  removed — dead in RDR2;
                                              //  never reuse the id)
                   CTL_FIELD_NR_DETAIL  = 33, // R82k: model luminance-ride
                                              // strength (1.0 exact)
                   CTL_FIELD_NR_COLOUR  = 34, // R82k: model chroma pass (0.25)
                   CTL_FIELD_NR_AUTOSKIN= 35, // R82k: DLSSNR.UseAutoMask (1)
                   CTL_FIELD_NR_HIGUARD = 36, // R84: per-pixel gain bound
                                              //  (was "highlight guard";
                                              //  same id, semantics renamed)
                   CTL_FIELD_NR_PSTRIDE = 37, // R82m present-pace probe —
                                              //  REMOVED in R88.2 (obsolete
                                              //  diagnostic; the ghost A/B
                                              //  is done). APPEND-ONLY: the
                                              //  id stays a documented hole,
                                              //  never reused; the host
                                              //  ignores the field.
                   CTL_FIELD_NR_FLOWRESET = 38, // R84: history auto-reset —
                                                //  % of frame height the
                                                //  scene may shift before
                                                //  the model history resets
                                                //  (0 = off; 6 default since
                                                //  R84's in-game calibration
                                                //  — the shipped 15 let half
                                                //  the pans through)
                   CTL_FIELD_NR_GATEWAIT = 39,  // R88: present-gate bounded
                                                //  wait in ms for delta N
                                                //  (0 = UNBOUNDED — max NR
                                                //  fidelity; >0 = native
                                                //  present on miss, no
                                                //  latch, next Present
                                                //  retries)
                   CTL_FIELD_NR_PERFQ    = 40, // R90: vendor PerfQuality
                                               //  0=MaxPerf..4=MaxQuality
                                               //  (create-time — rebuilds
                                               //  the model, ~1 s)
                   CTL_FIELD_NR_RATIOPIN = 41, // golpe 1b: ScalingRatio
                                               //  valve. 0 = OPEN (model
                                               //  picks its work res from
                                               //  PerfQuality); >0 = pinned
                                               //  ratio (1.0 = classic)
                   CTL_FIELD_NR_DEPTHMODE = 42, // golpe 2: 0 = real game
                                                  //  depth (clone seal; 1.0
                                                  //  proxy fallback), 1 =
                                                  //  always flat proxy
                   CTL_FIELD_NR_DEJITTER = 43, // HOLE (R92: de-jitter became
                                               //  structural — colour AND
                                               //  depth guides get the -j
                                               //  shift whenever the frame
                                               //  carries jitter; identity
                                               //  when jitter==0. Never
                                               //  reuse the id)
                   CTL_FIELD_NR_RESIDUAL = 44, // golpe 4: matched-residual
                                               // compose — edit carried up
                                               // from the small raster onto
                                               // the full proxy (blur
                                               // cancels; fork pattern)
                   CTL_FIELD_COUNT      = 45; // mirror[] is indexed by id
                                              //  (covers 1..COUNT-1)

} // namespace sli
