# Fork reference: OptiScaler-DLSSNR-PreSR-Multipass

The DLSSNR-first OptiScaler fork (upstream
`OptiScaler-DLSSNR-PreSR-Multipass`, vendored locally as
reference) is the semantic reference for NR composition. What we adopted
vs. what their measured verdicts warned us about:

## Adopted

- **Residual composition** (`final = original + invCodec(model) −
  invCodec(encode(original))`) instead of reconstructing from model output
  (their 67% error vs 0.93%).
- **Temporal accumulator with MV reprojection**
  (`dlssnr_residual.hlsl` v2): `hist = lerp(reproject(hist), delta, 0.08)`,
  invalid reprojection → history zero, fades back in. "Per-frame noise is
  temporally uncorrelated and averages to zero; the enhancement persists."
- **DlssNrWorkingScale** (model at render×scale; coverage-box encode,
  bilinear decode, MVs×scale).
- **Exposure handling** (white-point codec, 1-frame lag).
- **CREATE-latched knobs** → rebuild on change, debounced.
- **One feature per layer; evaluate only after the create epoch changes.**

## Their verdicts we did NOT re-litigate (measured dead ends)

- Temporal filtering of the MODEL OUTPUT (twice) — old answers do not
  belong to new frames.
- Static-HUD detection for masking (2.5:1 separation impossible).
- One-point exposure anchoring (drifts).

## Their instrument value

Deployed live, the fork's GPU-timestamp `DLSS-NR cost:` lines measured the
vendor model at 28.5 ms @1505×635 on this rig (R65) — the number that
re-legitimized the vendor runtime as the engine backend.
