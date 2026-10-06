# TensorRT engine (legacy POC line)

> Retired (R71b no-legacy purge). Kept as the distillation reference: if
> the vendor runtime path ever closes, this is the fastest own-engine
> starting point. Full history in 05-historical-worklog (R54-R64).

What existed: ONNX export of the 71-block graph; surgery (broadcast
constants materialized at frame size → scalars, −3.3 GB; zero-bias adds
rewired; selective fp16 Cast→MatMul(f16)→Cast around all 6413 MatMuls;
E4M3 emulation and softmax stayed fp32; external data dumped manually past
the protobuf 2 GB limit); TRT fp16-mixed at **98.5 ms / 42 dB vs the DLL**
(R62). fp16-full was 92 ms but 7.8 dB — discarded; the local optimum is
mixed, and the next speed step is QAT/distillation, not precision.
