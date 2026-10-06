# Weights extraction (legacy POC line)

> Historical: the own-weights path (PyTorch/TensorRT) was retired when the
> vendor runtime became runnable in-process (R65-R68). Kept for the record
> and for a possible distillation future; the operative NR backend is the
> vendor runtime (see 07).

Summary of what existed (full history: 05-historical-worklog):

- An earlier research line reconstructed the model from the runtime's
  embedded resources to validate an independent TensorRT implementation
  (fp32 bit-exact reference; CUDA Graphs 608 ms; TRT fp16-mixed
  98.5 ms / 42 dB).
- Input contracts that mattered: display-referred BGRA8 in / RGBA16F out;
  softmax approximation; swizzle handling under graph capture.

No weights, extraction tooling, or model files are or will be published;
the operative backend is the vendor runtime (see 07). This page is a
tombstone for the retired line.

