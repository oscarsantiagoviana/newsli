# Verification

How we prove the pipeline is alive and correct — by surface:

## 1. Structural: the delta-tint (live knob)

`nr.tint = 1` paints the delta as a saturated red(positive)/blue(negative)
tint instead of applying it. **If the tint reaches the screen, every link
is alive**: seal → transport → vendor model → delta → compose → present.
Amplitude is HDR-strong (×20·boost) so it survives game tonemapping (the
POC's 1.0-amplitude badge was invisible — 183 vs 181 sky; 512-HDR was
153 vs 41).

- Verify in a PRESENTER game (Cities: Skylines II): tint on/off must flip
  the screen measurably (POC calibration: 118.5% tint metric ON vs 19.9%
  OFF, 98.3% pixels changed).
- In RDR2 the compose is not presented (below) — the tint verifies the
  chain only up to the composed texture; screen verification there is
  counters + A/B of the pre-SR... (retired) — RDR2 is a counters game.

## 2. Counters (the RDR2-grade surface)

Read `FrameScalars.engineResult / engineHostFrame` and the done fence from
the mapping: `done` must track `lastDelivered` (delay-line gate: compare
against LAST DELIVERED, never the pending token — R69u). The POC's
`delta diag` line (every 600 frames) is the template.

## 3. Screen-capture rules (paid-for lessons)

- Fullscreen exclusive: PrintWindow/DWM capture the redirected surface —
  ghost/dark false negatives. Use **borderless + CopyFromScreen**.
- ESC/diagnostic keys PAUSE the game and poison the measured scene —
  re-verify scene content after any input.
- A/B scenes: LIVE gameplay, still camera. Never the pause menu (animated
  film grain poisoned a 26.6% "proof") and never photo mode (0 evaluates).

## 4. Delta content forensics

One-shot readbacks at boundaries (shared delta buffer, history texture).
CAREFUL: the dump's barrier must start from the resource's REAL state — a
history texture left in UAV by the compose reads back as zeros if the dump
transitions from NON_PIXEL (false-empty, R73c).

## 5. Dark scenes and grain (R74b)

Dark-scene grain decomposes as: ~90% game film grain + DLSS-at-crushed-res
(present in native too — always run the native control arm) + our delta's
own noise, which scales LINEARLY with boost (measured +0.51 hp-std at
boost 6). The accumulator (D9) is the fix on our side; boost is an A/B
knob, never a quality knob.
