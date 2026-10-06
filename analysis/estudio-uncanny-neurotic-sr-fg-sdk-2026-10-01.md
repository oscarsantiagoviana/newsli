# Estudio de referencia: UNCANNY, NeuRotic e inventario SDKs SR/FG — para new-sli (offload NR cross-process)

Fecha: 2026-10-01. Fuentes verificadas en disco. Citas cortas y literales. Sin copiar código GPL.

Fuentes principales:
- UNCANNY ZIP v0.20.0-alpha.2 FINAL ALPHA extraído (verificado presente): `<temp>/uncanny_zip/UNCANNY-v0.20.0-alpha.2-FINAL-ALPHA\` (= ZIP de la release final alpha del repo UNCANNY). Abajo abreviado `UNC-ZIP`.
- Repo público docs (subconjunto web): `D:\proyectos\UNCANNY\` (README, RELEASES, docs/{STRATA-PREVIEW, COMPATIBILITY, DLSS5-TOOLS-AND-FEEDERS, HOTFIX11-D3D11}.md).
- NeuRotic: `D:\proyectos\NeuRotic-an-OptiScaler-DLSSNR-fork\` (abreviado `NR`).

---

## 1) UNCANNY

### 1.1 ¿Hace SR además de NR? — NO. Es remasterización de imagen, no upscaler temporal
- `UNC-ZIP\README.md`: "UNCANNY is a Windows real-time graphics remastering runtime for games and emulators." Sus fixtures verificados son geometría/materiales/agua: "D3D11 rigid-geometry fixture changes actual vertex/index buffers and rasterized depth", "Normal-only recovery changes real normal buffers".
- Todo el pipeline neural es el "DLSS 5 / Feature 18" provider opcional (nvngx_dlssnr), NO un SR propio:
  - `UNC-ZIP\docs\DEEP-NEURAL.md`: "1–3 actual Feature-18 evaluations… Each later evaluation sees a different prepared input and weaker settings" — refinement neural sobre la misma resolución, no upscale: "All stages use the same full-frame dimensions and 100% work scale. No verified provider ROI, cropped/tiled evaluation or reduced-resolution later-stage contract was established."
  - `UNC-ZIP\docs\AMD-ENGINE45.md`: "DLSS 5 / Feature 18 is an NVIDIA provider path. When an AMD or Intel rendering adapter is positively identified, UNCANNY reports DLSS 5 as unavailable instead of attempting to present it as a cross-vendor feature." → sin NVIDIA, UNCANNY sigue (image path vendor-neutral: "A detected AMD adapter does not disable base UNCANNY processing") pero sin neural.
- Único "upscale" en el producto: REVENANT (assets/texturas offline vía Real-ESRGAN externo), no per-frame: `UNC-ZIP\docs\PC-ASSETS.md`: "The external Real-ESRGAN runner performs actual reconstruction… Output must decode at 4× dimensions and pass complete mip-chain… validation before upload."

### 1.2 ¿Tiene FG? — NO
- Búsqueda de "frame generation / dlssg / fsr" en todo el ZIP (md/json/txt): **cero resultados**. No hay FG en UNCANNY, ni propio ni puenteado.

### 1.3 ¿Qué es ELYSIUM?
- Es el nombre del motor (UNCANNY Engine 4.5), no una técnica: `UNC-ZIP\CURRENT_RELEASE.json`: `"engine": "UNCANNY Engine 4.5 — ELYSIUM"`, `"buildId": "elysium45-hf18.14"`, `"abi": 143`.
- `UNC-ZIP\docs\ENGINES.md`: tabla de motores — ELYSIUM (ABI 143, actual) y "UNCANNY 2.5 - Lucid" (ABI 136 preservado); "Engine builds are not quality presets… Binary engine switching is performed while the target is closed so installed files can be hash-checked and rolled back safely."
- `UNC-ZIP\CHANGELOG.md` (v0.20.0-alpha.1, Adaptive Realism): "Adaptive Realism is vendor-neutral and does not require DLSS 5, CUDA or Tensor Cores." ELYSIUM = adaptación de escena (exposición/contraste/saturación bounded) + guards + puente neural opcional.

### 1.4 Contrato de latencia / pacing (lo que describen los docs)
- **Warmup nativo obligatorio (fail-open)** — `UNC-ZIP\docs\HOTFIX11-D3D11.md`: "For a direct D3D11 swapchain, the first three native presentation opportunities are warmup frames. UNCANNY image preprocessing and in-frame Control Deck composition are deferred so the game's original Present/Present1 can establish visible output first." Principio explícito: "An enhancement layer should never be required for the base game to display its first frame."
- **Promoción neural por salud del stream** — mismo fichero: "Feature-18/DLSS5 interop is deferred until… native presentation is advancing, at least 8 successful Presents have completed, the last successful Present is recent." Y: "UNCANNY does not substitute a stale neural result simply because neural startup is delayed."
- Versión más estricta aún — `UNC-ZIP\CHANGELOG.md` HF18.11: "Requires 240 successful ELYSIUM frames, recent/advancing native Present, and a 9-second post-transition window before neural promotion." + "Keeps neural shared-resource/provider warmup off Present." + "DXGI ownership-changing transitions revoke promotion and re-arm stabilization."
- **Ventana de cadencia medible** — `UNC-ZIP\docs\FIX5-ACCEPTANCE.md`: "Allow the rolling cadence window (up to 2048 presents) to refresh after changing modes… Compare source-copy, flow, neural, image and history GPU scopes, CPU submission cost and cadence median/p95/p99… Stale/unavailable GPU times are reported as -1, never zero cost."
- **Coste medido, no asumido** — `UNC-ZIP\docs\DEEP-NEURAL.md`: "Budget decisions use actual measured available neural-call costs, measured VRAM budget/usage, queue availability, sampled scene confidence, source residual cue and observed CPU presentation cadence… A stale/absent measurement, low confidence, missing cue, reset, pressure or provider failure retains primary-only or last-good neural output."
- **Setup off-Present** — `UNC-ZIP\CHANGELOG.md` HF18.12: "Moves D3D12 first-use setup off Present. Makes D3D12 resize/resource retirement nonblocking."
- Sin números de latencia absoluta: UNCANNY mide cadencia/costes por-stage, no promete ms.

### 1.5 Patrones de transporte cross-API/cross-adapter descritos
- **NT-shared texture con inversión de propiedad** — `UNC-ZIP\CHANGELOG.md` RC2 HF1: "Preserves the D3D12-owned D3D11 interop route when accepted. If OpenSharedResource1 rejects that transport, reverses only transport ownership: D3D11 creates an NT-shared RT/SRV texture and D3D12 opens it on the same adapter." + "Keeps neural output work on a private D3D12 UAV; rejected/failed neural work never overwrites the last accepted shared output."
- **Helper x64 aislado (mismo adapter, out-of-process)** — `UNC-ZIP\docs\PROVIDERS.md`: "This build places x64 provider components in `.uncanny/host64` and uses `UNCANNY.NeuralHost64.exe` for that experimental route… the new native D3D9/D3D10 fallback uses it without sharing the in-process provider."
  - Fixture del transporte helper — `UNC-ZIP\docs\REPAIR7-ACCEPTANCE.md`: "This runs an x86 client and x64 client against the real x64 helper on the same D3D12 adapter. It checks repeated exact shared-copy pixels and a resized session. It deliberately never calls a NVIDIA provider… Lack of a suitable device/transport returns UNAVAILABLE or FAIL, not invented success."
- **Aislamiento IPC del helper** — `UNC-ZIP\CHANGELOG.md` RC2 HF2: "Acknowledge helper entry/IPC before explicit System32 DXGI/D3D12 loading; remove those graphics DLLs from the helper's loader import table." + deadlines de startup "outside Present" y latch de lost-device D3D9 por device.
- **Slots de grabación GPU completados, no índice de swapchain** — `UNC-ZIP\CHANGELOG.md` Repair 8: "Select a GPU-completed D3D12 recording slot independently of current swap-chain buffer index; fence safety remains mandatory. Keep a safe bypass if every slot is busy." + ring de 3 slots: `UNC-ZIP\docs\DEEP-NEURAL.md`: "A three-slot GPU resource/descriptor/query ring uses actual caller-queue fences. In-flight slots are not rewritten. Unretired resources are retained after failure instead of being freed from a CPU frame count."
- **Telemetría de transferencia explícita** — RC1: "same-adapter x64-helper isolation, current-frame/timeout policy and explicit transfer telemetry."
- Nota: TODO el transporte descrito es **same-adapter** (cross-API y cross-bitness). No hay docs de cross-adapter (iGPU↔dGPU) en UNCANNY.

### 1.6 Ghosting Guard
- `UNC-ZIP\docs\FIX5-ACCEPTANCE.md` (sección "HUD and Ghosting Guard"): "Test UNCANNY only and UNCANNY + DLSS with Ghosting Guard off/on. Record moving and stationary camera segments, animated minimap markers, health bars, subtitles, menus and loading transitions. Look for blocks, stale pixels, flicker, trails and unwanted edge changes."
- Integrado en el pase de imagen: "Ghosting is integrated in the image pass." (mismo fichero, sección perf).
- Motor del guard — `UNC-ZIP\CHANGELOG.md`: "Motion Guard/Ghosting Guard remain authoritative. Flow confidence loss, source disagreement and disocclusion reduce/reject history; X2.5 deliberately uses the lowest temporal-history weight."

### 1.7 Warmup y reset de historia (resumen UNCANNY)
- Warmup: 3 presents nativos (D3D11), 8 presents saludables para interop, 240 frames + 9 s para promoción neural (HF18.11). Resize D3D12 no bloqueante.
- Reset de historia por evidencia: "a gap or camera reset resets the affected later history" (`DEEP-NEURAL.md`); "The selected policy is latched…"; guards de flujo reducen/rechazan historia (ver 1.6). En REVENANT/restore: "OFF restore, stale-update rejection and reset cleanup" (`UNC-ZIP\CHANGELOG.md` top).

---

## 2) NeuRotic

### 2.1 THREE-RENDER-MODES-DESIGN-REVIEW.md (completo — `NR\THREE-RENDER-MODES-DESIGN-REVIEW.md`)
- Tres rutas propuestas: "Quality = `RR -> DLSS SR -> NR`; Performance = `RR -> NR -> DLSS SR`; Private Queue = the Performance route plus a creation-time private-queue experiment." (línea 7)
- **Modo game-rendered/bypass**: no existe como tal; el bypass es el orden Pre-SR vs Post-SR manteniendo EL SR del juego. El Private Queue NO es async ni bypass: "creates an internal SR feature on a private direct queue… Its own commit message calls this a one-time creation hitch avoidance. It does not submit NR evaluation per frame to that queue and does not establish GPU overlap. It does not disable or bypass RR. Therefore this experiment labels the option **Private Queue**, not Async." (líneas 17-19)
- Calidad vs coste del orden:
  - Performance (NR antes de SR) reduce píxeles del modelo pero puede romper dimensiones: hipótesis "A private queue may make NR feature creation/recreation safer on games sensitive to creation commands in the game command list; it is not expected to reduce steady-state NR model time." (línea 23)
  - **REGRESIÓN OBSERVADA (no promover)**: "live DLSS-mode changes exposed a transition defect: Quality/DLAA and Ultra Performance-to-Performance changes can produce incompatible in-flight dimensions, followed by repeated `DLSSD` evaluation failures (`0xbad00000`)." (línea 29)
  - Fallo de dimensions en Performance: "NR then ran at `3840x2160` while guides remained `1920x1080`, with approximately `16.6-17.1 ms` model time. A visible temporal ghosting/double-image artifact was reported around the player silhouette… Do not promote this experiment." (línea 30)
- **Recomendación explícita** (línea 30, final): "A future isolated repair should quarantine dispatch during resource changes, recreate only after dimensions stabilize, clear incompatible temporal history, and add independent RR/SR/NR per-frame counters before assessing image quality."
- Referencias externas (líneas 11-13): issue de Neural Upstream — "use the actual Color dimensions, bind output before feature creation, evaluate through the module that created the feature, use a UAV barrier before consumption, and defer/fence destruction across resolution changes."

### 2.2 Modos de render reales (0.9.6) — `NR\ALPHA-0.9.6.md` y `NR\docs\WHAT-NEUROTIC-ADDS.md`
- Tres rutas user-facing: "Native Temporal retains the game's temporal inputs… Present Image-Only processes the final presented image without Native depth or motion guides. Present Enhanced processes the final presented image while using a fresh matched pair of captured Native depth and motion guides when it is safe to do so." (`docs\WHAT-NEUROTIC-ADDS.md`). En 0.9.6 "Present Image-Only" se renombra **Present Compatibility** (`ALPHA-0.9.6.md` línea 162).
- Colocación Pre-SR vs Post-SR: "Native Temporal also offers a Performance placement that runs NR before the game's final DLSS Super Resolution pass, reducing the model's pixel workload while retaining the game's native jitter and mode-dependent input sizes." (`docs\WHAT-NEUROTIC-ADDS.md` línea 54). UI: "**Before upscaling** focuses on Native Temporal because the Present routes operate on the final presented image." (`ALPHA-0.9.6.md` línea 429).
- Escalas de coste en Present: "Present routes can follow the game's real native render area, run at full output resolution, or use a custom 100%, 77%, 67%, 58%, 50%, or 33% scale. The overlay reports the actual NR and output dimensions." (`docs\WHAT-NEUROTIC-ADDS.md`).
- Claves INI — `NR\docs\PRESENT-ENHANCED-RESOLUTION.md`: "Resolution keys under `[DlssNr]` are `PresentResolution`, `PresentCustomScale`, `EnhancedResolution`, `EnhancedCustomScale`. Policies are 0 Follow native, 1 full output, 2 custom." + "it never uses a DLSS preset name or stale allocation dimensions."
- Fallos cerrados (fail-closed): "Present Enhanced keeps the game HUD in the processed image and is designed to fail closed to the original frame, with a visible reason, whenever the temporal guides cannot be trusted." (`docs\WHAT-NEUROTIC-ADDS.md`).
- Bypass literal por strength=0 — `NR\docs\NR-HUMAN-FRIENDLY-UI.md`: "Zero totals bypass evaluation without releasing loaded resources and invalidate history for resumption… Apply hides every pass's effect but keeps processing and resource tracking. Present skips final conversion/copyback when hidden, preserving the exact game image; Pre-SR skips the final handoff."

### 2.3 frame-hold — `NR\OptiScaler\dlssnr\design\frame-hold.md` (design completo)
- Propósito: A/B de settings en UN frame: "A **Hold** toggle. On, it freezes the input to the Neural Rendering pass; while held, changing any of our downstream settings… re-runs the model + composition on the SAME frozen frame."
- Punto de congelación (clave para nosotros): "The freeze point is the **raw colour the encode reads**, captured once at hold-on into a persistent texture; while held the encode reads that copy instead of the live output… (freezing the proxy alone would be wrong — settings must still re-encode)." + congela también el white point (snapshot) porque "the meter still dispatches; its value is simply ignored while held".
- **Qué es imposible en hold (límites honestos)**: "DLSS SR / upscaler presets, FSR/XeSS choice, and anything UPSTREAM of this pass… Re-running a temporal upscaler on one frozen frame degenerates (no history/motion), so it is out of scope… The game's own post-process, tonemapper, and UI/HUD… keep updating… Temporal behaviour (ghosting, accumulation)… only shows in motion cannot be evaluated held."
- Historia del modelo: "v1 does NOT reset: the input is identical each frame so the model converges to a steady picture… v2 (follow-ups): reset model history each held frame (snap instead of morph); freeze depth + motion guides too."
- Guards: "Default off ⇒ byte-identical… allocated on the transition to held and released/parked on hold-off… rebuilt if the output's size/format changes while held (same rule as the guide clones). Inert when NR is off."

### 2.4 SR bypass y pérdida de calidad del SR
- No hay doc titulado "SR bypass": el bypass de SR en NeuRotic es implícito — rutas Present procesan la imagen final ya-upscaled (sin tocar SR), y Pre-SR inserta NR antes del SR del juego (el SR siempre corre). Lo más cercano a "pérdida de calidad del SR": el artefacto documentado en THREE-RENDER-MODES (ghosting/doble imagen por mismatch de dimensiones NR/guides, 2.1) y la dependencia de historia: `NR\PRESENT-HISTORY-STABILITY.md` — "the white and rapid Present flicker is principally caused by sending `Reset=true` to Feature 18 for every processed image." Contrato: "The first admitted frame after an enable/resume/route transition, target-signature change, or interruption sends `Reset=true`. A successful original Present promotes that output to history; subsequent uninterrupted frames send `Reset=false`."
- Pre-SR soft reset (cámara) — `NR\docs\enhancements\presr-soft-reset.md`: "Only the native D3D12 SR passthrough explicitly enables the internal authoritative entry flag. DX11 bridges, replacement upscalers, RR, Present, Vulkan and Post-SR do not opt into this policy." Reset-burst: "The selected policy is latched at the beginning of a Reset burst". Experimento origen `NR\EXPERIMENT-0.9.5-PRESR-SOFT-RESET.md`: "NR can reset and evaluate on that frame without retiring the feature or scratch resources… while still discarding old-scene temporal history."
- RR-aware: "Its Performance route steps aside at an incompatible reconstruction boundary" (`docs\WHAT-NEUROTIC-ADDS.md`) — el NR cede ownership donde RR lo exige.

### 2.5 FG en NeuRotic (no-NVIDIA y nativa)
- Tabla de capacidades — `NR\docs\PRESENT-FG-RR-PHASE-A.md`: FSR FG y XeSS FG: "none / untested / undeclared / unavailable — Experimental, not certified". DLSSG nativo/Streamline: observación solamente, "Inconclusive; inherited 0.9.5 behavior".
- Candidato viable con FG NATIVA — `NR\docs\CYBERPUNK-COMBINED-RR-FG-CANDIDATE.md`: "Present NR publishes the exact backbuffer copyback completion to native FG, whose command-list submission waits for that completion. Missing identity, lifecycle changes and failed dependencies remain fail-closed without a CPU wait." Regresión aprendida: "237 native-FG evaluation failures began immediately after two NR-Off transitions, producing a looping frozen image. Cycling the game's FG state cleared the stale completion record." → **invalidar handoffs de completion en transiciones NR-Off antes del siguiente Present**.
- Ciclo de vida FG — `NR\CRIMSON-FG-LIFECYCLE-EXPERIMENT.md`: "Hypothesis: FG teardown/recreation before NR activation exposes a lifecycle or queue ownership failure absent when the initial FG instance is retained." + límites de la observación: "`nr-fence-signaled` means the queue accepted a signal. Only a compatible completion poll proves that fence value completed; none of these proves the FG consumer accepted that dependency."
- Pacing — `NR\INTEGRATION.md`: componente "Present Image-Only/pacing"; test: "run >=900 eligible samples after 32 warm-up calls; switch Present → Native → Present with FG off" con gate "model and composite counters rise together; fallback streak remains zero".

---

## 3) Inventario SDKs SR/FG en disco

### 3.1 Submódulos declarados pero NO descargados (carpetas vacías)
- `.gitmodules` de todos los clones OptiScaler/NeuRotic declaran: `external/FidelityFX-SDK` y `external/FidelityFX-SDK-v2` (→ github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) y `external/xess` (→ github.com/intel/xess). Verificado: `D:\proyectos\OptiScaler\external\{FidelityFX-SDK,FidelityFX-SDK-v2,xess}\` y los equivalentes en NeuRotic/optiscaler_contrib/OptiScaler-Aurora/etc. están **vacíos** (submodule no inicializado). No hay headers ffx_*.h en ningún sitio.
- Lo que SÍ hay de SDKs en los clones: `external\nvngx_dlss_sdk\` (headers NGX), `external\streamline\` (headers sl_*.h, incl. `sl_dlss_g.h`), `external\latencyflex\latencyflex.h` (Apache 2.0, "Licensed under the Apache License… Copyright 2021 Tatsuyuki Ishi").

### 3.2 Binarios FSR/FG y XeSS presentes (con versión, vía VersionInfo)
Dirigidos a RDR2 offload: `D:\proyectos\old\osvnewg\deploy\rdr2-offload\OptiScaler\`:
- `amd_fidelityfx_loader_dx12.dll` — **2.3.0.2740** (FidelityFX SDK 2.x loader)
- `amd_fidelityfx_upscaler_dx12.dll` — (FSR upscaler, mismo paquete)
- `amd_fidelityfx_framegeneration_dx12.dll` — **4.0.1.2740** (FSR FG)
- `amd_fidelityfx_vk.dll` — **1.0.1.41314**
- `libxess.dll` — **2.0.2.68**; `libxess_dx11.dll`; `libxess_fg.dll` — **1.3.1.78** (XeSS FG)
- `dlssg_to_fsr3_amd_is_better.dll` (Nukem9 bridge), `dlssg_sm86\`, `dlss-enabler-headless.dll`, `streamline\`, `nvngx.ini`
- Otro ejemplar XeSS 2.0.2.68: `D:\proyectos\old\1-Click-DLSS5\core\payload\optiscaler\libxess.dll`.
- Bridge FG: `D:\proyectos\dlss-unlocked\DLLSG mod\dlssg_to_fsr3_amd_is_better.dll` + `nvngx.dll` (proxy).

### 3.3 Licencias (texto en disco)
- **FSR 3.x / FidelityFX (effects)**: MIT-style — `D:\proyectos\dlss-unlocked\Licenses\AMD_FidelityFX_LICENSE.txt`: "Permission is hereby granted, free of charge… to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies…" (© 2023-2025 AMD). Usable sin problema.
- **FidelityFX SDK v2 / FSR 4**: EULA binario-only — `D:\proyectos\AMD-NR---OptiScaler\FidelityFX_v2_LICENSE.md`: "REDISTRIBUTION: …in binary form only… No reverse engineering, decompilation, or disassembly of this Software is permitted."
- **XeSS**: Intel Simplified Software License (Oct 2022) — `D:\proyectos\AMD-NR---OptiScaler\XeSS_LICENSE.txt` (igual en dlss-unlocked\Licenses\Intel_XeSS_LICENSE.txt): "provided in binary form only, without modification… No reverse engineering, decompilation, or disassembly… nor any modification or alteration of the Software or its operation at any time, including during execution." → **prohibido inyectar/hookear dentro de libxess**; solo uso tal cual vía su API.
- **dlssg-to-fsr3 (Nukem9)**: GPLv3 — `D:\proyectos\AMD-NR---OptiScaler\dlssg-to-fsr3_ATTRIBUTION.txt`: "Licensed under the GNU General Public License v3… the source is at the repository above." → no copiar código; el binario redistribuible según su licencia propia.
- **OptiScaler / NeuRotic**: GPLv3 (`NR\LICENSE`, OptiScaler LICENSE). **UNCANNY: closed source** — "UNCANNY is closed source. The public repo is for releases, docs and support material." (`D:\proyectos\UNCANNY\README.md`); paquete "restricted to compiled release binaries, installer/runtime scripts, configs, documentation and third-party notices" (`UNC-ZIP\README.md`). Solo leer docs, cero código.

---

## 4) Patrones aplicables a new-sli

### 4.1 Entrega de frames a resolución final SIN SR
- **Fail-open absoluto**: primeros N presents 100% nativos antes de tocar nada (UNCANNY: 3 warmup frames; "an enhancement layer should never be required for the base game to display its first frame"). El compose/overlay también se difiere.
- **Promoción por salud, no por tiempo**: gate = presents advancing + N exitosos + último reciente (8 / 240 frames / 9 s según ruta). Si no hay salida nueva, se entrega el frame fuente vivo: "keeps the live source visible while neural startup is deferred; no stale neural result is reused."
- **Skip del copyback si no se procesa**: NeuRotic — "Present skips final conversion/copyback when hidden, preserving the exact game image" → el camino sin procesar debe ser byte-identical (regla "Default off ⇒ byte-identical" de frame-hold).
- **Bypass por cero coste**: strength 0 = no evaluar pero mantener recursos e invalidar historia para reanudación limpia (NeuRotic Basic profile).
- **Contrato de admisión estricto antes de tocar el swapchain**: formato SDR/flip/single-sample/rectángulos completos; si falla, frame original intacto y descriptor logueado (`NR\PRESENT-COMPATIBILITY.md`: "Admission fails before model work for HDR… dirty rectangles or scroll updates, multisampling, non-flip swapchains… An admission failure leaves the original target unchanged.").
- **Reset de historia solo en frontera real**: primer frame admitido tras transición/interrupción → Reset=true; frame original completado promueve historia; cualquier fallo invalida (`NR\PRESENT-HISTORY-STABILITY.md`). Soft-reset para cortes de cámara sin destruir feature (`presr-soft-reset.md`).

### 4.2 Optimización de transporte/copias cross-adapter / cross-API
- **NT-shared texture como transporte único, con fallback invirtiendo propiedad** (D3D12-owned ↔ D3D11-owned, same adapter) — patrón directo replicable.
- **NUNCA sobrescribir la última salida buena**: salida neural a UAV privado; fallo → se conserva la última compartida aceptada (UNCANNY HF1).
- **Slots por completado GPU, no por índice de buffer**: ring de 3 slots con fences del caller-queue; "In-flight slots are not rewritten"; bypass si todos ocupados; recursos no retirados se retienen tras fallo (no free por contador de CPU).
- **Helper fuera de proceso**: aislado del loader (sin DXGI/D3D12 en import table hasta ack IPC), deadlines fuera de Present, FAIL/UNAVAILABLE explícito en vez de éxito inventado; fixture de píxeles exactos del shared-copy + resize.
- **Completions reales, no suposiciones**: GPU-safety de NeuRotic — "replacing frame-age assumptions with actual submission completion"; cookies refcounted en command lists/fences; "Reset may happen before GPU completion: the ticket survives that Reset"; device loss (UINT64_MAX) nunca cuenta como completion; sin CPU waits en el camino normal.
- **Coste de transporte medido por stage**: "D3D11's neural scope includes preparation and cross-API synchronization" — separar prep/sync del tiempo de modelo en telemetría; -1 para stale, nunca 0.
- **Setup/allocación fuera de Present**; resize/retiro no bloqueante; invalidación de handoffs de completion en cada transición (lección FG Cyberpunk: los completion records stale → FG en bucle congelado).
- **Barriers/ownership del upstream**: "bind output before feature creation, evaluate through the module that created the feature, UAV barrier before consumption, defer/fence destruction across resolution changes" (THREE-RENDER-MODES, referencia externa) + "quarantine dispatch during resource changes, recreate only after dimensions stabilize, clear incompatible temporal history".

### 4.3 Incorporación de FG no-NVIDIA
- Estado del arte en forks: **FSR FG / XeSS FG existen como binarios pero están sin certificar** en NeuRotic ("untested / undeclared / unavailable — Experimental, not certified"); el único camino FG probado end-to-end es publicar completion de copyback al FG nativo con espera en command-list y fail-closed sin CPU wait (Cyberpunk candidate). Patrón exportable: **contrato de completion publicada + wait en GPU, nunca CPU, + invalidación en NR-Off**.
- Bridges disponibles en disco (licencias OK): FSR 3.1 FG = MIT (amd_fidelityfx_framegeneration_dx12 4.0.1), XeSS FG 1.3.1 binario-only (sin tocar su ejecución), dlssg-to-fsr3 GPLv3 (usar binario, no código). Pacing/latency: LatencyFlex header Apache 2.0 disponible; OptiScaler histórico hace pacing propio de FG ("Fixed OptiFG frame pacing (one frame wasn't rendering)" — `NR\Changelog.md`).
- Requisitos que imponen los forks (aplicar en new-sli): retenida de la instancia FG inicial (no teardown/recreate antes de activar el consumidor — Crimson experiment), HUD en backbuffer final (sin política HUDless inventada), clasificador de frames generados pendiente (NeuRotic lo dejó "unavailable").

### 4.4 Posturas de calidad (para decidir default de new-sli)
- NeuRotic THREE-RENDER-MODES recomienda NO promover el orden Performance (Pre-SR) sin: quarantine en cambios de recursos, recreate tras estabilizar dimensiones, clear de historia incompatible y counters independientes RR/SR/NR. La ruta conservadora validada = procesar imagen final (Post-SR/Present) con historia estable.
- Frame-hold marca el estándar de verificación: default-off byte-identical, freeze en la fuente cruda del encode, límites honestos de qué no se puede evaluar en un frame.
