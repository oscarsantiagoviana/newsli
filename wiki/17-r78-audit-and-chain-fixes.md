# R78 — Full RDR2 chain audit & fixes (capture → engine → compose)

**Scope**: user directive — verify the whole RDR2 chain, check NR is visually
present, compare face/skin handling vs reference projects, then implement
multipass. Special attention to how frames are treated, aligned, accumulated
("we suspect failures in the capture or output chain").

## What was verified (evidence in `analysis/2026-09-29_audit-cadena-tiempo-captura-compose.md`)

| # | Point | Verdict |
|---|-------|---------|
| 1 | Capture origin / jitter | OK-equivalent — the fork's NR path does not consume jitter either; our capture records it but no consumer needs it |
| 2 | MV domain & scale | OK — literal fork formula |
| 3 | MV alignment K | OK — `mvRing[8]` hands the delta's own frame MV |
| 4 | Exposure chain | OK-equivalent + **[FIXED]** tile grid sized to RENDER but filled at WORK: median read uninitialized VRAM when `workScale < 1` |
| 5 | Accumulator | fork-exact + **[FIXED]** alpha stamped 1.0 (now preserved, fork parity); **[FIXED]** knob rebuild no longer leaves stale history (`nrParamSeq` invalidates) |
| 6 | Engine guides | **[FIXED]** read `(frame-1)%3` while the seal writes `frame%3` — color N with guides N-1, an incoherent set; now same-frame |
| 7 | Delay-line K-lag | **By design** (measured **2 frames** live): delta lands 2 frames late; per-frame grain (film grain) is noise at N-2 and lands as new noise |
| 7b| Apply smear | **[FIXED]** apply now chain-reprojects the history with the current frame's MV (texMv refreshed every seal), decay 0.96 — the trailing-smear class of artifacts |
| 8 | NR adds noise | Root-caused: at K-lag>0 the per-frame grain component (~76% of delta power) cannot cancel; measured +8% hp-std on/off at boost 1 (night scene) |
| 9 | Skin/faces | Gap documented: fork has full YCbCr-ellipse skin protection (detail/colour lerp); we only expose vendor create-time knobs |

## The noise math (why lagged NR adds noise)

`delta(K) ≈ −grain(K) + structure`, applied at N (K = N-2):
`out(N) = frame(N) + hist ≈ frame(N) − 0.2·avg(grain(N-2…))`.
Var(out) > Var(frame): the correction aims at grain that already changed.
Same-frame application (fork single-pass) cancels it; lagged application
cannot. **Fixes**: (a) chain-reproject (done, kills the smear), (b) multipass
half-rate accumulation with anchor motion & suppression (fork design — next).

## Multipass plan (from the fork, GPL pattern re-implementation)

1. `Before`: capture at the SR seam into delay-line slots (already ours)
2. NR evaluate on the private feature (ours)
3. `After`: `ApplyResidual` in-place on the SR output (ours — compose)
4. Fork extras to port: half-rate `ResidualFg` + `anchorMotion` +
   `suppression` map + camera-cut detection on MV-field similarity
5. Skin protection (YCbCr ellipse, fork pattern) — host-side compose mask

## Live measurements (RDR2, night scene, after fixes)

- K-lag: **2 frames** median (10 samples)
- hp-std on/off (boost 1): 3.19 vs 2.94 (+8%) — the lag-applied grain
- boost≈0 control: −0.08 (chain itself is clean; noise lives in the hist)
- Structural tint: strong blue (negative) DC — real denoise signal present
- Flicker on 1.99-2.26 vs off 1.15-1.32: inference grain in the history

## Files touched

- `src/engine/nr_vendor.h` — tile grid WORK-sized
- `src/engine/loop.cpp` — guides `frame%3` (was `(frame-1)%3`)
- `src/host/offload_session.cpp` — lastParamSeq hist invalidation; texMv
  refresh per seal; swap on both modes
- `shaders/compose_delta.hlsl` — alpha preserved; apply chain-reproject
  with current MV + decay; structural tint unchanged
