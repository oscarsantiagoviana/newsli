# Publishing and licensing

## License decision

**MIT** for new-sli code. Our code is original; patterns observed in
OptiScaler (GPL-3.0) are re-implemented, not copied — copying its files
would force GPL on the whole repo. Third-party vendored: Dear ImGui (MIT),
Detours (MIT) — each keeps its LICENSE file in tree.

## What we NEVER distribute

- `nvngx*.dll`, `sl.*.dll`, `amd_fidelityfx_*.dll`, `libxess*.dll`, the
  vendor NR runtime, model weights from NVIDIA DLLs — the USER provides
  them (from their driver/game/SDK). Releases ship code, never vendor
  binaries (the OptiScaler model).
- No signature-bypass replication (as some third-party DLSS enablers
  do): legally sensitive, and passthrough suffices for us.

## Attribution

- OptiScaler (optiscaler/OptiScaler) — backend-switching pattern, FSR2
  integration reference.
- OptiScaler-DLSSNR-PreSR-Multipass fork — NR composition semantics
  (residual accumulate, DlssNrWorkingScale), measured verdicts.
- renodx (Carlos Lopez) — DX proxy minimalism.
- NVIDIA NGX SDK headers — used under the RTX SDKs license terms.

## Disclaimers

Not affiliated with NVIDIA/AMD/Intel. DLSS, FSR and XeSS are trademarks of
their respective owners. Frame-generation features (future) are single-
player-oriented; swapchain hooks can trip anti-cheat in online titles.

## Repo hygiene (enforced at F5)

English-only code/comments; no personal paths, machine names, credentials;
no binaries in git; fresh-clone build must succeed with only VS + Windows
SDK + CMake installed.
