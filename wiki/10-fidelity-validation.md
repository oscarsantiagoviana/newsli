# Fidelity validation

## The chain of truth (POC method, kept)

1. **Bit-exactness vs the reference implementation itself** — probes
   import and call the vendor's own path, never a re-implementation of
   "what we think it does". (A probe that validates your own reading of
   the reference validates only your misunderstanding.)
2. Per-stage decomposition (features → head → compose), counting every
   cast in the reference sequence — one dropped fp16 cast of scaled_color
   shifted features 3e-5 and the transformer amplified it to 1.69.
3. Content gates on REALISTIC non-zero inputs; PSNR always printed
   together with maxdiff and NaN counts (mse=NaN clamped by max() reads
   as 120 dB perfection — a probe artifact, not agreement).

## Live A/B (the end truth)

One variable, live game, still camera, native control arm; grain-bearing
menus and photo mode invalidate the measurement. Numbers in
[09-benchmarks](09-benchmarks.md); method lessons in
[08-bug-catalog](08-bug-catalog.md).

## Identity is a feature

Passive model ⇒ delta 0, byte-exact. The fail-safe is not "close enough"
— it is exactness by construction (zero-delta == native frame).
