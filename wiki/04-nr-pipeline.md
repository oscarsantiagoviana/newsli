# NR pipeline (the model's own graph)

> Historical/structural page: the operative backend is the vendor runtime
> (07); this documents the MODEL's contract as reverse-engineered — it is
> what the codec and the delta semantics implement against.

## Input/output contract (measured, sm_86)

- Input: **display-referred BGRA8 sRGB** (the "carrier"). Raw HDR linear
  (median ~104, max ~36k) triggers the runtime's exact silent-copy —
  ×0.01 processes, ×0.005 does not. fp16-linear input = __fastfail
  0xC0000409. Output: RGBA16F display-domain.
- Guides: depth R32_FLOAT (0.5 constant passes, real depth better) with
  `DepthInverted=1`; MVs R16G16 (zero MVs OK; no MV texture = crash).
  Without any depth the runtime falls to safe-passthrough — depth was the
  inference gate (R66).
- Tuning read ONCE at create (evaluate-only writes ignored) → knob changes
  rebuild the feature.

## The graph (own-weights era, for the record)

19 layers / 71 blocks: Swin 1h→16h + split-Swin 16h + VIT (qkv/ffn/
attention/projection), int4/int8-quantized weights, E4M3 publish
semantics, approximated softmax, a swizzle tensor (device-side index
cache needed under graph capture), quadratic gate. Full extraction and
block-by-block notes: archived POC repos (05-historical-worklog R40-R62).

## Codec around the model (what new-sli implements)

```
encode: HDR linear → ×exposureScale(0.18/median, lag 1 frame) → Reinhard
        d/(1+d) → clamp 1 → pow(1/2.2) → BGRA8 (per-component write)
delta:  model_disp − encode(orig)  in display domain [-1,1]
guard:  delta = 0 where encode ≥ 0.985 (carrier saturated — the amplified
        inverse there is noise, not edit)
resample: bilinear work→render when workScale ≠ 1
```

Identity guarantee: a passive model yields delta = 0 bit-exact (NR-off ==
native by construction).
