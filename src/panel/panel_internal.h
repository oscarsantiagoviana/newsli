// panel_internal.h — shared contract of the panel's translation units.
//
// panel.cpp used to be 1,575 lines carrying eight concerns in one file
// (R82d review). The split is mechanical, one concern per TU:
//
//   panel_internal.h  this file: the ONE control table, types, shared
//                     state declarations and cross-TU prototypes
//   panel_state.cpp   panel state + event log + sli.ini persistence
//   panel_host.cpp    ctl client (open/poll/apply) + GPU enumeration
//   panel_ui.cpp      ImGui rendering (rows, help, tabs, header)
//   panel.cpp         Win32 + DX11 bootstrap and the message loop
//
// Design contract (docs/panel-spec.md, analysis/03):
//  - ONE ControlSpec table drives rendering, ini keys, ctl fields, ranges,
//    defaults and the always-visible help text. Adding a control = adding
//    one table row; there are no parallel arrays to grow in lockstep (the
//    POC's "TRAMPA PANEL": four index-parallel arrays that corrupted
//    neighboring HWNDs the moment one grew alone). Row lookups go through
//    IdOf(field)/KeyIdx(key), constexpr scans of the table, so even those
//    cannot rot.
//  - The panel never pretends to be the truth. The host's mirror is the
//    truth: it is polled every 500 ms and drives the status dots, the
//    "Game: <value>" readouts and the live knob values. The panel's own
//    state is what WOULD be sent on Apply.
//  - Apply = live (batched ctl SET, effective immediately). Save =
//    persist sli.ini (the host reloads it when the offload (re)arms).
//    The header always says how the two diverge.
//  - Zero free-text input: every control is a bounded slider, a checkbox
//    or a combo.
//
// The game's live input values (jitter, MV scale, pre-exposure) are NOT in
// the ctl mirror. The host publishes them per frame in FrameScalars
// (Local\sli_frame_<host pid>), which the panel locates by walking
// enginePid -> parent process. The same mapping yields the engine counters
// (produce frame / consumed frame / engineResult) shown in Debug &
// Verification.
//
// R82l: the override machinery is GONE. No row had overrideable=true since
// R82i.5 — every control is ALWAYS-VALUE (sent as-is on every Apply, the
// fork's value_or_default semantics). The checkbox, the -1 "game wins"
// encoding, GameValue and the appliedOvr/savedOvr snapshots were dead code
// that still promised game-override semantics in the help text (panel
// honesty rule: no text for features that do not exist).

#pragma once

#include "shared/abi.h"
#include "shared/ctl_common.h"
#include "shared/log.h"

#include <cstdint>

using namespace sli;  // ctl fields, ABI constants (internal panel header)

// ---------------------------------------------------------------------------
// Version strings (About tab).
// ---------------------------------------------------------------------------
inline constexpr const char* kPanelName    = "new-sli control panel";
inline constexpr const char* kPanelVersion = "1.0";

// ---------------------------------------------------------------------------
// The ONE control table.
//
// Row order == drawing order inside each tab's group; tabs pick rows by
// field/key lookup, never by index. Conventions the render code relies on
// (the table IS the UI contract, so they are spelled out here):
//  - a combo item containing "(planned)" renders disabled and cannot be
//    selected (future-feature placeholder, analysis/03 §3);
//  - a `sends` string containing "re-arm" makes the row show the amber
//    "! re-arms (~2 s)" tag (gpuIndex, workScale);
//  - ini keys of ctl-mapped rows MUST match the host's CtlLoadIni names
//    (enforced by static_assert below) — Save writes the very file the
//    host reloads at (re)arm.
// ---------------------------------------------------------------------------
enum class Ctrl : int { Checkbox, SliderF, SliderI, Combo };

// R95c: slider quantization. ImGui sliders take full float precision, so
// a drag used to leave arbitrary fractions (flowReset 8.101, ratioPin
// 1.37...) — hard to hit an intended number, and the ini carried the
// noise. Each row now declares its step; the draw rounds to it.
inline constexpr float SliderStep(float lo, float hi)
{
    // coarse ranges (>=8 wide) step by 0.5; medium (>=2) by 0.1; fine by
    // 0.05. Every declared default lands exactly on its step grid.
    const float span = hi - lo;
    if (span >= 8.0f)  return 0.5f;
    if (span >= 2.0f)  return 0.1f;
    return 0.05f;
}

struct ControlSpec
{
    const char* label;        // short English label
    const char* key;          // ini key
    int ctlField;             // ctl v3 field id, -1 = local only
    Ctrl  type;               // how to render
    float lo, hi, def;        // slider bounds / combo index range
    const char* const* items; // combo items (nullptr except Ctrl::Combo;
                              // gpuIndex items are enumerated at runtime)
    const char* what;         // WHAT IT DOES — always visible under control
    const char* sends;        // WHAT IS ACTUALLY SENT (field, when, cost)
    const char* deviate;      // SIDE EFFECT OF DEVIATING
};

inline constexpr const char* kBackendItems[] = { "DLSS", "FSR2 (planned)",
                                                 "XeSS (planned)" };
inline constexpr const char* kFgItems[]      = { "Off", "DLSS-G (planned)",
                                                 "FSR-FG (planned)" };
inline constexpr const char* kNrStyleItems[] = { "0 Default", "1 Alternate",
                                                 "2 Extra" };
inline constexpr const char* kTintItems[]   = { "0 Off (quality)",
                                                "1 NR output view" };

inline constexpr ControlSpec kSpecs[] =
{
    // ---- Upscaling -------------------------------------------------------
    { "Upscaler backend", "backend", -1, Ctrl::Combo, 0, 2, 0,
      kBackendItems,
      "Selects which reconstruction backend the host drives.",
      "Nothing sent today — the choice is saved to sli.ini (backend=) but "
      "only DLSS exists; the value waits for a second backend.",
      "No effect today: only DLSS is implemented; FSR2/XeSS are slots." },
    { "Offload to second GPU", "offloadOn", CTL_FIELD_OFFLOAD,
      Ctrl::Checkbox, 0, 1, 1, nullptr,
      "Runs the neural denoise pass on a second GPU: the finished frame "
      "travels there, the NR model cleans it, and the correction comes "
      "back composed onto the SAME frame before it is shown. The game's "
      "own DLSS upscaling always runs on the game's GPU — off is a live "
      "A/B back to the fully native path.",
      "OFFLOAD 1/0 live, effective the next frame.",
      "Off also disables the neural pass (it rides the offload); toggling "
      "mid-Apply is safe." },
    { "Offload GPU", "gpuIndex", CTL_FIELD_GPU_INDEX, Ctrl::Combo,
      -1, 7, -1, nullptr,
      "Chooses which GPU runs the offload pass (skipping the game's own).",
      "GPU_INDEX (-1 = auto, first non-game NVIDIA); a change re-arms the "
      "transport.",
      "A low-VRAM or mixed-driver GPU fails to arm; switching re-arms "
      "(~2 s)." },
    // (R82i.5: the dead game-override rows are GONE — sharpness, MV scale,
    //  jitter, pre-exposure, exposure scale, MV offset, frame time. None was
    //  ever consumed by the host/engine; the game's own values always won.
    //  Their ctl ids stay documented holes in ctl_common.h, never reused.)

    // ---- Super Resolution ---------------------------------------------------
    // (R84.1: the srBypass/srRatio rows are GONE — dead in RDR2, removed
    //  with their whole chain; R85 supersedes them with real SR backends.
    //  Their ctl ids stay documented holes, never reused.)

    // ---- Neural Rendering ------------------------------------------------
    { "Neural rendering", "nrOn", CTL_FIELD_NR_ON, Ctrl::Checkbox,
      0, 1, 0, nullptr,
      "Runs the NR model (DLSS-NR) on the offload GPU over the game's "
      "finished (upscaled) frame; the correction is composed onto that "
      "same frame inside Present — every shown frame carries its own "
      "denoise.",
      "NR_ON 1/0; ON builds the feature (~1 s first frame), OFF stops "
      "composing instantly.",
      "OFF = no neural correction (the base comparison image)." },
    { "NR style", "nrStyle", CTL_FIELD_NR_STYLE, Ctrl::Combo,
      0, 2, 0, kNrStyleItems,
      "Variant of the NR model.",
      "NR_STYLE (model rebuild ~1 s).",
      "Alternate styles are not guaranteed better; experimentation only." },
    { "Perf quality", "nrPerfQuality", CTL_FIELD_NR_PERFQ, Ctrl::SliderF,
      0.0f, 4.0f, 2.0f, nullptr,
      "The vendor model's effort dial (NVSDK PerfQualityValue). Scale: "
      "0 = max speed, 1 = balanced, 2 = max quality (DEFAULT — what has "
      "always run), 3 = ultra perf, 4 = ultra quality. It tells the model "
      "how hard to work, paired with the Scaling pin valve below.",
      "NR_PERFQ always; a CHANGE rebuilds the model (~1 s) — create-time "
      "parameter.",
      "With the pin open (below), lowering this is where the engine "
      "milliseconds actually drop. With the pin at 1.0 this dial is "
      "DECORATIVE — the full-res pin cancels it (golpe 1b finding)." },
    { "Scaling pin", "nrRatioPin", CTL_FIELD_NR_RATIOPIN, Ctrl::SliderF,
      0.0f, 1.5f, 0.0f, nullptr,
      "The model's work-resolution valve (DLSSNR.ScalingRatio). "
      "0 = OPEN (default, golpe 1b): the model picks its own interior "
      "resolution from Perf quality — this is where the speed lives. "
      "1.0 = classic: forced full resolution, the historical behavior "
      "that silently cancelled Perf quality.",
      "NR_RATIOPIN always; a CHANGE rebuilds the model (~1 s).",
      "0 + Perf quality 2 should look the same as 1.0 (model picks full "
      "res at max quality). 0 + Perf 0/3 is the fast path — watch the "
      "split view for softness." },
    { "Depth source", "nrDepthMode", CTL_FIELD_NR_DEPTHMODE, Ctrl::Checkbox,
      0.0f, 1.0f, 0.0f, nullptr,
      "UNCHECKED (0) = REAL: the game's own depth buffer travels to the "
      "model every frame (the correct guide for its temporal logic; menus "
      "without a depth buffer fall back to a flat near plane "
      "automatically). CHECKED (1) = flat proxy always (the historical "
      "behavior before this knob existed).",
      "NR_DEPTHMODE live — takes effect on the next frame, no rebuild.",
      "This is the golpe 2 lever: real depth should hold detail better in "
      "motion. If the image degrades with it on, uncheck and report — that "
      "would be a finding, not a setting." },
    { "Residual compose", "nrResidual", CTL_FIELD_NR_RESIDUAL, Ctrl::Checkbox,
      0.0f, 1.0f, 1.0f, nullptr,
      "CHECKED (1, DEFAULT): when the model works at reduced "
      "resolution (Work scale < 1), only its EDIT travels up to the full "
      "frame — the blur of the reduction cancels out instead of being "
      "misread as brightness headroom (the resolution-dependent colour "
      "shift measurable in reference forks). UNCHECKED (0) = classic "
      "compose (only for A/B).",
      "NR_RESIDUAL live — takes effect on the next frame, no rebuild.",
      "Inert at Work scale 1.0 (the arithmetic collapses — nothing to "
      "carry). The speed does not change; this is a CORRECTNESS knob." },
    { "Intensity", "nrIntensity", CTL_FIELD_NR_INT, Ctrl::SliderF,
      -1.0f, 2.0f, -1.0f, nullptr,
      "Global strength of the neural renderer. Always-value knob: sent on "
      "every Apply. -1 = the vendor's own default (the host's convention).",
      "NR_INT always (model rebuild ~1 s).",
      "High = plastic over-smoothing; low = barely any correction." },
    { "Local structure", "nrLocalStructure", CTL_FIELD_NR_LS, Ctrl::SliderF,
      -1.0f, 2.0f, -1.0f, nullptr,
      "Local detail and structure strength. -1 = vendor default.",
      "NR_LS always (model rebuild ~1 s).",
      "High = drawn-looking textures; low = flat detail." },
    { "Local tone", "nrLocalTone", CTL_FIELD_NR_LT, Ctrl::SliderF,
      -1.0f, 2.0f, -1.0f, nullptr,
      "Local tone and contrast strength. -1 = vendor default.",
      "NR_LT always (model rebuild ~1 s).",
      "High = heavy HDR look; low = a flat image." },
    { "Skin structure", "nrSkinStructure", CTL_FIELD_NR_SS, Ctrl::SliderF,
      -1.0f, 2.0f, -1.0f, nullptr,
      "Skin detail strength. -1 (default) follows Local structure — both "
      "this knob and the vendor treat -1 that way.",
      "NR_SS always (model rebuild ~1 s).",
      "High = visibly fake skin texture." },
    { "Work scale", "nrWorkScale", CTL_FIELD_NR_WORKSCALE, Ctrl::SliderF,
      0.25f, 2.0f, 1.0f, nullptr,
      "Resolution the NR model runs at, as a fraction of the frame the "
      "offload transports. 1.0 = the game's finished (upscaled) frame "
      "at full size; 0.5 = a quarter of the pixels.",
      "NR_WORKSCALE always; a change re-arms the engine (~2 s).",
      "1.0 = full fidelity, heaviest on the second GPU. Below 1 = more "
      "FPS, softer model raster (the view shows the model's real "
      "resolution). Above 1 never adds detail — no supersample." },
    { "Boost (gain^x)", "nrBoost", CTL_FIELD_NR_BOOST, Ctrl::SliderF,
      0.0f, 4.0f, 1.0f, nullptr,
      "Exponent on the NR gain when composing it back.",
      "NR_BOOST always, next frame (no re-arm).",
      "1.0 = exact. 2-3 = amplified A/B (g^2). 0 = edit visually OFF "
      "(neutral gain); the identity stays bit-exact at any value." },
    { "Detail strength", "nrDetail", CTL_FIELD_NR_DETAIL, Ctrl::SliderF,
      0.0f, 4.0f, 1.0f, nullptr,
      "How much of the model's luminance verdict rides on top of the "
      "frame. The size of the edit itself, separate from the boost's "
      "exponent.",
      "NR_DETAIL, live every frame.",
      "1.0 = exact (the model's own intent). 0 = the model's luminance "
      "edit is OFF (clean baseline A/B). Above 1 amplifies the verdict — "
      "detail AND model noise alike." },
    { "Colour strength", "nrColour", CTL_FIELD_NR_COLOUR, Ctrl::SliderF,
      0.0f, 2.0f, 0.25f, nullptr,
      "How much of the MODEL'S OWN colour shift passes into the frame. "
      "The reference forks ship this at ~0 — their measured verdict is "
      "that model colour shifts are the part you do not want.",
      "NR_COLOUR, live every frame.",
      "0.25 = our default. 0 = colour strictly from the game (safest). "
      "Above ~0.5 the model starts repainting hues — judge on skin and "
      "night skies." },
    { "Auto skin mask", "nrAutoSkin", CTL_FIELD_NR_AUTOSKIN, Ctrl::Checkbox,
      0, 1, 1, nullptr,
      "Lets the model find skin itself rather than treating the frame "
      "uniformly (the core's UseAutoMask flag). Pairs with Skin structure "
      "below/above: the mask decides WHERE the skin treatment applies.",
      "NR_AUTOSKIN rides the next evaluate call (no re-arm).",
      "1 = what this proxy always sent (model decides). 0 = uniform "
      "treatment — skin texture may come out smoother or plastically "
      "denoised in close-ups." },
    { "Per-pixel gain bound", "nrGainBound", CTL_FIELD_NR_HIGUARD,
      Ctrl::SliderF,
      1.0f, 8.0f, 2.0f, nullptr,
      "How far the NR edit may push any single pixel's light, at most, "
      "relative to the frame it lands on (the composition clamp, per "
      "pixel — the MaxRatio guard reference implementations carry). "
      "Bounds a wrong or extrapolated model verdict; it does not add "
      "anything. The former global 'highlight knee' (x1.12 on bright "
      "originals) was replaced by this: the knee was a global patch for "
      "a per-pixel question.",
      "NR_HIGUARD (field kept, semantics renamed), live every frame.",
      "2.0 = the reference default. 1.0 = edit off everywhere (every ratio "
      "clamped to 1). 8.0 = almost no bound: strong verdicts pass whole, "
      "and stale-history ghosting prints at full strength — the ghosting "
      "A/B showed exactly this." },
    { "History reset shift (%)", "nrFlowReset", CTL_FIELD_NR_FLOWRESET,
      Ctrl::SliderF,
      0.0f, 50.0f, 6.0f, nullptr,
      "Auto model-history reset: when the scene moved more than this "
      "percent of the frame height since the previous frame (85th "
      "percentile of the per-tile motion peak, 1-frame lag), the model "
      "history resets BEFORE the edit composes — the proven ghosting "
      "cure (stale history was printing the displaced copy). At high "
      "fps the per-frame shift stays far below the threshold, so this "
      "never fires — the expected behaviour. 0 = off.",
      "NR_FLOWRESET, live every frame (signal arrives 1 frame late by "
      "design: in-band on the existing tile readback).",
      "A reset frame denoises cold (slightly weaker NR for that frame) "
      "— the trade for never printing a displaced copy. Sustained pans "
      "trip it every frame at low fps; that is exactly the ghost case." },

    { "Gate wait cap (ms)", "nrGateWaitMs", CTL_FIELD_NR_GATEWAIT,
      Ctrl::SliderI,
      0, 10000, 0, nullptr,
      "How long the frame on screen waits for ITS OWN neural delta "
      "before giving up. 0 = INFINITE: the wait is unbounded — every "
      "presented frame carries its own NR result (maximum fidelity; the "
      "fps floor is whatever the engine delivers — this is the default "
      "and what the game has always run). A cap > 0 (e.g. 100-300) "
      "presents that frame NATIVE when the delta is late: image keeps "
      "flowing at the game's own pace, and NR coverage dips on exactly "
      "the frames the engine missed — never a stale or mixed result, "
      "and the session recovers by itself (no re-arm). The host clamps "
      "any cap below 20 ms up to 20: waits shorter than one engine "
      "frame would fire native every frame.",
      "NR_GATEWAIT, live (host-side wait, no re-arm, no engine restart). "
      "Slider values 1-19 arrive but the host floors them at 20.",
      "The trade is honest: infinite = total NR fidelity at the engine's "
      "fps; capped = smooth cadence with occasional native frames. When "
      "a frame goes native you may see a one-frame clarity dip — that "
      "is the un-denoised frame, not a bug." },

    // ---- Debug & Verification --------------------------------------------
    // (R89b: the Capture-frames row is GONE — the host-side capture flag
    //  had no consumer; ctl id 3 is a documented hole.)
    { "Reset history every frame", "forceReset", CTL_FIELD_FORCERESET,
      Ctrl::Checkbox, 0, 1, 0, nullptr,
      "Discards the DLSS temporal history every frame (no accumulation).",
      "FORCERESET 1/0.",
      "Shows the pure per-frame SR contribution; keep OFF in normal use." },
    { "NR output view", "nrTint", CTL_FIELD_NR_TINT, Ctrl::Combo,
      0, 1, 0, kTintItems,
      "The screen shows the NR model's PURE output (post tile-median), so "
      "you can see WHERE and HOW it is acting — no eye-comparing "
      "subtleties. With the split enabled, the left of the seam is the "
      "model view and the right stays the game's untouched frame.",
      "NR_TINT 0/1 live, no re-arm; the draw goes OPAQUE (the payload "
      "replaces the frame) and the seam becomes a scissor cut.",
      "Visualization only: the quality correction is still computed and "
      "its calibration (tiles) keeps updating. Turn OFF for the real "
      "image." },
    { "Test split A/B", "nrTestSplit", CTL_FIELD_NR_SPLIT, Ctrl::SliderI,
      0, 1000, 0, nullptr,
      "Checkbox enables the split; the slider positions the seam across "
      "the screen (permille of width; 500 = centre) and is remembered "
      "while off. LEFT of the seam gets the NR edit, RIGHT keeps the "
      "game's untouched output.",
      "NR_TESTSPLIT 0-1000 live, no re-arm (0 = off, 500 = centre).",
      "Side-by-side comparison view; a thin divider marks the seam. Move "
      "the seam onto the subject (e.g. a face) to judge fidelity there. "
      "Uncheck for full-frame NR." },

    // ---- Frame Generation (placeholder tab) -------------------------------
    { "Frame generation", "fgMode", -1, Ctrl::Combo, 0, 2, 0,
      kFgItems,
      "Reserved for frame interpolation inside the offload.",
      "Nothing yet — the slot exists so the pipeline shape is visible.",
      "No effect today." },
};

inline constexpr int kNumControls =
    sizeof(kSpecs) / sizeof(kSpecs[0]);

// constexpr key comparison (strcmp is not a constant expression).
constexpr bool KeyEq(const char* a, const char* b)
{
    while (*a != 0 && *b != 0 && *a == *b) { ++a; ++b; }
    return *a == 0 && *b == 0;
}

constexpr int KeyIdx(const char* key)
{
    for (int i = 0; i < kNumControls; ++i)
        if (KeyEq(kSpecs[i].key, key)) return i;
    return -1;
}

constexpr int IdOf(int ctlField)
{
    for (int i = 0; i < kNumControls; ++i)
        if (kSpecs[i].ctlField == ctlField) return i;
    return -1;
}

// Table sanity: duplicate keys or duplicate ctl fields would silently
// split-brain the ini and the Apply batch — both are compile errors here.
constexpr bool TableIsSane()
{
    for (int i = 0; i < kNumControls; ++i)
    {
        if (kSpecs[i].key == nullptr || kSpecs[i].label == nullptr)
            return false;
        for (int j = i + 1; j < kNumControls; ++j)
        {
            if (KeyEq(kSpecs[i].key, kSpecs[j].key)) return false;
            if (kSpecs[i].ctlField > 0 &&
                kSpecs[i].ctlField == kSpecs[j].ctlField) return false;
        }
    }
    return true;
}
static_assert(TableIsSane(), "ControlSpec table: duplicate key or field");
static_assert(IdOf(CTL_FIELD_OFFLOAD) >= 0 &&
              IdOf(CTL_FIELD_GPU_INDEX) >= 0 &&
              IdOf(CTL_FIELD_NR_WORKSCALE) >= 0 &&
              IdOf(CTL_FIELD_NR_TINT) >= 0,
              "ControlSpec table: a required field is missing");
static_assert(KeyIdx("backend") >= 0 && KeyIdx("fgMode") >= 0,
              "ControlSpec table: local-only rows missing");

// Every ctl-mapped row must use the exact ini key the host's CtlLoadIni
// reads (Save writes the file the host will reload at (re)arm).
static_assert(KeyIdx("offloadOn") == IdOf(CTL_FIELD_OFFLOAD) &&
              KeyIdx("gpuIndex") == IdOf(CTL_FIELD_GPU_INDEX) &&
              KeyIdx("nrOn") == IdOf(CTL_FIELD_NR_ON) &&
              KeyIdx("nrStyle") == IdOf(CTL_FIELD_NR_STYLE) &&
              KeyIdx("nrIntensity") == IdOf(CTL_FIELD_NR_INT) &&
              KeyIdx("nrLocalStructure") == IdOf(CTL_FIELD_NR_LS) &&
              KeyIdx("nrLocalTone") == IdOf(CTL_FIELD_NR_LT) &&
              KeyIdx("nrSkinStructure") == IdOf(CTL_FIELD_NR_SS) &&
              KeyIdx("nrWorkScale") == IdOf(CTL_FIELD_NR_WORKSCALE) &&
              KeyIdx("nrBoost") == IdOf(CTL_FIELD_NR_BOOST) &&
              KeyIdx("nrTint") == IdOf(CTL_FIELD_NR_TINT) &&
              KeyIdx("nrTestSplit") == IdOf(CTL_FIELD_NR_SPLIT) &&
              KeyIdx("forceReset") == IdOf(CTL_FIELD_FORCERESET) &&
              // (R95: the nrPreSr row is gone with the pre-SR mode.)
              KeyIdx("nrDetail") == IdOf(CTL_FIELD_NR_DETAIL) &&
              KeyIdx("nrColour") == IdOf(CTL_FIELD_NR_COLOUR) &&
              KeyIdx("nrAutoSkin") == IdOf(CTL_FIELD_NR_AUTOSKIN) &&
              KeyIdx("nrGainBound") == IdOf(CTL_FIELD_NR_HIGUARD) &&
              KeyIdx("nrFlowReset") == IdOf(CTL_FIELD_NR_FLOWRESET) &&
              KeyIdx("nrGateWaitMs") == IdOf(CTL_FIELD_NR_GATEWAIT) &&
              KeyIdx("nrPerfQuality") == IdOf(CTL_FIELD_NR_PERFQ) &&
              KeyIdx("nrRatioPin") == IdOf(CTL_FIELD_NR_RATIOPIN) &&
              KeyIdx("nrDepthMode") == IdOf(CTL_FIELD_NR_DEPTHMODE) &&
              KeyIdx("nrResidual") == IdOf(CTL_FIELD_NR_RESIDUAL),
              "ctl rows must keep the host-side ini key names");

// ---------------------------------------------------------------------------
// Panel state. Three snapshots drive the divergence indicator:
//   value   = what the UI shows (would be sent on Apply)
//   applied = what the last successful Apply sent
//   saved   = what sli.ini holds (restored at startup)
// ---------------------------------------------------------------------------
struct PanelState
{
    float value[kNumControls];
    float applied[kNumControls];
    bool  appliedValid = false;
    // (R95 audit: saved[]/savedValid/DiffersFromSaved removed — written by
    //  LoadIni/SaveIni, never read; the header's "three snapshots" story
    //  overstated what the code used. The divergence indicator is
    //  value-vs-applied, which is live.)

    bool DiffersFrom(const float* v) const
    {
        for (int i = 0; i < kNumControls; ++i)
            if (value[i] != v[i]) return true;
        return false;
    }
    // (R95 audit: DiffersFromSaved() removed — zero callers since the
    //  R82l override-machinery removal; the divergence indicator uses
    //  DiffersFrom(applied), which is live.)
};

extern PanelState g_st;
void SetDefaults();

// Value a row sends on Apply: every control is ALWAYS-VALUE (sent as-is;
// the fork's value_or_default semantics). Kept as a function so the send
// path has exactly one definition point.
float SendValue(int i);

// ---------------------------------------------------------------------------
// Host view: everything polled from the host, refreshed every 500 ms.
// ---------------------------------------------------------------------------
struct GpuItem
{
    // (R89b: luidLo/luidHi were write-only — the combo shows the name and
    //  the index; nothing read the LUID. Removed.)
    char name[128] = {};
};

struct HostView
{
    // ctl mapping
    HANDLE map = nullptr;
    volatile sli::CtlMsg* msg = nullptr;
    bool   incompatible = false;   // wrong magic: read-only banner
    uint32_t hostVersion = 0;
    bool   hostLive = false;       // heartbeat fresh (< 2 s)
    uint32_t enginePid = 0;
    unsigned long long gameLuidLo = 0, gameLuidHi = 0;

    // outstanding PING (verb get): non-blocking ack watch
    bool    pingPending = false;
    uint32_t pingSeq = 0;
    ULONGLONG pingSentAt = 0;
    DWORD   pingMs = 0;            // last measured round-trip

    // mirror copy (the truth the UI displays). Sized by CTL_FIELD_COUNT:
    // R82k.7 sized CtlMsg by COUNT but this copy stayed [32], so the
    // readouts of fields 33-36 (detail/colour/autoskin/higuard) read
    // trampled memory — the panel calibrated A/Bs against fake values.
    float mirror[sli::CTL_FIELD_COUNT] = {};
    uint32_t mirrorSource[sli::CTL_FIELD_COUNT] = {};

    // frame mapping (per-frame scalars): engine counters + the game's live
    // inputs (jitter/MV/pre-exposure as fed to the engine)
    HANDLE frameMap = nullptr;
    volatile sli::FrameScalars* scalars = nullptr;
    bool  countersValid = false;
    uint64_t produceFrame = 0, doneFrame = 0, prevDoneFrame = 0;
    bool  engineLive = false;      // engineResult==1 && done advancing
    bool  wasLive = false;         // transition edge for the event log

    // GPU combo items
    GpuItem gpus[8];
    int gpuCount = 0;
};

extern HostView g_host;

// ---------------------------------------------------------------------------
// Event log: last N ctl operations with ack state (Debug tab). Ring buffer,
// newest first; bounded so a dead host cannot grow it forever.
// ---------------------------------------------------------------------------
constexpr int kLogCap = 48;
struct LogEvent
{
    ULONGLONG tick = 0;  // GetTickCount64 (wraps at 49.7 days on DWORD)
    char  text[160] = {};
    bool  bad = false;   // ack failure / timeout — rendered red
};
extern LogEvent g_events[kLogCap];
extern int g_eventCount;

// A leading '!' in fmt marks a failure (rendered red in the log).
void Event(const char* fmt, ...);

// ini persistence (panel_state.cpp): sli.ini beside the panel exe,
// section [sli] — the exact file the host's CtlLoadIni reads.
void BuildIniPath();
void LoadIni();
void PanelReloadIniOnActivate();   // R89e: WM_ACTIVATE reloads sli.ini
bool SaveIni();  // false = a write failed (surfaced in the event log)
// GPU enumeration (panel_host.cpp): NVIDIA adapters only (VendorId
// 0x10DE), skipping the game's adapter when its LUID is known.
void EnumGpus();

// ctl client (panel_host.cpp). All calls run on the UI thread — the panel
// is a single ctl writer, so seq/ack never interleave.
bool OpenCtl();
void CloseCtl();
bool OpenFrameScalars();
void PollHostIfDue(ULONGLONG& nextPoll);
void ApplyAll();

// UI (panel_ui.cpp).
const char* Dot(bool on);
extern const char* kWarnMark;
void MutedText(const char* fmt, ...);
void DrawHelp(const ControlSpec& s);
void DrawControl(int i);
void DrawHeader();
void DrawUpscalingTab();
void DrawFgTab();
void DrawNrTab();
void DrawDebugTab();
void DrawAboutTab();
