# Benchmarks

All numbers measured on the reference rig (RTX 3060 GPU 2, RDR2 unless
noted). Historical context in [05-historical-worklog](05-historical-worklog.md).

## NR cost per frame (1707×720 render)

| Path | ms/frame | Notes |
|---|---|---|
| Vendor in-process, CPU codec | 200 | 5.0 fps era (R67) |
| Vendor in-process, GPU codec @853×360 | 17-22 | 21.4 fps (R68) |
| Vendor in-process @1707×720 | 38-53 | 38 post-recycle (R69+) |
| workScale 1.0 / 0.5 / 0.25 | 52.5 / ~28 / ~19.5 | −47% / −63% (R71) |
| (legacy) own TRT fp16-mixed | 98.5 | 42 dB vs DLL (R62) |
| (legacy) CUDA graphs fp32 | 608 | bit-exact (R53) |
| (legacy) torch eager fp32 | 1550-1749 | the starting line |

## A/B deltas (live, verified chains)

| Comparison | Result |
|---|---|
| NR off/on @1.0 | 21.7% px, 3.3 mean |
| NR knobs 2.0 vs 0.0 | 24% px, 5.9 mean |
| Boost 1.0 vs 8.0 | 26.6% px, 7.0 mean (invalid scene — pause grain; kept as the cautionary tale) |
| Offload on/off, boost-amplified | 64% px, 10.9 mean |
| Delta-tint on/off | 118.5% vs 19.9%, 98.3% px |

## Dark-scene grain decomposition (R74b, hp-std in luma<20)

| Arm | Value |
|---|---|
| Native (offload OFF) | 5.51 |
| Our chain, NR off | 5.3-6.6 (≈ native) |
| pre-SR delta @boost 1.0 / 6.0 | +0.02 / +0.51 over boost 0 |

## Fidelity

- Own-weights path: bit-exact vs vendor eager fp32 (CUDA graphs); TRT
  fp16-mixed 42 dB PSNR vs the DLL.
- Identity guarantee: passive model ⇒ delta = 0 bit-exact (NR-off equals
  native by construction).
