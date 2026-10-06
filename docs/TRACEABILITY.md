# TRACEABILITY — engine

Map of each new-sli engine file to its POC source
(the working SR-offload POC's `engine/` tree).
Line numbers are POC lines. "Kept" = ported logic; "Deleted" = deliberately
dropped per the design changes (delta-only output, no badge, no CPU echo
bridge).

## src/engine/engine_ctx.h ← engine/coproc_engine.h (92 LOC)

| POC | Disposition |
|---|---|
| `EngineArgs` (L10-28) | Kept, reduced to `--luid --w --h --cf --map`: SR dims (`ow/oh/df/mf/of/dw/dh/mw/mh`), bias/exposure (`bw/bh/bf/ew/eh/ef`), `srFlags`, `srPath/dataPath/nrPath/nrRuntimePath`, `replayDir/replayPasses`, retired `hrf/hib/hie` — **deleted**. |
| `EngineCtx` device/queue/alloc/cmd/localFence (L40-46) | Kept verbatim. |
| `EngineCtx` transport heaps/buffers/textures (L54-67) | Kept: `heapInColor/heapOut/bufInColor/bufOut/texInColor/texOut`, guide triple buffer `heapGuideD/M[3]`, `bufGuideD/M[3]`, `texGuideDepth/texGuideMV` + fmt/pitch/dims/`depthInverted` metadata. |
| torch-bridge members (L69-78): `pocnrReadback/pocnrUpload/badgeUpload/pocnrRbPtr/pocnrUlPtr/pocnrBytes/pocnrSeq/pocnrInFlight` | **Deleted** (CPU echo bridge). Replaced by `bufZero` + `nrBytes` — the zero-delta fail-safe source. |
| `CoprocLog` in coproc_common.h (L186-246) | Replaced by `sli::Log/LogRate/LogInit` from `src/shared/log.h` (rotation + compile-time-checked formatting + minidump crash handler). |
| `NrVendor vendor` member (L79) | Kept. |

## src/engine/main.cpp ← engine/engine_main.cpp (239 LOC)

| POC | Disposition |
|---|---|
| `ParseEngineArgs` (L15-56) | Kept for the 5 live args only; ~20 legacy SR/replay args and the `cap_params.txt` replay block (L167-219) **deleted**. `--luid` is REQUIRED (POC allowed "first NVIDIA" fallback in replay mode, L86-100 — deleted: never fall back). |
| `PickAdapter` (L58-104) | Kept verbatim (NVIDIA 0x10DE + LUID match → `D3D12CreateDevice` FL 12.0), replay fallback branch deleted. |
| `SetupDevice` (L106-122) | Kept verbatim (DIRECT queue, allocator, list, fence, event). |
| `wmain` (L144-238) | Kept shape: crash handler → log argv → args → adapter → device → transport → `hs->ready = READY_MAGIC` (L229-235; ready through the mapping, never a fence) → loop. Added: log path beside the exe (`sli_engine.log`), ABI version check on the handshake. Legacy dead-code comment block (L150-153) deleted. |

## src/engine/loop.cpp ← engine/engine_loop.cpp (780 LOC)

| POC | Disposition |
|---|---|
| `BppFor/PitchFor` (L19-39), `FootprintFmtFor` (L93-110), `Footprint` (L112-122) | Kept verbatim. |
| `OpenSharedBuf` (L42-68), `MakeLocalTex` (L70-89) | Kept verbatim. |
| `OpenSharedTransport` (L124-278) | Kept: named-mapping open, handshake magic + NT handles, guide metadata, symmetric render-sized transport bufs, 6 guide bufs + local guide textures (raw game fmt), fence open. **Deleted**: the whole pocnr-bridge block (L222-276: readback/upload buffers, 8×8 fp16 badge upload, persistent maps). Added: handshake `version` check; `bufZero` upload buffer (zero-delta source, per-pixel alpha=1.0 fp16). |
| produce-wait loop (L340-368) | Kept verbatim: 60 s timeout WITHOUT advancing the frame counter, poison `UINT64_MAX` → clean exit. Added `vendor.Shutdown()` on poison (POC never released the feature; process death cleaned up). |
| DRS guard (L374-375) | Kept: `renderW/H != args` → bypass. The bypass is now a **zero delta**, not an echo. |
| `SubmitSeg` named segments (L379-399) | Kept verbatim (120 s fence wait per segment). |
| Vendor init block (L314-338) | Kept; knobs read from `Tuning` with `knobOr`. `gTuningForVendor` global (L17, L290) replaced by the `tuning` pointer passed into `NrVendor::Init`. |
| `RebuildKnobs` on `nrParamSeq` change (L480-497) | Kept verbatim. |
| Seal segment `in` (L416-438) | Kept verbatim (bufInColor → texInColor, COMMON↔COPY_DEST). |
| readback segment `rb` (L440-470) | **Deleted** (CPU bridge; vendor path works on GPU). |
| vendor `ProcessFrame` call (L472-528) | Kept (one cmd session; guides ring `[(frame-1)%3]`; `nrStage`/`deltaOut` parameter **deleted** — delta is always the output). Timing log kept. `nrDeadStreak` kept (evaluate failing 10× → zero-delta). |
| CPU echo `memcpy` path (L529-541) | **Deleted** (design change 3). |
| vendor GPU echo `vendor-eco` (L542-576) | **Deleted** — replaced by the zero-delta `zero` segment: `CopyBufferRegion(bufOut ← bufZero)` (cheapest correct: no textures, no state assumptions). |
| `nr-ul` upload+badge segment (L578-616) | **Deleted** (CPU bridge + badge). |
| delta → bufOut `out` segment (L618-641) | Kept verbatim (UAV→COPY_SOURCE→copy→UAV with args-dims footprint). |
| `engineResult=1`, `engineHostFrame`, done signal (L643-648) | Kept verbatim (write BEFORE the done signal — host reads on done). |
| "first frame computed" log (L650-651) | Kept. |
| catch std/non-std (L653-776) | Kept shape: `DumpGuidesNow` (L662, deleted with the dumps) + `EchoToOut` (L665-667, replaced by `ZeroDeltaToOut` = bufOut ← bufZero through a standalone cmd list, no texture-state assumptions) + cmd/alloc recreation + `engineResult=0` + done signal. The duplicated in-catch echo command recording (L681-714, L740-772) **deleted** — the zero-delta helper covers delivery. |

## src/engine/nr_vendor.h ← engine/nr_vendor.h (1212 LOC)

| POC | Disposition |
|---|---|
| Header recipe comment, format notes (L1-46), param-block ABI typedefs, `nr_half_to_float` | Kept (translated to English). |
| `nr_codec_cso.h` includes (L44-45) | → `g_nrEncodeCso_cso.h` / `g_nrDeltaCso_cso.h` (CMake `embed_shader`). |
| State: `ok/core/fwd/P/fslot/feature/fEval/seq/w/h`, workScale block (L89-93), model textures, guide ptrs/fmts, knobs, rebuild context | Kept. `stubDepth/stubMv/texDepthStub/texMVStub` (L106-107) **deleted** (stub A/B). `curDeltaOut` (L122) **deleted** (delta always). `devDump/dumpFrame/dumpPitch/dumpTaken` etc. **deleted** (dumps). |
| `FpFmtFor` (L136-154) | **Deleted** — superseded: guide copies carry the game's RAW format (the POC already copied depth with `rawDepthFmt` at L772; only the MV copy used `fpMv`, now also raw). |
| `set_uint/set_res/set_flt/discover_float_slot/core_path` (L156-198) | Kept verbatim. |
| `make_tex/make_buf/barrier/uavBarrier/GPUHandle` (L200-265) | Kept verbatim (`make_buf` failure now logs device-removed reason via `sli::Log`). |
| `InitCompute` (L270-442) | Kept: encode root sig (consts trimmed 9→7: the debug `gFrame` cbuffer member was deleted), delta root sig (8→7), tile grid + readback + CPU median storage, descriptor heap slots 0-5, SRV/UAV views with the two device-removed checks, BGRA8-UAV feature query. **Deleted**: `psoDec` (full-frame decode PSO, L349-354); `CoprocDumpD3DMessages` calls (kept out of the public tree; failure path logs directly). |
| `Init` (L448-680) | Kept verbatim in sequence: workScale clamp [0.25,2.0] + 64 px floors, registry core path, `LoadLibraryExW(_nvngx.dll)`, `Init_Ext(projectId, cwd, dev, 0x15)` (L505), capability block, `discover_float_slot`, post-init `GetDeviceRemovedReason` check (L523-529), model textures (BGRA8 sRGB in / RGBA16F out, work dims), no-guides bail, `InitCompute`, guides COMMON→SRV segment, pre-create tuning block (L617-636), forwarder load + export resolve + `dlssnr_call_set_float_slot` (L639-663), `dlssnr_call_create` feature 18 (L667-670). **Deleted**: stub-guides A/B arm (L561-604) and its flag file; `devDump` capture. Added: `tuning` pointer parameter (design change 13). |
| `tuningBoost` (L694-698) | Kept — reads the ABI block directly now (no `extern` global). |
| `tuningTestMode` (L699-710) | **Deleted** (the `coproc_compose_test.bin` FILE read) — replaced by `tuningTint()` reading `tuning->nrTint` live (volatile, no caching) per design change 4. |
| `ProcessFrame` (L712-953) | Kept in one cmd session: guide copies from the ring (raw-fmt footprints, SRV↔COPY_DEST), encode dispatch (7-const cbuffer), evaluate (`fEval` with work-dims color/out, full-res guides, `mvScale*workScale`, reset on first/forceReset), failure path restoring texInColor to COMMON, delta decode dispatch (re-bound heaps after evaluate, boost+tint live from tuning, 7-const cbuffer), tiles→readback copy, texInColor→COMMON. **Deleted**: `curDeltaOut`/`psoDec` branch (L866-875) — `psoDelta` always; the `deltaOut` parameter; dump blocks (L781-791, L896-945); stub texture selection (L828-829). |
| `EchoToOut` (L968-1029) | **Deleted** — replaced by `ZeroDeltaToOut` in loop.cpp (buffer-to-buffer zero copy, no texture states). |
| `DumpGuidesNow` (L1034-1085), `InitDump` (L1087-1112), `DumpToDisk` (L1133-1156), dump state | **Deleted** (frame-240 forensics). |
| `UpdateExposure` (L1113-1130) | Kept verbatim (median over tile grid → `expoScale = 0.18/med`; the dump-save tail deleted). |
| `RebuildKnobs` (L1171-1211) | Kept verbatim (release + re-create with new CREATE-time knobs, `seq=0`). |
| `Shutdown` (L1158-1167) | Kept. |

## shaders/nr_encode.hlsl ← engine/nr_codec.hlsl `CSMain` (L20-102)

| POC | Disposition |
|---|---|
| Encode kernel (L21-102) | Kept verbatim: coverage-box workScale downsample (L44-80), tile luminance grid (L82-91), Reinhard + 1/2.2 + per-component unorm write (L93-101). **Deleted**: the `gFrame` debug cbuffer member (and its CPU-side slot); everything decode-side (that is `nr_delta.hlsl` or dead). |

## shaders/nr_delta.hlsl ← engine/nr_codec.hlsl `CSDecodeDelta` (L104-201)

| POC | Disposition |
|---|---|
| Delta kernel (L113-201) | Kept verbatim as the ONLY engine output: display-domain delta `d = model_disp − enc(orig)` with saturation guard `enc≥0.985 → 0` (L186-189), clamp [-1,1], `SanitizeFinite3`, bilinear work→render resample (L154-178). **Deleted**: the 8×8 badge block (L193-199, `d=512`/`badgeA=2`). Added: `dBoost` multiply + clamp back to [-1,1] (boost moved from the deleted `CSDecode` L227-229 into the delta path — design change 5); `dTint` flag: the POC `CSDecode` tint block (L230-237: red=+/blue=−, amp `20*boost`, ×32 HDR) applied INSTEAD of the plain delta when set, with alpha=2.0 solid-signal marker (design change 4). `CSDecode` itself (L203-245, full-frame decode) — **deleted entirely** (design change 1). |

## CMakeLists.txt

- `sli_engine`: `embed_shader(sli_engine shaders/nr_encode.hlsl CSMain g_nrEncodeCso)` + `embed_shader(sli_engine shaders/nr_delta.hlsl CSDecodeDelta g_nrDeltaCso)`; include dirs `src` + `external/nvngx_dlss_sdk`; link `d3d12 dxgi user32 advapi32 dbghelp`.
- `/EHsc` added globally (the POC's bat set it; CMake was missing it for the try/catch loop).
- `external/nvngx_dlss_sdk/` populated with `nvsdk_ngx.h`, `nvsdk_ngx_defs.h`, `nvsdk_ngx_params.h` copied from the POC (no LICENSE.txt existed there to copy).

## Deliberate deviations from the POC (all per spec)

1. Delta-only output; no echo frame anywhere; zero-delta fail-safe (base+0).
2. No badge (CPU upload, CSDecode, CSDecodeDelta — all three deleted).
3. No CPU echo bridge; `ZeroDeltaToOut` replaces `EchoToOut`.
4. Tint via `tuning->nrTint` cbuffer flag, read live each frame (file read deleted).
5. `nrBoost` applied inside the delta shader (moved from deleted `CSDecode`).
6. Args minimal; LUID required, no NVIDIA fallback.
7. Ready = `hs->ready = READY_MAGIC` through the mapping (unchanged from POC).
8. Root-constant counts trimmed to the actual cbuffer sizes (7/7) — the POC over-declared (9/8) because of a since-deleted debug scalar.

---

# TRACEABILITY — host port (F3)

Map of each new-sli host file to its POC source
(the POC's `host/` tree: offload_host.cpp = 2755 L,
nvngx_host.cpp = 734 L). Line numbers are POC lines. The structural change
of the port: the POC's FOUR ad-hoc hot re-arm paths (gpuIndex 545-557,
workScale 526-535, DRS 2166-2178, late guides 1491-1499) are replaced by
ONE state machine (IDLE → ARMING → LIVE → DRAINING → COOLDOWN,
docs/fsm-design.md). The pre-SR delivery path (nrStage==0, texNrColor, lag
rings for the SR input, param substitution) is deleted entirely — post-SR
delta compose is the only delivery.

## src/host/nvngx_host.cpp ← host/nvngx_host.cpp (734 L)

| POC | Disposition |
|---|---|
| `CoprocLdrLoadExW` (49-66) | Kept as `CoreLoadDirect` (LdrLoadDll-direct, bypasses the load hook — no redirect recursion). |
| `CorePath` (79-110) | Kept verbatim (registry `NGXCore\FullPath` → DriverStore `nv_dispi.inf_*` glob fallback). |
| `Core`/`Real`/`Forward` (114-146) | Kept verbatim. |
| Queue observer `QTrack` (157-267): ECL vtable slot 10, probe-queue leak, `HookedECL`, `NoteEvaluateList` ring of 8, `SealQueue` | Kept. `SealQueue` returns the ComPtr by move (the POC's Detach + re-AddRef leaked one queue reference per evaluate). |
| Enablement `OnInit` (278-303) | Kept, simplified: engine exe present + `SLI_DISABLE` kill switch (the POC also required `nvngx.dll_fwd.dll` + `models/`, artifacts of the dead SR forwarder). Logging init (`sli_host.log` + crash handler) added — the POC host had no crash handler (POC-debt #9). |
| `GetParamResource` 3-step read (307-322) | Kept verbatim. |
| `DumpParamBlock` reference dump (331-393) | **Deleted** (forensics; the ABI surface is now frozen in src/shared/abi.h). |
| `TryOffload` (395-505) | Kept as `OfferFrameToSession`: guides-seen diagnostic block deleted; normalize (Width/Height → Render.Subrect, subrect bases, mvScale/jitter/preExposure/sharpness/reset + createFlags) kept. Returns void now — the native evaluate ALWAYS runs (post-SR: the offload no longer replaces the frame). |
| `DoInit` anti retry-storm (513-537) | Kept verbatim (forward ONCE, cache the result; the driver retried 1.2M times against raw failures). |
| Exports D3D12 (543-716): Init family → `DoInit`/Init_ProjectID; `CreateFeature` (captures the SuperSampling handle); `EvaluateFeature`/`_C` (offer → forward → `AfterRealEvaluate`); Shutdown/Shutdown1/ReleaseFeature; plain forwarders; `Host_Unsupported_Family` stub | Kept 1:1 (the _C variant is the one RDR2 uses). `CreateFeature`'s create-block dump deleted. |
| nvngx.def export table | Kept 1:1 (69 NVSDK names; D3D12 real path + CUDA/VULKAN/D3D11/OTA → one stub). |

## src/host/offload_session.cpp/.h ← host/offload_host.cpp (2755 L)

| POC | Disposition |
|---|---|
| ABI mirror structs (21-130) | **Deleted** — single-sourced in `src/shared/abi.h` (FrameScalars/Handshake/Tuning, offsets, magics, `FrameMapName(pid)`). |
| Format utils `BppFor/PitchFor/BytesFor/FootprintFmt/TypelessGuideFmt/TypedGuideFmt` (133-222) | `BppFor/PitchFor/FootprintFmt` kept verbatim. `BytesFor` (buffer sizing) folded into `PitchFor(w,bpp)*h`. `TypelessGuideFmt/TypedGuideFmt` **deleted** (lag-clone ping-pong died with pre-SR). |
| `EnvOn` (224), `StepFence` (247-275, 896-902) | **Deleted** (dead). |
| `SharedBuf` (229-245) + `MakeSharedBuf` (800-847) | Kept verbatim (cross-adapter heap + placed ROW_MAJOR buffer + NT handle on the heap). The R69k comment rot (comment says CUSTOM/system, code says DEFAULT) is resolved as built: DEFAULT, documented. |
| `MakeSharedFence` (887-894) | Kept verbatim. |
| `PickSecondAdapterLuid` (849-885) | Kept verbatim (NVIDIA ≠ game adapter, configurable ordinal). |
| `SpawnEngine` (906-986) | Kept: suspended CreateProcess (hidden console, cwd = engine dir), `dup()` NT handles into the child, handshake write (`hic/hgd[3]/hgm[3]/guide fmt/pitch/dims/depthInverted/hoc/hpf/hdf/version`), ResumeThread. `hib/hie` were already 0 in the POC; the fields are gone from the ABI. |
| `DumpTexDeltaDump` (992-1079) | **Deleted** (R73c debug). |
| `Teardown`/`TeardownEngine` (1084-1162) | Replaced by `DoDrain` (the ONE teardown path, fsm-design): state→Draining first (evaluates/submit-hook stop touching session objects), bounded done-fence wait, produce poison, kill by handle + zombie check, game-thread barrier (`g_frameBusy`), queue flush with a private fence (R70d: release only after every recorded list executed), release, delivery state dies (R70i). The POC's engine-only vs full split is gone — the ctl link outlives sessions by design. |
| `ArmDepthClone` (1180-1200) + `RecordDepthCapture` (1202-1256) | Kept verbatim (identical-desc clone; net-zero DW↔COPY_SOURCE in the game's list; plane 0 → R32_FLOAT footprint into guideD[frame%3]). |
| `InitCompose` (1258-1427) | Kept: view-format twins (never inherit DENY_SHADER_RESOURCE/TYPELESS), delta RGBA16F render-res, hist ping-pong, MV-of-K, RS (10 root consts, t0-t3, u0-u1), MipLevels=1 SRVs, t2=hist[1]-while-u1=writes-hist[0] (R73c fix). CB trimmed 11→10 (`cTest` deleted). |
| `RecordSealFrame` (1429-1766) | Kept: color in-list at the native read point (subrect box), depth clone in-list, guides+delta staging in the private seal list (R69q: never barrier the game's textures inside its own list beyond the two net-zero seals), close-then-Reset cycle discipline. **Deleted**: pre-SR lag-ring sealing (1600-1671), `coproc_sealguide.on` flag file (1531-1546), seal counters/diag logs. Late guides (1491-1499) now enqueue `Reason::LateGuides` instead of a synchronous teardown. |
| `DeliverPendingInList` (1775-1810) | **Deleted** (pre-SR: bufOut→texNrColor into the game's list). The post-SR delivery is the seal list's delta staging (POC 1673-1747, kept) + `AfterRealEvaluate`. |
| `WriteGpuBytesToDisk` (1812-1864), `FillTuples`+`COPROC_KEYS` (95-128, 1870-1908) | **Deleted** (dead since R64 / dumps). |
| `AfterRealEvaluate` (1914-2093) | Kept: output→clean (CopyTextureRegion, family-view twin), compose dispatch, composed→output, output back to its arrival state, hist swap only on accumulate. **Deleted**: damero/`cTest` test patterns, `coproc_compose_arrival.bin`/`coproc_compose_test.bin` flag files (arrival is now `sli.ini [sli] composeArrival`), compose diag counter spam. |
| `GetParamRes` (2099-2110) | Kept in nvngx_host.cpp (3-step read). |
| `Configure`/`SetAppDataPath` (2113-2132) | Kept; Configure starts the ctl server at NGX init (menu-time, R64c). |
| `DlssEvaluateSync` (2134-2690) | Split: the arm block (2180-2415) became `DoArm` (session thread; the game thread only publishes an arm snapshot + pokes); the READY wait (2417-2437) is inside DoArm with the FSM's 30 s timeout; the delivery/scalars/seal block became `Evaluate` (game thread, FrameGuard, single atomic state check — R69j never-block invariant); re-arms enqueue reasons instead of tearing down inline. **Deleted**: pre-SR delivery branch (2543-2581), param substitution (2621-2687), `return false` skip-native semantics (the native evaluate always runs now). |
| ctl listener (432-604) + hotkeys (639-712) + `CtlStart/CtlStop` (606-743) + `CtlLoadIni` (748-798) | Ported to ctl **v3** (`src/shared/ctl_common.h`): verbs get/set/reset/set-batch, mirror-by-field + mirrorSource, enginePid, heartbeat; reconfig-class sets (gpuIndex/workScale) enqueue FSM reasons instead of setting `rearmRequested`; `nrStage` handling deleted (field removed from the ABI); F7/F8 kept, Ctrl+F9 panel-spawn deleted. ini → `sli.ini [sli]` (was `coproc.ini`), `composeArrival` added. |
| `OnListSubmitted` (2698-2745) | Kept: pending-triple match under its own mutex (never the session mutex — R69i), seal list pushed behind the frame list, produce signaled on the queue (GPU-ordered). **Deleted**: delta dump staging (2724-2739). Now FrameGuard-counted so DRAINING cannot free the seal list under a live push. |
| `Shutdown` (2747-2752) | Kept: synchronous DRAINING with a 1.5 s cap on the game's thread. |
| **New (no POC source)** | The session thread (`SessionThread` + poke event): owns every FSM transition under `session.cs`; health poll in LIVE (engine death, done-fence poison w/ game-device check R70f, WS>5 GB recycle = drain reason Recycle); COOLDOWN timer; `Reason` taxonomy + coalescing per fsm-design's table. |

## shaders/compose_delta.hlsl ← engine/nr_compose.hlsl (149 L, fork-exact accumulator)

| POC | Disposition |
|---|---|
| Accumulate (mode 1): manual-bilinear delta/MV resample render→display, `hist' = lerp(reproj(hist), d, blend)`, invalid reprojection (non-finite / abs(MV)>2 / prevUV outside) → zero history, solid-signal (alpha>1) → take as-is; single dispatch also applies `out = clean + hist'` | Kept verbatim (ResidualBlend 0.08, fork parity). |
| Apply (mode 0): `out = clean + hist` (no re-lerp — the ghosting fix) | Kept verbatim. |
| Damero test `cTest==1` (59-64), delta-view tint `cTest==2` (76-89, 139-147) | **Deleted** — visual verification is the ENGINE-side `nrTint` ABI knob (painted INTO the delta with solid alpha, passed through this kernel unfiltered). |

## src/tools/host_smoke.cpp (new, F3 smoke gate)

S1 lifecycle (Configure/Evaluate/AfterRealEvaluate/OnListSubmitted/Shutdown
with mock args), S2 ctl v3 protocol against the live mapping (magic, set
ack, mirror, heartbeat), S3 shutdown budget. Exercises the real session
code; the arm fails on purpose (no engine beside a test exe) and must land
in COOLDOWN — not hang. Live RDR2 gate (F3 real) is a later phase.

## CMakeLists.txt

- `nvngx` target: `ngx_proxy.cpp` stub REPLACED by `nvngx_host.cpp` +
  `offload_session.cpp` + `nvngx.def`; links `d3d12 dxgi user32 advapi32
  psapi dbghelp`; `embed_shader(nvngx shaders/compose_delta.hlsl CSMain
  g_nrComposeCso)`. The load hook stays OUT (sli_dxgi owns it).
- `host_smoke` test target added (same sources + the embedded shader).

## Deliberate deviations from the POC (host port)

1. Post-SR compose is the ONLY delivery; no echo, no pre-SR, no badge. The
   native evaluate ALWAYS runs; the offload only adds the delta on top.
2. One FSM (fsm-design.md) replaces the 4 re-arm paths; resources exist
   only ARMING→DRAINING; game thread never waits (single atomic check).
3. `nrStage` removed from Tuning/ctl/ini (id 23 retired); `nrTint` is a
   live ABI knob painted engine-side (no host test pattern).
4. No file flags: `coproc_sealguide.on`, `coproc_compose_arrival.bin`,
   `coproc_compose_test.bin` → `sli.ini` config (`composeArrival`) or
   deleted outright.
5. ctl v3 protocol (shared/ctl_common.h) instead of the POC's v2.5
   hand-mirror; gpuIndex/workScale sets enqueue FSM reconfigs.
6. POC-debt #4 (MakeSharedBuf comment rot): documented as built (DEFAULT
   heap), comment fixed.
7. POC-debt #6 (queue-observer queue leak): fixed (move, no re-AddRef).
8. No ABI change: `src/shared/abi.h` used AS IS (ABI_VERSION stays 3); the
   host fills `Handshake::version` and the engine already checks it.
