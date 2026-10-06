> Consolidated from the POC skill's validated recipe (rounds R65-R69,
> 2026-09-25 → 09-26). This is the operative document for running the vendor
> DLSS-NR runtime in-process on the engine GPU. Names refer to POC binaries
> (coproc_engine); new-sli's sli_engine implements the same contract.

# Vendor DLSS-NR runtime in-process — validated recipe (R66–R68)

How to run NVIDIA's own DLSS-NR model (nvngx_dlssnr.dll) inside YOUR OWN
process — standalone stub or `coproc_engine` — on Ampere (RTX 3060).
Executable reference: the DLSS-POC stub repo (`--step nr --via-fwd`);
engine integration: the POC's `engine/nr_vendor.h` (nrEngine=1).

## Files needed in the working dir

- `nvngx_dlssnr.dll` — the runtime. Distinguish by HASH under the same
  filename: cross-gen builds with sm_86 cubins infer on 3060; verify md5
  before diagnosing anything.
- `nvngx.dll_dlssnr.dll` — the forwarder shim. Its path contains
  "nvngx.dll", which is what satisfies the runtime's caller gate.
- The driver core resolves via registry
  `HKLM\System\CurrentControlSet\Services\nvlddmkm\NGXCore\NGXPath`
  → `<path>\_nvngx.dll`.

## Init sequence (all steps rc=1)

1. `LoadLibraryExW(core\_nvngx.dll, LOAD_WITH_ALTERED_SEARCH_PATH)`.
2. `NVSDK_NGX_D3D12_Init_Ext(appId, dataPath=cwd, device, sdk=0x15, nullptr)`
   — game's real app-id works; the forwarder uses its own for the snippet.
3. `NVSDK_NGX_D3D12_GetCapabilityParameters(&P)` — the DRIVER's capability
   block, NOT a fresh AllocateParameters block.
4. Discover the block's float vtable slot by round-trip probe
   (write sentinel via candidate setter s, read via getter s+8).
5. Tell the forwarder: `dlssnr_call_set_float_slot(slot)` — the forwarder
   writes floats through its OWN hardcoded slot (default 1); without this
   its tuning writes land on the wrong vtable entry and read 0.0 in the
   model.
6. Write ALL tuning into P BEFORE create: dims (DLSSNR.Width/Height),
   preset, Intensity/Style/Local*/Skin*, ControlMask=null, UseAutoMask,
   PerfQualityValue, ScalingRatio, UI/UIAlpha/Backbuffer=null, Reset=1.
   The model latches tuning ONCE, at create — evaluate-only writes are
   ignored.
7. `dlssnr_call_create(snippetPath, dataPath, dev, cmd, P, w, h, preset,
   intensity, style, structure, tone, skin, autoMask, uiCorrection)` →
   feature-18 handle. Submit+wait the cmd list right after (create records
   into it).
8. Per frame: `dlssnr_call_evaluate_v2(cmd, feature, P, color, depth, mv,
   output, dims…, depthInverted=1, reset, tuning…, mvScaleX, mvScaleY)`.

## The inference gate: DEPTH (the whole mystery was this)

Without a `DLSSNR.Depth` resource the runtime does SAFE PASSTHROUGH: create
succeeds, weights load (153 raw tensors, ~141 MB heap), but CG2RKernelManager
registers only PostProcess+Copy and the output is a BIT-EXACT copy of the
input. Hash-identical output is NOT evidence of support.

Validated guide set: depth = R32 texture filled with constant 0.5,
`DepthInverted=1`; motion = R16G16 filled with zeros (works; a MISSING mv
texture fastfails the process). Static synthetic guides are enough for the
network to engage.

## Format contract (sm_86)

| Channel | Format | Verdict |
|---|---|---|
| color in | BGRA8 sRGB | works |
| color in | RGBA16F linear | __fastfail 0xC0000409 (both contracts) |
| output | RGBA16F | required — values in the SAME sRGB-encoded domain as the input, no gamma in between |
| output | BGRA8 | silent passthrough (bit-exact copy) |
| depth | R32 (TYPELESS tex, R32_FLOAT footprint) | 0.5 constant + DepthInverted=1 |
| mv | R16G16_FLOAT | zeros |

The model is trained on finished sRGB frames. Feeding linear HDR requires an
encode/decode codec around the model (the fork's white-point meter codec, or
median-exposure 0.18 → Reinhard → 1/2.2 and its inverse — the TRT runner's
codec ports cleanly to C++).

## Pitch rule (recurring E_INVALIDARG source)

D3D12 footprints demand RowPitch 256-aligned — `w*bpp` only counts when it
happens to be a multiple of 256 (853×8=6824 is NOT; use 6912). Size EVERY
staging buffer as alignedPitch×h (not w×bpp×h) and fill row-by-row. Applies
to upload (guides, color), readback AND one-off diagnostic dump buffers
alike. Two failure signatures: CopyTextureRegion overreads the buffer →
Close() fails with E_INVALIDARG naming nothing; a READBACK footprint with
unaligned RowPitch kills the device SILENTLY → the engine hangs after frame
1 with zero error lines (main thread sleeping on produce, host still
producing — see the hung-engine signature in `live-ab-sessions.md`).

## Measurement pitfalls

- A stub instrumented with per-frame readback+PNG measures ~150 ms @1280×720
  while the model itself costs ~28 ms at comparable area — strip the
  instrumentation before quoting throughput.
- In-engine vendor frames: decompose model vs codec. At 853×360 the model
  was ~28 ms and the CPU HDR codec ~170 ms — the codec dominates; move the
  codec to GPU before condemning the backend.
- Native-fps reference is SCENE-DEPENDENT (light camp scene: 60 fps; heavy
  scene: ~12) — always re-measure the A/B native arm in the same scene and
  session as the offload arm.

## In-engine selection

`coproc.ini` → `nrEngine`: 0 = TRT runner out-of-process (python),
1 = vendor in-process. The host reads the ini, writes it into the tuning
block, and skips spawning the python runner in vendor mode. Same delay-line
of 1 frame, same badge, same bypass-eco fallback when vendor init fails.

## GPU codec — one command session per frame (R68)

The CPU codec (~170 ms of the 200 ms frame) ports to compute shaders: encode
(linear HDR RGBA16F → BGRA8 sRGB, median-exposure 0.18 → Reinhard → 1/2.2) and
decode (model RGBA16F output → transport texture + watermark). Per-frame flow
records into ONE cmd list:

1. SRV(texInColor) → CS encode → UAV(texColor BGRA8) + RWStructuredBuffer of
   8×8 tile luminances (f16 pair packed in a uint).
2. Copy tiles → readback buffer for NEXT frame's exposure (lag 1 frame); UAV
   barrier on texColor → SRV state before the vendor evaluate.
3. Vendor evaluate (the runtime records its own barriers/heaps into the list).
4. RE-BIND the codec descriptor heap before the decode dispatch — the
   runtime's evaluate may swap the cmd list's heaps, and the decode then
   dispatches against garbage.
5. CS decode → UAV(transport texOut) + badge. Zero CPU round-trips.

View rules (each a silent deferred device-kill — see the D3D12 device-removed
attribution rule in SKILL.md): structured-buffer UAV = `Format=UNKNOWN` +
`StructureByteStride=4`; UAV only onto resources created with
`ALLOW_UNORDERED_ACCESS` (a transport texture created depth-stencil-only
cannot take a UAV — decode into a local UAV texture and copy). BGRA8 UAV
writes are supported (verify once via CheckFeatureSupport).

### Porting the CPU codec to HLSL: typed views swizzle BY NAME

The CPU codec wrote BGRA by BYTES (`dst[x*4+(2-c)]` — byte addressing, the
index math IS the swizzle). A `RWTexture2D<unorm float4>` over a BGRA8
resource maps components by NAME (.r writes the R channel; the format
mapping lives in the VIEW). Copying the byte-order swizzle into the shader
(`float4(d.b,d.g,d.r,1)`) applies a SECOND flip = net R↔B on the whole
frame: skins go blue (the canary — human vision flags blue skin where it
accepts swapped skies/clothes) and the model dutifully processes the
pre-swapped input, so the bug reads as "the model tints skins". Write
`float4(d,1)` and let the view own the format mapping. Rule: byte-order
swizzles from CPU codec code must die at the HLSL boundary.

## Diagnosing color/tint artifacts — one-shot 3-point dump

Don't re-read shader math when a tint appears at the end of the pipeline:
instrument a ONE-SHOT dump at each stage boundary (model input / model
output / final output) at a fixed frame number — readback buffers created
at init (256-aligned pitch!), CopyTextureRegion inside the frame's own cmd
session, files written to disk once (`dumpTaken` flag; e.g.
`vendor_dump_{in,model,final}.rgba16f` in the engine's cwd). Compare
per-channel means in an artifact region (a skin patch) offline with numpy:
uint16 raw → reshape (h, pitchBytes/2) → slice `[:, :w*4]` → float16 view.

Reading the signature: IN clean but MODEL already wrong = the bug is in the
ENCODE path (the model saw bad input — exonerates the model); FINAL = IN
with two channels exchanged = a net swizzle wrapped around the model
(double flip; see the typed-view rule above). This closes attribution in
one captured frame instead of source-reading both shaders.

The same 3-point dump quantifies the model's CONTRIBUTION when a user
reports "toggling NR does nothing": correlate the model stage against the
input — corr ≈ 1.0 with an affine fit of a≈1, b≈0 means passthrough
(synthetic guides, static camera, or a scene with no noise to remove); a
materially lower corr with large per-channel deltas means engaged. Never
eyeball the toggle — correlate the dump, and remember a menu/static scene
makes even a healthy model a near-identity.

## Real game guides (R69) — transport, the R32G32_SINT wall, per-guide A/B

Transport: shared buffers (depth+MV × triple-buffer, sealed per frame next
to the lag buffer, footprint with the game's REAL format and 256-aligned
pitch — NOT the FLOAT-family mapping: a FLOAT footprint describing SINT bits
is an invalid copy that lands as silent zeros, see the sealing rules below);
the engine copies buffer→local texture (COMMON→COPY_DEST→SRV) and evaluates
frame N with guides from N−1 — same timing as the native NGX patch. Raw
dims/formats/pitches/depthInverted (from the game's create-flags) travel in
the handshake; changing the handshake is an ABI break: the game must be
restarted.

## Seal placement & the delivery gate (R69 closure)

Seal placement: record the guide seal in a PRIVATE command list submitted to
the same DIRECT queue immediately AFTER the game's frame list (from the
ExecuteCommandLists hook), never inside the game's own list — the native NGX
runtime records its own transitions into the game's list, and barriers over
game textures in unknown states surface minutes later as deferred device
death (ERR_GFX_D3D_DEFERRED_MEM / EMP.dll / nvwgf2umx AVs, typically during
loading). Two structural companions: a fresh command list is BORN RECORDING —
Close it empty at creation or the first Reset fails silently forever; and
the submit hook must run WITHOUT the frame mutex (the done-wait holds it —
a hook that needs it deadlocks the game's submit thread).

Delivery gate: deliver when the producer's fence passed the LAST DELIVERED
completion (`done > lastDeliveredDone`), never when it catches the CURRENT
pending frame — pending is overwritten every evaluate, so a slower producer
makes that condition permanently false: delivery never runs, the consumer
flag never sets, the native SR keeps receiving the game's own color =
perpetual echo (no badge, A/B toggle dead) while the engine log shows healthy
ENHANCED frames the whole time. Fingerprint: `result not ready / delivering
late` lines with a GROWING done-vs-pending gap, zero deliver-failures.

Diagnostic chain for 'user sees no NR / toggle does nothing, both GPUs at
100%': (1) slice the host log at the last session marker and read the
done-vs-pending trend in the late lines; (2) correlate the 3-point dump —
corr(model, encode(input)) ≈ 0.9995 means the model is PASSIVE (with stub
guides it is near-identity by design; the GPU load is the engine grinding
the passive path); (3) screenshot + vision-zoom the top-left corner for the
badge ON SCREEN — the in-dump badge (decode writes 512.0 at pixels 16..24)
proves only the engine chain. GPU load proves work, not effect.

## Guide-sealing copy rules (the all-zeros hunt, R69c)

Guides arriving as ALL ZEROS while color flows through the SAME seal
function means the guide copies are invalid, not that the game sends zeros.
Three chained bugs, each silent (D3D12 copies record no HRESULT — they fail
at Close/execute or no-op):

1. `CopyResource` accepts only IDENTICAL formats (or typeless→typed twin).
Cloning the game's typed R32G32_SINT depth to a FLOAT-family twin (the
`FootprintFmt(TypedGuideFmt(...))` recipe) is invalid → zeros. Keep any
clone's format IDENTICAL to the source; typed formats pass through raw.
1b. THE HARD WALL (per-guide A/B verdict): CopyTextureRegion/CopyResource
PROHIBIT a SOURCE created with ALLOW_DEPTH_STENCIL — the game's real depth
(RDR2 flags 0x2) cannot be sealed by copy AT ALL. Symptom chain: the seal
list's Close fails E_FAIL; a rebuild-every-frame loop around it ends in a
driver crash (nvwgf2umx access violation). The MV (ALLOW_RENDER_TARGET 0x1)
seals stably for thousands of frames with the game alive. This is WHY no
reference implementation copies the game's depth — OptiScaler/fork/Streamline
pass the texture direct to the runtime on the same GPU; when NO reference
does X with game resources, check the API spec before engineering X. Working
shape: seal MV only; depth = engine stub (`coproc_stubguides.on` containing
'd'); real-depth capture = a read-only compute shader sampling the DSV
texture via SRV, never a copy.
2. EVERY guide source needs its own barrier to COPY_SOURCE before the copy
(and back to SRV after). The MV was copied untransitioned → zeros. The
color path had it; per-guide code paths don't inherit it.
3. A PLACED_FOOTPRINT must describe the buffer with the SAME format as the
texture on the other side. The engine's copyGuide used the FLOAT-family
footprint over SINT-typed textures → invalid. The rule binds BOTH copies of
the transport: the host's seal into the shared buffer carried the same
FLOAT-family mapping and kept delivering zeros for several rounds after the
engine side was fixed — audit the sealing side you did NOT just edit.

Also verify guide CONTENT from gameplay, not a menu (see below), before
blaming the transport — and dump guides from the catch (`DumpGuidesNow`)
when the evaluate itself dies.

## Host-side lag & seal lifecycle — the perpetual skip (R69e)

The host's Arm runs at the game's FIRST evaluate. In menus/cinematics the
game may not provide depth/MV guides yet: the arm leaves the lag textures
null and guideW=0, and since render dims never change afterwards NO re-arm
ever fires — the seal's guide loop then `continue`s on null lags EVERY
frame for the rest of the session, even after the game starts sending
real guides. Symptom chain: guides stay zeros → dispatch death per frame →
throw loop → frozen frame over a live game (a device poisoned by the
per-frame dispatch death also no-ops the echo, so bufOut pins to the last
good frame while the game's own post passes keep animating underneath).
Structural fix in RecordSealFrame: detect guides-present && lags-null &&
guideW==0 → create the lags on-demand and force a full re-arm (Teardown +
retryAt) so shared bufs and the engine spawn with guides from frame 1.

Lag format (HISTORICAL — the lags were DELETED in the R69 closure: the seal
copies game texture → shared buffer directly). The `creation -> 11` line
that drove the TYPELESS detour printed TWO SUCCESS BOOLEANS (l0=1, l1=1) as
one number and was misread as an HRESULT; the actual root cause of null lags
was WRL ComPtr `operator&` in the guides array (ReleaseAndGetAddressOf —
released the lag every frame). Rule: log HRESULTs by NAME and multi-value
results as LABELED fields, never concatenated digits.

Instrumentation that cracked it: a silently-`continue`d block in a
per-frame hot path is invisible — add a skip-counter log WITH reason bits
(src null vs lag null) plus a creation-result log (fmt+dims+per-texture
bool) before theorizing about the data. RESOLVED: the `-> 11` was two
success booleans, and the null lags were the WRL `operator&` bug — see the
lag-format note above for the logging rule.

Slice log sessions by boundary lines (index of the last `engine starting`
/ `Offload: enabled`), never by timestamp string comparison — `[19:…]`
sorts after `[15:…]` and the filter silently returns the wrong session.

Format verdicts — the failure mode is the dispatch dying INSIDE the evaluate
(`CubinBackendNGX::launch Dispatch failed (NvAPI_Status=-1)`; create
succeeds, guides copy, the cubin launch refuses):

| Guide | Format | Verdict |
|---|---|---|
| mv real | R16G16_FLOAT | works (same fmt as the validated stub) |
| depth real | R32G32_SINT raw texture | dispatch death EVEN WITH all-zero content — the FORMAT, not the data |
| depth converted | R32_FLOAT via compute shader | dispatch survives; evaluate returns rc (content-dependent — test with real sealed guides before concluding) |
| depth stub | R32_TYPELESS, R32_FLOAT footprint, 0.5 | validated |
| depth as SEAL source | game texture with ALLOW_DEPTH_STENCIL (0x2) | not copyable by spec — Close E_FAIL, rebuild loop ends in driver crash; seal MV only + stub 'd' |

Conversion shader (SINT→R32_FLOAT): a small CS reads the shared buffer as
`StructuredBuffer<int2>` (SRV with `Format=UNKNOWN`), writes
`RWTexture2D<float>` with `asfloat(v.x)` (bit-cast of channel 0), constants
carry (w, h, pitchPx=pitch/8). Evaluate then passes the CONVERTED texture.
If the runtime still returns non-1 rc with zero-content guides, re-test
with real content first — the rc can be a content rejection, not a contract
error.

Translate evaluate rc codes to their NAMES in the log (the fork's
NgxResultName table): 0xBAD00002 = FAIL_PlatformError — GENERIC, it does
not distinguish format vs parameter; 0xBAD00005 InvalidParameter,
0xBAD00008 UnsupportedInputFormat, 0xBAD0000E UnsupportedFormat,
0xBAD00009 RWFlagMissing. A number-only log line sends you guessing at the
wrong layer.

The SINT-dispatch-death verdict above was measured passing a PLAIN SINT
texture in plain SRV state. The validated stub differs in more than format:
it is R32_TYPELESS created with ALLOW_DEPTH_STENCIL and born in
DEPTH_READ|SRV — the runtime may treat DLSSNR.Depth as a depth RESOURCE
(flags/state), not just bytes. The TYPELESS+DSV variant of that experiment FAILED AT CREATION: R32G32 is
not a depth format and CreateCommittedResource with ALLOW_DEPTH_STENCIL
rejects it outright (engine log `texGuideDepth (TYPELESS+DSV) creation
failed`; the engine dies before READY and the host re-arms in a loop) —
only real depth formats (R32_TYPELESS→D32 kin) accept the DSV flag.
Deployed arm (NOT yet verified live): local guide texture R32G32_TYPELESS
with NO flags, born in SRV, raw SINT bits copied over TYPELESS (identical
bits = valid copy). More generally, when our guide pipeline fails where the fork's
works, diff the RESOURCE PROPERTIES, not just the call args: the fork
passes the game's own texture — guides reach its evaluate in
NON_PIXEL_SHADER_RESOURCE, and its model OUTPUT enters the evaluate in
UNORDERED_ACCESS (MakeModelWritable) — so enumerate desc Flags, initial
state and state-at-evaluate in the comparison.

All-zero guide content does NOT exonerate or condemn the transport: with the
game parked in a menu the real guides dump as zeros legitimately. A
solid-color screen while ENHANCED frames flow is the game's content, not a
broken pipeline — check the engine log's liveness before diagnosing the
screen, and don't quote guide-content statistics from a menu frame (raise
`dumpFrame` so the one-shot dump lands on gameplay).

Per-guide A/B arm: flag file `coproc_stubguides.on` in the game dir whose
CONTENT picks the stubbed guide — 'd' = depth stub / MV real, 'm' = MV stub /
depth real, 'dm' = both (parse is `strchr(mode,'d')`: the letter NAMES the
stubbed guide). Read the parse code or the engine's resolved-state log line
(`A/B arm - stub depth=%d mv=%d`) BEFORE reporting which arm ran — reading
the letter backwards inverts the verdict. Delete the flag after the A/B.

Dumping guide content: record the CopyBufferRegion (shared buf → READBACK,
256-aligned pitch) BEFORE the evaluate when the evaluate is what dies; a
`DumpGuidesNow()` with its OWN command list + fence, called from the catch,
captures guides even when every frame throws. Parse R32G32_SINT as int32
pairs per pixel and try the f32-view of each channel before concluding
which one carries depth. A dump taken after a dispatch death poisoned the
device can read ZEROS with real content in flight: when the all-zeros
verdict comes from a session whose frame 1 died, re-dump in an arm where
the device survives (stub guides via the flag file, `dumpFrame` raised into
gameplay) before condemning the transport.

Runtime identity: pins in the fork's INSTALL docs are SHA256 prefixes while
sweep logs quote MD5 — comparing across hash functions "proves" a deployed
runtime wrong when it is the right one. Know which function a pin uses
before declaring a mismatch.

## Structural eco-on-throw (black-screen-with-HUD fix)

A throw from the vendor evaluate jumps past the frame's normal echo; texOut
keeps its creation zeros and the game presents BLACK with HUD/audio alive
(those render on another path). The catch must complete the FULL delivery chain — texIn → texOut → the
SHARED output buffer the host delivers from — before reporting done. An echo
that stops at texOut reproduces the black screen it was meant to fix: the
host's deliver reads the shared bufOut, which the happy path fills in its
final segment and a mid-frame throw skips entirely. Give the echo its OWN
command list + fence (the frame's list may be unclosable after the runtime
failure — E_INVALIDARG at Close) and call it from BOTH catches — then a
per-frame throw costs NR quality, never the image.

Catch BOTH kinds: a frame loop that throws int codes (`throw 7` from a
failed segment Close) needs `catch (...)` alongside `catch
(const std::exception&)` with the same echo-and-deliver body — an int throw
escaping a std::exception-only catch is an UNHANDLED exception that kills
the engine (host then re-arms forever). Distinguish the two witnesses in the
log: a dispatch throw inside the evaluate (`CubinBackendNGX::launch`)
vs `evaluate #N -> rc` with a following `seg vendor-frame close failed
0x80070057` = the runtime left the list unclosable after a non-1 rc.

## Panel NR knobs are CREATE-latched → rebuild on change (R69b)

The four DLSSNR.* sliders are ignored at evaluate time (the runtime latched
them at create). The engine watches the tuning block's `nrParamSeq`; a bump
triggers release + re-create of the feature with the new values. Save
fCreate/dev/cmd/gameDir as MEMBERS at Init — ProcessFrame parameters are not
visible to member helpers (InitDump/DumpToDisk/RebuildKnobs); plumb them at
frame entry. Verified live: ctl set → host ack → `feature rebuilt
(intensity …)` in the engine log. A rebuild failure falls back to bypass-eco
(vendorOn=false), never to a dead frame.

## Hot engine redeploy while the game lives

The host re-arms (respawns) the engine within ~1 s of its death — a plain
kill-then-copy races the respawn and lands a PermissionError or a stale
binary. Loop instead: kill coproc_engine by PID → try the copy → on
PermissionError kill again → repeat until the copy lands (a handful of
iterations), then verify the deployed hash. After repeated engine crashes the
host logs `done fence dead (device lost) - cooling down` and stops respawning
until the next evaluate cycle or relaunch — a silent engine after deploy is
cooling-down, not success.

Redeploying the HOST `nvngx.dll` needs the game AND the panel dead: the
panel process holds a handle to nvngx.dll (it reads host state), so killing
only RDR2 still yields PermissionError. Kill RDR2 → kill coproc_panel by
PID → copy → md5-verify → relaunch the panel from the game dir. A
A `done fence dead (device lost)` pair in host+engine logs right after the
game exits is the benign shutdown race, not a regression.

Post-deploy proof: md5 deploy==build is necessary but NOT sufficient — a
multi-anchor patch aborted mid-list leaves the source WITHOUT the fix yet
compiling clean, and the md5 then matches a fix-less build. Grep the
DEPLOYED BINARY for a marker string the fix adds (a new log line) before
declaring it shipped.

Experiment-state reset before a validation run (user standing request):
delete A/B flag-files (`coproc_stubguides.on`, `coproc_guidef.on`) and
restore `coproc.ini` overrides/knobs to defaults — experiment rounds leave
knobs at 2.0/style 2 and stale flags behind, which produce false verdicts
and confusing A/Bs.

## Which compose path reaches the SCREEN (RDR2 present-leg verdict)

The post-SR compose (write into the DLSS output texture after the Forward,
fork DeferredSr style) is mechanically correct — deltas arrive, the CS
dispatches, counters climb — but its output is NEVER PRESENTED by RDR2:
the game copies/processes the DLSS output before presenting. The decisive
probe is a pattern that WRITES WITHOUT READING (checkerboard cTest==1
straight into gOut): invisible while composes flow = the composed texture
is not the presented one. Every visible NR effect in this project therefore
traveled the PRE-SR path (edited color handed to the SR as its Color
input). Any screen-facing feature (boost, debug view) must live in the
pre-SR decode; the post-SR compose remains an internal instrument only.

Delta-view (visual verification of "is the delta applying?", user's
preferred method over numeric estimates): tint the delta in the PRE-SR
decode — red = positive correction, blue = negative, over a darkened
(25%) frame, amplitude scaled by the boost knob and multiplied to HDR
levels (×32) so it survives the game's tonemap. Implemented in CSDecode:
DecCb grows {float boost; uint test}, read from the tuning ABI and the
flag-file `coproc_compose_test.bin` ('2' = view on, '0' = off). The
flag-file MUST be re-read every ProcessFrame for live toggling — a
`static const` initialization reads it once per process and freezes the
mode for the whole session.

History-accumulator SRV rule (found via one-shot readback dumps): the SRV
the compose reads its history from must NEVER alias the UAV the same
dispatch writes (read of a resource in UAV state = undefined → the driver
returns zeros → the accumulator is born dead, no error anywhere). Pattern:
read hist[1], write hist[0], then swap by copy. When instrumenting a dump
of such a texture, its barrier must start from the resource's REAL state
(hist[0] sits in UAV after the compose — a NON_PIXEL→COPY transition from
there invalidates the whole dump list and the Map reads an untouched heap
= a false all-zeros that mislocalizes the bug).

DXGI format enum, verified this rig: 10 = R16G16B16A16_FLOAT (8 bytes/px);
R10G10B10A2 lives at 24 (TYPELESS) / 87 (UNORM), NOT 10 — check the enum
before building format arithmetic off a remembered number.

Valid A/B scenes for the view: live gameplay with a still camera ONLY. The
pause menu overlays animated film grain that poisons pixel diffs (a 26.6%
"effect" measured as pure grain), and photo mode freezes the game's
evaluates entirely (zero composes — no parameter change can reflect on a
frozen frame). Trusting either scene produces false negatives/positives.
