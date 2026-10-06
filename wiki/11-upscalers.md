# Upscalers and backends

## Today

The game's own **DLSS** runs unmodified — our proxy forwards to the real
NGX and only adds the NR offload around it. Native is not just the
default; it is the fallback for every frame we do not handle.

## Architectural slot (F6)

The evaluate interception point is backend-agnostic: an `IBackendSR`
interface (`Init(device, params) / Evaluate(cmdList, params) / Shutdown()`)
with a factory keyed by enum, exactly the pattern OptiScaler uses —
re-implemented, not copied (GPL).

Candidates and what running them needs (catalog with sources:
the algorithms catalog of the reference projects):

| Backend | Vendor need | Entry | Status |
|---|---|---|---|
| DLSS (native passthrough) | NVIDIA | real nvngx | **shipped** |
| FSR 2.1.2/2.2.1 | none (any GPU) | `ffx_fsr2_api_x64.dll` + GetProcAddress | planned (first) |
| FSR 3.1/4 via ffx-api | none (FSR4 = RDNA4) | `amd_fidelityfx_*.dll` | planned |
| XeSS / XeSS 2 | none (DP4a on NVIDIA) | `libxess.dll` + GetProcAddress | planned |
| DLSS-D (RR) | NVIDIA | `nvngx_dlssd.dll` passthrough | possible |

**The differentiator**: backends run on the SECOND GPU — the context is
created on the engine's device with cross-adapter input copies. OptiScaler
does not do multi-GPU; that is our contribution.

## Frame generation (placeholder, phase after backends)

Every FG variant (OptiFG, DLSS-G via SL, FSR3-FG, XeFG) requires a
swapchain layer (Present hook + HUD fix + pacing) — unreachable from the
upscaler evaluate alone. Planned as its own module, opt-in, single-player
first (anti-cheat risk documented).
