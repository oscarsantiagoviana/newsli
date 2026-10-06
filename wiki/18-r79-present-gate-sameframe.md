# R79 — Same-frame Present-gate composition (fidelity first)

**User directive (verbatim intent)**: fps are irrelevant (will be recovered later
with FG/SR); the on-screen frame N must ALWAYS carry its own delta N. No
artifices with the NR output — if fidelity is lost, NR is pointless. The fork is
the reference for its CURRENT functionality only (no optional/beta modes).
Keep downscale-before-NR (workScale — verified working). Keep everything ready
for future FG/SR hooking (nvidia or alternatives).

## Why the architecture changed (R78 root cause closed)

R78 measured the old delay-line at **K-lag = 2 frames**: delta N was composed
onto frame N+2. Per-frame grain (film grain, ~76% of delta power) cannot be
cancelled at a distance — correction aimed at already-changed grain ADDS
variance (+8% hp-std on/off measured). No accumulation scheme fixes that; the
only fix is same-frame application.

Mechanics: the engine cannot start delta N until the seal of frame N executes
on GPU (end of frame-N submission), so the only point where frame N is fully
sealed AND not yet presented is the game's **Present(N)** call — exactly where
the fork's ApplyToFinishedPicture composes. Blocking earlier deadlocks.

## Implementation (commit `ed79a3f`)

1. **Swapchain proxy** (`src/host/dxgi_proxy.cpp`): CreateSwapChain* wrappers
   return a proxy IDXGISwapChain1..4; every method forwards, only
   Present/Present1 route through the gate. All 38 dxgi forwarders intact.
2. **PresentGateInner** (`offload_session.cpp`): event-based UNBOUNDED wait for
   delta N while the engine lives — WaitForMultipleObjects(done event, engine
   process). NO time-based per-frame skip (user directive). Only fallbacks,
   all PERMANENT degrades to native: engine process death, session re-arm
   (generation change), 10 s stuck-engine watchdog, GPU flush timeout. Slow
   engine = longer frame, never a mixed frame.
3. **Named compose stage**: fullscreen additive triangle draw on the backbuffer
   (own direct queue + command list on the game device, RTV cached per buffer,
   resize-safe, PSO per backbuffer format). Structured as a backend-agnostic
   stage — the hook point for future FG/SR composition.
4. **Test split mode** (ctl 28, panel row, ini): left half = NR, right half =
   untouched game output, 2 px divider. The fork's CompareSplit concept.
5. **F6 = panel bring-up/spawn** (was NR toggle); NR on/off stays on the panel.
6. **Removed entirely** (dead code rule): AfterRealEvaluate compose path,
   texHist accumulator + ping-pong + histValid, chain-reproject apply, per-seal
   texMv refresh, `shaders/compose_delta.hlsl`, nrTint==2 structural mode,
   LumaWeight. tint stays 0/1 (raw paint for chain verification).

## Live verification (RDR2, Colter night, 2026-09-29)

| Metric (same frame, split screen) | LEFT (NR) | RIGHT (native) |
|---|---|---|
| hp-std dark mask (v1) | **1.22** | 2.30 |
| hp-std dark mask (v2) | **1.13** | 2.17 |
| temporal flicker (mean |Δ|, 3 pairs) | **0.98–1.28** | 1.48–1.99 |

- **−47/−48% noise on the NR half, same frame, same scene** (before R79 the
  same A/B measured +8% WORSE with NR on).
- **−40% temporal flicker** on the NR half.
- Vision verification (full frame + ×2 zoom at the seam): right (native) side
  visibly grainier; seam clean (no halo/double image/color jump); NR side
  "smoothing, not smearing" — snow contour striations and rock silhouette
  preserved, no waxy look.
- Throughput: ~16 fps, GPU2 97% — expected price of same-frame waiting at
  workScale 1.0 (user: fps irrelevant; workScale/FG later).
- 0 degrades, 0 gate anomalies over the session; host_smoke 9/9, feeder
  G1/G2/G3 PASS, build 0 warnings /W4 /WX.

## Controls

- Hotkeys: **F6** panel, **F7** offload/native, **F8** reset.
- ctl: 20=NR on, 25=boost, 26=tint(0/1), **28=test split**.
- The split is the standing A/B tool: same-frame, same-scene, no captures
  needed to judge fidelity.

## R79b — positionable split seam + panel fixes (commit 6f86bab)

- The seam is now a POSITION knob (permille of width): 0 = off, 500 = centre,
  1..1000 = left of seam gets the delta. Slider in the panel ("Test split
  seam %"). Lets you park the seam on the subject (RDR2 keeps the character
  left-of-centre → 600 works well).
- Bug found & fixed: the host's ctl case bool-clamped the value
  (`v != 0 ? 1 : 0`) — a 600 arrived as 1, the shader read seam = 0.001 and
  the WHOLE frame rendered native with the compose counter still climbing.
  Symptom signature: composes flow, no visible split, hp-std equal halves.
- Live verification: seam at 0.400 / 0.600 / 0.700 → divider measured at
  columns 1024 / 1536 / 1791 (exact), vision confirms NR side cleaner.
- F6 spawn fixed operationally: deploy `sli_panel.exe` from `build/deploy/`
  (the `build/` root copy dies instantly rc=0 — wrong binary was being
  shipped).
