# new-sli

A research proof-of-concept that offloads NVIDIA's DLSS-NR neural denoise
(the ray-reconstruction-class model, NGX feature 18) to a **second GPU in
a separate process**, and composes its correction back onto the game's
frame — around the game's own DLSS Super Resolution, never replacing it.

This is a personal laboratory. It is not a product, not tuned for optimal
performance, and not affiliated with NVIDIA.

## What it does

Every presented frame keeps its **own** denoise. The finished (upscaled)
frame is sealed into cross-adapter shared buffers right after the game's
DLSS evaluate, a worker process (`sli_engine.exe`) on the second GPU runs
the NR model, and the resulting display-domain correction is multiplied
onto **that same frame** inside Present — a same-frame gate (default:
unbounded wait, so coverage is total; the fps floor becomes the engine's
speed). If anything fails anywhere, the correction is neutral (gain 1.0):
the game's frame ships untouched — fail-open, never a stale verdict from
an older frame.

```
game (GPU 1)                      engine (GPU 2, own process)
├─ DX12 render + game's DLSS SR   ├─ receives sealed frame + guides
├─ Evaluate: seal copy into  ────► │   (cross-adapter shared heap)
│  shared transport               ├─ DLSS-NR model (vendor runtime)
├─ Present gate:                  ├─ decode -> display-domain GAIN
│  wait for THIS frame's gain ◄───┤
│  multiply over backbuffer       └─ fail -> unity gain (frame untouched)
└─ real Present
```

- The interception is exactly one point: the SuperSampling evaluate.
  Every other NGX call is forwarded untouched to the real driver NGX;
  `dxgi.dll` + `nvngx.dll` sit next to the game purely as the load
  vector. All shaders compile at build time (nothing at runtime).
- The Present gate runs on the game's own queue (falling back to the
  offload's direct queue when no game queue was observed). Guides
  (depth/motion) ride the same transport, triple-buffered and labelled by
  the produce-fence value, so the slot always matches the sealed frame.
- **Panel** `sli_panel.exe` (Dear ImGui) — every control bounded and
  explained in plain language (what it does / what the engine really
  receives / the side effect of deviating). Hotkeys: **F6** panel,
  **F7** offload⇄native instant A/B, **F8** reset tuning.
- **CLI** `sli_ctl.exe` — same protocol, scriptable.
- **Offline harness** `replay_feeder` — feeds real frame captures through
  the exact production ABI into the engine, no game required.

## Why this exists (a note from the author)

This project was not built to ship an optimal upscaler/denoiser — that is
neither my time budget nor my field. It pursues two goals at once.

The first is methodological: a hands-on testbed for generative-AI
platforms and models — in particular **zcode**, the **Hermes agent**, and
the models available on **z.ai** — and a personal exercise in how far I
can currently steer a fleet of generative AIs through a problem far
outside my area of expertise, at a scale and with integration surfaces
that cannot be fully covered by unit tests alone. The repository, its
worklog and its verification culture are as much the artefact of that
exercise as the binaries.

The second is a materialization exercise. I believe digital animation is
heading toward **dual engines**: a rule engine that generates scenarios,
characters, plot, actions and their consequences — and a media engine
that turns those directives into the actual audiovisual content, and is
the side you train with your styles, characters and scenography.
Real-time neural post-processing that lives *beside* a host application,
receives its inputs over an interop transport, computes a correction on
separate hardware and hands it back within the frame's own deadline is a
small but concrete seed of the shape of that second engine. This repo is
that seed, kept public in case it is useful to anyone walking the same
road.

## Reference projects

The composition math, the operational patterns and several hard-won
lessons here come from studying (and running) these public projects:

- [OptiScaler](https://github.com/optiscaler/OptiScaler) — the universal
  DLSS/upscaler proxy this project's load architecture learned from
  (dxgi-slot loading, NGX call forwarding, parameter contracts).
- [OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)
  — the DLSSNR-first fork whose finished-colour gain composition,
  matched-residual carry and bounded-ratio guards are the semantic
  reference for our decode/compose (see `wiki/15-fork-reference.md`).
- [Dagherbou's OptiScaler DLSS-NR fork](https://github.com/Dagherbou/OptiScaler_DLSSNR)
  — the original neural-rendering fork lineage.
- [NeuRotic](https://github.com/MagicalPrincessUnicorn/NeuRotic-an-OptiScaler-DLSSNR-fork)
  — an OptiScaler DLSSNR fork with additional rendering routes,
  diagnostics and installer tooling, whose delivery patterns informed
  our transport design.
- [RenoDX](https://github.com/clshortfuse/renodx) — colour-processing
  patterns referenced by the forks above.

No GPL code from these projects is included in this tree; their public
source was read as documentation (the composition math was re-derived in
our own terms and is MIT here).

## What you need

- Windows 10/11, **two GPUs** (the offload is the point; single-GPU is
  out of scope), one NVIDIA GPU as the engine side (Ampere-tested:
  RTX 3050 game + RTX 3060 engine).
- A game that loads `dxgi.dll` from its own folder and uses NGX/DLSS on
  D3D12 — RDR2 is the proven one.
- The proprietary NVIDIA runtime, user-provided — see
  [Proprietary NVIDIA components](#proprietary-nvidia-components-not-in-this-repository)
  for exact files, hashes and placement.

## Install on Windows

1. Close the game.
2. Build (see below) or get a release, and run `sli_installer.exe`:
   pick the game's folder, Install. It writes the deployables, a default
   `sli.ini` (never clobbering a user-tuned one) and an uninstaller.
3. Put the two NVIDIA runtime DLLs beside the installer before
   installing (or copy them next to the game's exe afterwards).
4. Launch the game, launch `sli_panel.exe`.
5. **F6** opens the panel; tick **Offload to second GPU**, then **Neural
   rendering** (first enable builds the model, ~1 s). **F7** is the
   instant offload⇄native A/B; **F8** resets tuning. The built-in
   **Test split** paints the NR edit left of a movable seam, native
   right — the honest in-game A/B.
6. `sli_ctl.exe` (CLI) mirrors every live value.

**Zero trace**: the installer writes nothing outside the chosen folder
— no registry, no Add/Remove-Programs entry, no remembered state. Uninstall:
`uninstall_sli.exe` in the game folder removes exactly what was deployed
and then deletes itself; after a full cycle nothing remains.

## Keep in mind

- The same-frame gate bounds fps by the engine's round-trip while NR is
  on with the unbounded wait (the honest trade this project chose: never
  present a frame without its own denoise). The bounded-wait knob trades
  coverage for fps explicitly.
- One game proven end-to-end. Different NGX integrations may differ.
- Anti-cheat: injecting `dxgi.dll`/`nvngx.dll` into a game is
  modding-adjacent. Research use only; avoid protected multiplayer games.
- This is a laboratory: knobs are exposed deliberately, defaults are
  conservative, and the wiki documents what every control really does.

## Build

Requires: Visual Studio 2022 Build Tools (MSVC), Windows SDK, CMake ≥ 3.24
(Ninja). The proprietary NVIDIA bits are **not** in this repository — see
the section below for the three SDK headers you must place by hand to
compile.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
# proxy DLLs + engine + panel land in build/deploy/; the CLI tool
# sli_ctl.exe lands in build/ (kept apart from the proxy DLLs by design)
#   deploy/: dxgi.dll  nvngx.dll  sli_engine.exe  sli_panel.exe
#   build/:  sli_ctl.exe
```

For the installer: `cmake --build build --target package` builds
**`build/package/sli_installer.exe`** (single exe, everything embedded:
deployables + default `sli.ini` + the uninstaller; copies the NVIDIA
runtime DLLs when they travel beside it).

## Documentation

Full documentation in [wiki/](wiki/README.md) (28 pages, English):
architecture (02), the NR pipeline (04), verification culture (17),
performance (09), panel design (16), runbook (13), historical worklog
(05). Design docs in [docs/](docs/) (FSM, POC→here traceability).

## Status

| Phase | State |
|---|---|
| F0 skeleton + toolchain | done |
| F1 shared ABI/ctl/log | done |
| F2 engine delta-only (offline-certified: identity bit-exact, real model deltas, 1000-frame soak) | done |
| F3 host session FSM + NGX proxy (offline smoke: lifecycle, ctl v3, 16 ms shutdown) | done |
| F4 panel (live mirror, batched Apply, ini) | done |
| F5 live verification | host LIVE in RDR2 (observer+ctl+F7 verified) |
| F6 alternative backends (FSR/XeSS slot) | planned |

## Proprietary NVIDIA components (NOT in this repository)

This project needs NVIDIA's proprietary DLSS-NR runtime and NGX headers.
**They are never distributed here** — each developer supplies them
locally (they are gitignored). The exact files, their versions and
hashes:

| File | Version | SHA-256 | Placed at (build) | Placed at (installer) |
|---|---|---|---|---|
| `nvngx_dlssnr.dll` | 310.8.0.0 (NVIDIA DLSSNR, DVS PRODUCTION) | `e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a` | not needed to compile | beside `sli_installer.exe` — the installer copies it into the game folder |
| `nvngx.dll_dlssnr.dll` | (forwarder shim, unversioned) | `092d004e9ec0bc0d277df479eddbb94ac791c477a6cf1ac9cf46fdd41a73fb61` | not needed to compile | beside `sli_installer.exe` — same |
| `external/nvngx_dlss_sdk/nvsdk_ngx.h` | NGX SDK (sdk version 0x15 API) | `b3867e9381c458fa0e407b127de71ef7f16a05e043360d4bead20b5ca5a04938` | `external/nvngx_dlss_sdk/` (compile fails without it) | not needed |
| `external/nvngx_dlss_sdk/nvsdk_ngx_defs.h` | — | `bf4f5e7f89eb98bf116b539c408a3b03a59cd413070b7fd859de1a3c4adf1f07` | same | not needed |
| `external/nvngx_dlss_sdk/nvsdk_ngx_params.h` | — | `dd9ef57ac264256ddd63b3c75b2894f519c72a1a9c9c23814fb433aa71478ada` | same | not needed |

Where to get them, from your own machine (no redistribution):

- **Headers** — the NGX SDK headers ship inside the NVIDIA DLSS SDK
  (`NVIDIA NGX SDK`, developer.nvidia.com/ngx/dlss-sdk) and are also
  mirrored in the public source trees of DLSS-related open-source mods
  (e.g. OptiScaler). Copy the three `nvsdk_ngx*.h` into
  `external/nvngx_dlss_sdk/`. The hashes above are the headers this
  project was built against; other header versions usually compile but
  are not the tested set.
- **Runtime** — `nvngx_dlssnr.dll` is NVIDIA's DLSS-NR model container,
  delivered by the driver to games that request the feature (search the
  driver's NGX store: `<NGXPath>` from
  `HKLM\SYSTEM\CurrentControlSet\Services\nvlddmkm\NGXcore\NGXPath`, or
  a game install that ships it). `nvngx.dll_dlssnr.dll` is the forwarder
  shim the runtime's caller-gate needs (its filename must contain
  `nvngx.dll`). Both go beside the game's exe / the installer.
  Verify by hash before debugging anything (cross-gen builds share the
  filename; wiki/07 documents how to tell them apart).

To compile the project you need ONLY the three headers. The two DLLs are
needed at RUNTIME (deployed next to the game's exe).

## A note to NVIDIA

This repository distributes none of NVIDIA's software. The runtime is
user-provided from the user's own machine, and the SDK headers are
referenced by hash only. If anything here nonetheless oversteps an
NVIDIA policy or directive, I apologize in advance: it is unintentional.
This is a non-commercial personal laboratory with no revenue or benefit
of any kind attached to it — contact me and I will address the
correction in the shortest possible time.

## License

MIT (see LICENSE). Vendored third-party code under its own license:
**Dear ImGui** (MIT, `external/imgui/LICENSE.txt`) and **Microsoft
Research Detours** v4.0.1 (MIT, `external/detours/`).

The gain-composition math and the matched-residual pattern were
developed by studying the public source of the DLSS-NR mod forks listed
under Reference projects; no GPL code from them is included in this
tree. Not affiliated with NVIDIA/AMD/Intel.
