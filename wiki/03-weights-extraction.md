# Weights extraction (legacy POC line)

> Historical: the own-weights path (PyTorch/TensorRT) was retired when the
> vendor runtime became runnable in-process (R65-R68). Kept for the record
> and for a possible distillation future; the operative NR backend is the
> vendor runtime (see 07).

Summary of what existed (full history: 05-historical-worklog):

- 649 tensors extracted from `nvngx_dlssnr.dll` (`.rsrc` WEIGHTS_HT,
  147 MB, 19 layers, int4/int8 quantized) → `dlssnr_logical.safetensors`.
- 71-block graph re-implemented in eager fp32 (bit-exact vs vendor), CUDA
  Graphs (608 ms), TRT fp16-mixed (98.5 ms, 42 dB).
- Input contracts that mattered: display-referred BGRA8 in / RGBA16F out;
  E4M3 publish semantics; softmax approximation; swizzle tensor (device-
  side cache needed for graph capture).

The extraction TOOLING (scripts, probes) lives in the archived POC repos;
it is not part of new-sli.
