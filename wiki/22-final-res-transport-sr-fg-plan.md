# Final-resolution delivery, transport, and the SR/FG roadmap

Consolidated plan (2026-10-01) after the full reference study
(the reference-projects study, OptiScaler
upstream/Aurora code map, UNCANNY v0.20 docs, NeuRotic design docs) plus
our own measurements. Sources verified on disk; licenses checked in git
history, not assumed.

## 0. The one fact that reframes everything

The PCIe transport is **not** the bottleneck. Per frame we move
~59 MB (color 21.1 + gain 21.1 + depth 4.7 + MV 9.4, pitch 256B-aligned);
at 25 fps that is ~1.5 GB/s ≈ **9% of PCIe 3.0 x16**. The 150 ms
frame-to-frame latency is **serialization**: late seal inside Evaluate →
one inference on GPU2 (~50-100 ms) → single output buffer → the gate waits
for it inside Present. Every optimization below targets the timeline, not
the wire.

## 1. Frames and inputs at final resolution (the user's goal)

Three levers already exist in the product (R82i); no new code for the
first pass:

| Lever | Meaning | Cost | Quality |
|---|---|---|---|
| `srRatio = 1.0` | Game renders **native 2560×1080**; its DLSS keeps running as DLAA (temporal AA at 1.0). Our wrapped optimal-settings callback forces the ratio (`nvngx_host.cpp:299-341`) | ~3.6× pixels on GPU0 — affordable: GPU0 sits at 25% because the gate strangles fps, not because raster is heavy | **Strictly better than DLSS Quality**: no upscale, full accumulation |
| `srRatio = 1.0` + `srBypass = 1` | No SR at all; our NR composes over the native frame | Same raster cost | Native + NR; some shimmer (no temporal SR) — documented in the panel |
| game menu (today's default) | DLSS Quality as always | baseline | the SR loss we are removing |

**In-game verification (A/B)**: DLSS Quality baseline vs `srRatio=1.0`
(needs the game to re-query — level change / re-create; knob is latched at
create). Compare crops offline.

**Gap to close (small)**: our callback overrides Width/Height/Scale and
Dynamic_Max, but not `DLSS_Get_Dynamic_Min_Render_*` — under RDR2's DRS the
engine can still drop below the forced ratio. Add the Min pair (upstream
overrides both, `NVNGX_Parameter.cpp:480-531`). Same callback, one more
write, panel text updated.

Honest limit: at 9 fps the scene still suffers from cadence (the ghosting
investigation). Final-res rendering fixes **sharpness**, not fps; fps is
Section 2.

## 2. Transport and pipeline optimization (fork-evidenced, ordered)

1. **Output ring (UNCANNY pattern)**: today `out.buf` is a single shared
   buffer — the engine blocks while the gate stages/composes, the gate
   blocks while the engine writes the next frame. Ring of 2-3 slots,
   advanced by **fence-completed values, never slot indices** (UNCANNY:
   ring of 3, completion-driven; "a failed output never overwrites the
   last good shared buffer"). Local change, engine+host. Removes one
   serial link.
2. **Bounded wait knob (UNCANNY REPAIR7 pattern)**: optional cap on the
   gate's wait (e.g. 100 ms, 0 = today's unbounded). A frame whose delta
   misses the cap presents **native** (never a stale composite — that is
   the whole point of same-frame R79; UNCANNY discards late results
   instead of reusing them). Fps floor becomes engine-independent; NR
   coverage % shown in the panel. This is the honest answer to "6 fps"
   without touching R79.
3. **Matched-residual (divergence #1, our top [THEY-BETTER])**: with
   `nrWorkScale<1` the classic path reads downsample blur as headroom →
   color shift. The fork's residual transfer fixes it. Once fixed,
   `nrWorkScale 0.5` becomes usable → engine ~2× faster → gate wait
   halves. Offline-verifiable with the feeder before any in-game test.
4. **sRGB piecewise encode/decode (divergence #2)**: what the model was
   actually trained on; also required before 3 can be trusted
   numerically.
5. **Relaxing same-frame (R79) = user decision, not an optimization**:
   composing frame N with delta N-k hides latency but reintroduces
   mix-age ghosting. Only if 1-4 are not enough. Default answer: no.

Also adopted from the study, cheap and structural: GPU timestamps around
the offload leg (OptiScaler `GpuTime_Dx12` pattern — timestamp heap ×2,
resolve read back **next** Present, never inline) so the panel can show
seal/infer/compose legs instead of one opaque DELTA ms.

## 3. Additional SR backends (DLSS alternatives)

License verdict (verified): **vendorable with source** = FidelityFX SDK
FSR3.1 upscaler via `ffx_api` (**MIT**, explicit exception list in the
SDK's license.md) and FSR2 2.1.2/2.2 (MIT). **Redistributable binaries,
no modification** = Intel XeSS `libxess.dll` (DP4a runs on our 3060s),
NVIDIA `nvngx_dlss.dll`, FSR4 (closed, RDNA4-only — irrelevant to this
rig). **Untouchable** = all OptiScaler/Aurora/NeuRotic code (GPLv3 since
2025-02-05; the MIT era ended with FG's arrival) and Nukem's
dlssg-to-fsr3 (GPLv3).

Architecture note that makes this natural for us: the engine process
already owns a D3D12 device on **GPU 2** and already holds render-res
color + MV + depth + jitter per frame — the exact input contract of every
upscaler. FSR3/XeSS init happily on a second device. So:

- **SR1 (first)**: `IBackendSR` factory in the engine (the wiki-11 slot,
  OptiScaler-style contract, re-implemented); backend **FSR 3.1** via
  vendored MIT `ffx_api`. New mode: game renders render-res → we do
  **SR on GPU 2** (with NR around it) → display-res frame returns. GPU0
  sheds the DLSS cost entirely. Fallback per session = native DLSS on
  GPU0 (fail-open, UNCANNY promotion pattern: N healthy presents before
  the backend engages, degrade on streak).
- **SR2**: XeSS 2 via redistributed `libxess.dll` (DP4a on NVIDIA),
  same interface.
- **SR3 (revisit)**: DLSS-on-GPU2 (both cards are RTX 3060) — this was
  the vetoed "SR-takeover"; the veto predates the stable gate. Only
  after SR1/SR2 are proven, opt-in, default off.
- The panel's backend combo (honest placeholder today) becomes the real
  selector: `native (game DLSS)` / `fsr3-gpu2` / `xess-gpu2` — every
  option live, per the panel rule.

## 4. Frame generation (including non-NVIDIA)

Verified requirement map: **every** FG needs swapchain-side cooperation.
RDR2 ships DLSS via plain NGX, **no Streamline** (`sl.*.dll` absent from
the gamedir) → DLSSG in-list is impossible here; the Nukem route needs a
DLSS-FG game anyway and is GPLv3 (excluded twice over).

- **Output**: FSR3-FG via the **MIT** FrameInterpolationSwapchain
  (`framegeneration/fsr3` in the FidelityFX SDK) — requires intercepting
  `CreateSwapChain(ForHwnd)` and owning Present/resize (OptiScaler's
  `FG_Hooks.cpp` documents the whole surface: vtable slots
  Present=8/ResizeBuffers=13/Present1=22/ResizeBuffers1=39, latency
  waitable emulation, Reflex pacing markers, auto-pause on stall). We
  already own a full DXGI proxy layer — the surface is familiar.
- **Inputs**: we are unusually well positioned — FG consumes
  display-res color + MV + depth + jitter, which is literally our
  transport. The missing piece is **UI exclusion** (hudless): OptiScaler
  gets it via RTV hooks + heuristics (`ResTrack`/`hudfix`, their most
  complex subsystem); XeFG accepts explicit UI tracking; FFX-FG takes the
  backbuffer copy with the standard artifacts. Honest rating: this is
  the biggest new subsystem of the whole roadmap.
- XeFG (`libxess_fg.dll`): binary-only, borderless required; candidate
  after FSR3-FG, same interface.
- **Sequencing gate (honest)**: FG interpolating a 9-15 fps stream is
  pointless. FG starts only after Section 2 puts the cadence ≥40 fps.
  Anti-cheat caveat stands (single-player only; RDR2 story qualifies).

## 5. Order of execution

| Round | Content | Gate |
|---|---|---|
| R83 | `srRatio=1.0` A/B in-game + Dynamic-Min override + output ring + bounded-wait knob + GPU timestamps | in-game crops; fps & latency numbers in the panel |
| R84 | matched-residual + sRGB piecewise | feeder-verified byte parity at ws=1; in-game ws=0.5 A/B |
| R85 | IBackendSR + FSR3.1 on GPU2, panel backend selector | in-game: SR-on-GPU2 vs native DLSS crops; fps |
| R86 | XeSS backend; DLSS-on-GPU2 spike (veto review) | same harness |
| R87+ | FG spike (FSR3-FG swapchain offline first), only if cadence ≥40 fps | standalone sample before game |

Every round: knobs land in the panel the same day, honest descriptions,
no dead controls; deploy = 5 binaries to gamedir + `build\deploy`; in-game
verification with captures before anything is called done.
