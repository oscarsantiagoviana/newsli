# Glossary

| Term | Meaning |
|---|---|
| SR | Super Resolution — the game's upscaler (DLSS unless a future backend takes over). |
| NR | Neural Rendering — the DLSS-NR model (NGX feature 18) that denoises/enhances before/around the upscale. |
| NGX | NVIDIA's in-driver neural API; the game calls `NVSDK_NGX_D3D12_*` which our nvngx.dll proxies. |
| Delta | The NR output expressed as a *difference* against its own encoded input (display domain, RGBA16F) — never a full frame. Delta-only is this engine's contract. Since R81 the transported payload is a multiplicative GAIN (delta semantics at composition: base × gain). |
| Boost | Live multiplier of the gain at composition (0..4, clamped). 1.0 = exact; 2-4 only for A/B visibility (amplifies signal AND noise). |
| Delta-tint | Visual verification flag: paints the delta as saturated red(+)/blue(-) instead of applying it. Replaces the POC badge. |
| workScale | The model runs at render×scale (fork DlssNrWorkingScale): coverage-box-exact encode down, bilinear decode up. |
| Exposure median | Codec exposure = 0.18/median(luma) computed on GPU tiles, applied with 1-frame lag (fork parity). |
| Carrier (BGRA8) | The model's required input format: display-referred sRGB bytes. fp16-linear input crashes the runtime (0xC0000409). |
| Guard (0.985) | Delta is zeroed where the carrier saturated — the amplified inverse there is noise, not edit. |
| Seal / in-list | Recording copies inside the game's own evaluate command list at the exact GPU point where DLSS reads (out-of-band delivery was the R21 stripes bug). |
| Guides | Depth + motion vectors sealed from the game each frame, triple-buffered; the engine evaluates frame N with N-1's guides (native NGX lag parity). |
| Delay-line | Pipelined delivery: seal N, deliver N-1. Removed with pre-SR: delta-only needs no frame substitution. |
| Bypass-eco | POC fallback that echoed the input frame; replaced in new-sli by ZERO-DELTA (base + 0 = untouched frame). |
| Recycle | Engine restart on working-set budget (the vendor runtime leaks ~10 MB/s; recycle keeps sessions bounded). |
| FSM | Host session state machine: Idle→Arming→Live→Draining→Teardown→Cooldown. Resources are recreated ONLY in Arming. |
| Present-path asymmetry | Historical (R73c, superseded by the R79 Present gate): an early compose-on-our-queue experiment was invisible on screen because RDR2 re-copies its backbuffer; the Present-gate architecture composes ON the presented backbuffer and removed the asymmetry. |
| A/B | Toggle one variable live (F7 offload/native) and measure pixel diffs; the POC's core verification method. |
| Poison | produce fence signaled to UINT64_MAX by the host on teardown → engine exits cleanly. |
| ABI | The shared-memory contract (`src/shared/abi.h`); any field change rebuilds host+engine together. |
| ctl | Live control protocol v3 (Local\sli_ctl_v3): panel/CLI ↔ host, set-batch, mirror-by-field. |
