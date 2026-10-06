# AMD-NR---OptiScaler (3zwr1) — examen completo y qué nos aporta a new-sli

Fecha: 2026-09-29 (noche). Clonado en `D:\proyectos\AMD-NR---OptiScaler` (261★, creado 2026-09-20, GPL-3.0, v0.3.4.2).
Licencia: GPL-3.0 (patrón sí, código no — igual que el fork GPL). El runtime **danielblnc** es cerrado (© Daniel Blanco, se envía aparte en `Runtime.zip`); el runtime **lmxxf** es open (red de Kien, MIT) con kernels RDNA4 de AMDNR y backend RDNA3 propio.

## 0. Qué contiene el repo realmente

- `main/AMDNR-source/` — OptiScaler casi completo (95 .cpp/.h en el repo): proxies, upscalers (dlss/dlssd/ffx/fsr2/fsr31), wrapped swapchain, menú, Config.
- **FALTA `dlssnr/` entera**: `OptiScaler.vcxproj` lista `dlssnr\amd\AmdPreSr.cpp`, `AmdBridge.cpp`, `RtgiNative.cpp`, `lmxxf\LmxxfBackend.cpp`, `dlssnr\menu\*.cpp`… y no están. `wrapped_swapchain.cpp` incluye `<dlssnr/amd/InterleavePacing.h>` (no compila tal cual). Los comentarios de `Config.h` citan `NrCompose.h`, `EditShapeRules.h`, `PresentExperimental.h`, `LateSubmitGrace.h`, `ComIdentity.h` — todos ausentes. **El host del pase neural es closed-source en la práctica; solo vive en el DLL.**
- Lo que SÍ se publica y es oro puro: `Config.h` (72+ keys `[DlssNr]`/`[Amd*]` documentadas inline con mediciones y razones), `CHANGELOG.md` (1636 L de decisiones), `Issues.md`, `Features.md`, NOTICE/RDNA3 docs, y el Launcher WPF C# completo.
- Runtimes: danielblnc `dlssnr_amd_pass1..3.dll` + weights (cerrado, 3 DLLs = multipass); lmxxf `LmxxfNrRuntime.dll` + `.pak` (HIP, kernels por arquitectura).

## 1. Arquitectura: dónde engancha el pase (dos placements)

- **Path NVIDIA** (lo que el README llama "driven directly through its snippet", feature 18): por defecto **post-upscale** — sobre el output del SR dentro del evaluate hookeado (el placement del fork GPL que ya mapeamos). `DlssNrRunBeforeSr=false` "preserves the v0.2.0 post-upscale placement" (`Config.h:286-288`).
- **Path AMD** (ambos runtimes): **SIEMPRE pre-SR** — el pase reemplaza la textura Color que el upscaler va a consumir: `DlssNr::AmdBridge::HasReplacement(InParameters)` en `FFXFeature_Dx12.cpp:142-169` — "The AMD replacement arrives in COMPUTE_READ and must remain there. Game-specific barriers describe the original texture, not this scratch UAV". Con Ray Reconstruction corre tras RR automáticamente; `ApplyAfterRR`/`RRPasses`/`RRWorkingScale` son keys NVIDIA-only (`CHANGELOG:1400-1403`).
- **Modo final-image** (`AmdFinalImage`, `Config.h:658-662`): "run the neural pass on the swapchain back buffer at Present, for games with FSR 1 or no upscaler… No motion, no depth, no history; HUD included" — **el equivalente exacto de nuestro Present-gate**, que ellos tienen como experimental off-by-default. Validación independiente de que ese hueco existe.
- `DlssNrProxyProbe` (`Config.h:884-895`): comprueban si el `nvngx.dll` **del driver** ya sabe despachar feature 18, para eliminar el forwarder — "the model refuses callers whose module path does not contain 'nvngx.dll'" (idéntico a nuestro hallazgo; nosotros YA somos nvngx.dll).

## 2. El modelo de composición del residual (lo más valioso para nosotros)

Todo en `Config.h` con defaults y porqués explícitos:

- **`DlssNrTransferStrength` / `DlssNrColourStrength`** (`Config.h:810-813`): separan **edición de luma** de **desviación de color** — "detail synthesis is a luminance edit and any colour shift is usually the part you do not want, and allowed past 1.0 because exaggerating an edit is the only honest way to see whether there is one" (nuestra misma filosofía del boost>1).
- **`DlssNrMaxRatio { 2.0 }`** (`Config.h:806`): "The most the pass may multiply or divide a pixel by. A detail pass has no business restyling a light source" — **clamp de ratio [1/2, 2]**, MUCHO más tight que nuestro [1/8, 8] del R81. Dato para afinar.
- **`AmdResidualLimit { 0.25 }`** bajado de 0.5 con medición (`Config.h:575-584`): "the network works in tiles, and a tile where it extrapolated rather than saw returns something nothing like the rest. dlss5-neural-amd measured one run at a **mean of 0.072 and a MAXIMUM of 4.16**… every extra pass runs on top of the last one's blown tile, so passes compound the outliers instead of averaging them away. At 0.5 a clipped outlier still moves its pixel by half its own brightness — a visible blotch". **Explica nuestro grano dominante en tint=1: tiles extrapolados, no ruido blanco.** Y justifica un clamp de edición MUCHO más agresivo que el que tenemos.
- **`AmdResidualFade` 0..0.25**: fade del edit hacia los bordes de pantalla solo cuando el NR corre por debajo de 100% (bordes = zona sin datos reales).
- **`AmdResidualTemporal`** (`Config.h:648-651`): "Temporal smoothing of the model's EDIT rather than of the picture, with the model running every frame" — la idea de nuestro struct-EMA pero como **control de calidad**, no solo vista.
- **`AmdEveryFrame { true }`**: "Every-frame is the only configuration under test" — su filosofía same-frame es la nuestra.
- **`DlssNrHoldFrame`** (`Config.h:800-803`) + `DlssNrApplyModel` (`Config.h:796-798`): congelar la entrada del pase para re-renderizar **EL MISMO frame** al cambiar settings ("the only clean way to A/B our settings") y apagar la aplicación del modelo sin parar el pase. **Esto resuelve nuestro problema de A/B con la cámara moviéndose** — candidata inmediata a portar al panel.
- **`DlssNrWorkingScale { 1.0 }`** (`Config.h:873-876`): "The fraction of the frame's resolution the model works at. **The frame itself is never reduced — only the model's contribution is computed small and enlarged**" + `DlssNrScalingDownscaler Lanczos3` cuando >1. Es exactamente la semántica de nuestro `workScale` (que ya teníamos), con downscaler nombrado.

## 3. Guards de highlight y exposición (nuestra sospecha #2 confirmada por terceros)

- **HighlightChromaGuard + AutoExposureHighlightCap** (`Config.h:537-547`, ON por defecto, "grey highlights in Silent Hill 2 are fixed"): donde la copia que ve la red pasa el hombro del codec (canal máx >0.75, pleno desde 1.5), "the shoulder squashes such a pixel toward white and the decode hands it back grey" → la respuesta **conserva su luz y toma el color del juego**. El auto-exposure deja de subir con >8% de samples sobre 0.75 y puede bajar hasta 4x.
- **`AmdHighlightProxy`** (`Config.h:318-327`, medido en Forza): la red devuelve highlights a ~la mitad — comprime lo que ve como HDR lineal. Solución: squeeze reversible sobre el knee antes de la red y "its answer is brought back **with each pixel's ORIGINAL scale (RenoDX's principle): exact wherever the model changes nothing**" — **el mismo principio ratio/gain que acabamos de implementar en R81, validado independientemente**.
- **`DlssNrWhitePointFromExposure { true }`** y el white-point medido OFF porque el meter se mide a sí mismo ("1545 samples… 57 jumps beyond 1.5x in a single frame… the picture pumping and occasionally flickering", `Config.h:830-841`) — trampa documentada de auto-exposición en bucle. Nuestra mediana con lag 1f evita ese bucle por diseño.
- **Encoding sRGB/Gamma2.2** (`AmdEncoding`, `CHANGELOG:528-531`): decodificar el color del juego a lineal antes de la red y re-encode al salir.

## 4. Interleave (perf del MODELO, no de la entrega)

- `AmdInterleave` (`Config.h:407-416`): correr la red cada N frames (N puede ser fraccional: 1.4 = salta ~2 de cada 5) y rellenar por reproyección temporal. `AmdInterleaveAdaptive`: cada frame cuando la imagen cambia, interleave solo quieta. Fill modes: 0 = historia pura re-proyectada (fill nunca difiere del frame-modelo; ghosting como único coste), 1 = smart fill (rompe el ghost pero "cadence-locked flicker").
- **Compensación de jitter en el fill** (`AmdJitterSign`, `Config.h:556-559`): "prev = p + motion + jitterPrev − jitterCur… the raw shifts by up to 0.8 px between consecutive frames standing still" — sin esto, wobble sub-pixel a media cadencia. Con signo configurable porque los juegos discrepan.
- Ghosting arreglado "at the source" (v0.2.0, `CHANGELOG:1621-1623`): reactive mask, velocity dilation, **clamped residual upsample**, bounded carried edit.

## 5. Suite de verificación en pantalla (paralelo directo con nuestros knobs)

| AMDNR | new-sli hoy | Nota |
|---|---|---|
| `DlssNrDebugView` 0-3 (off/input/raw/amplified) | tint 0/1/2 | idéntico concepto; su modo 3 "amplified" ~ nuestro boost |
| `DlssNrCompare` 1 side-by-side / **2 wipe** + `CompareSplit` + `CompareSwap` + **`CompareTags`** (etiquetas dibujadas en el frame) | split permille + checkbox | el **wipe** corta un frame sin resamplear (el nuestro también); faltan tags y swap |
| `DlssNrHoldFrame` (congela input, vives cambiando settings sobre EL MISMO frame) | — | **port inmediato recomendado** |
| `DlssNrApplyModel` (pase vivo, frame limpio) | F7 offload/native | equivalente |
| `AmdNetworkOutput` (respuesta cruda, sin composition tail) | — | "settles an argument instead of continuing it" |
| Capture key → 8 frames a carpeta time-stamped | replay_feeder | mismo patrón |

## 6. Trampas de ingeniería documentadas (confirman nuestras R78-R81)

- **Compute wait forzado** (`AmdSpinDraw=0`, `Config.h:652-665`): graphics wait con draws de 1px "disturb rasteriser and output-merger state" y el admission nunca aprueba una lista cuyo root signature gráfico no conoce — lo dejaron experimental. Nosotros: coherente con nuestra regla de no tocar estados del juego.
- **List discard recovery** (`AmdNeuralListRecovery`, `Config.h:668-674`): una cmdlist del juego con Record del pase que se descarta sin ejecutar dejaba el runtime esperando **el resto de la sesión**. Watch de `Reset` por puntero.
- **OneStreamPerFrame** (`Config.h:681-686`): split-view/PiP = múltiples entries por frame presentado → NR solo en la mayor, las otras pasan intactas.
- **Device gate** (`AmdDeviceGate`): NR solo en el device donde se construyó su backend — frame grabado en otro adapter pasa intacto.
- **VRAM pooling de runtimes cerrados**: danielblnc ~75-350 MB **por tamaño neural nuevo** (nunca libera; "restart the game after many changes"; redondean a 64px para limitar tamaños); lmxxf fuga en AMD HIP al cambiar tamaño (0.3.3.2 reusa buffers). Precedente de nuestro recycle WS>5GB.
- **Late submission grace** (TLOU2, job-system engines): "the game submits a frame's command list only after the next frame's Evaluate" — guardan el job 1 frame contra una copia del frame alimentado en vez de dropear y resetear historia.
- **Barriers del replacement** (`FFXFeature_Dx12.cpp:142-169`): el scratch UAV llega en COMPUTE_READ y debe salir en COMPUTE_READ; los barriers del juego describen la textura original, no la nuestra.

## 7. Qué aplicamos a new-sli (rankeado por impacto)

1. **Clamp de edición por tile-outlier** (de `AmdResidualLimit` + medición 0.072/4.16): nuestro [1/8,8] es demasiado generoso — los "blown tiles" pasan enteros. Propuesta: `maxRatio` efectivo [1/2,2] (`DlssNrMaxRatio` default) + clamp de la EDICIÓN (no del ratio) tipo 0.25. A/B medible con split.
2. **Separar luma/color del gain** (`TransferStrength`/`ColourStrength`): el gain RGB completo deja pasar shifts de color que un detail-pass no debería causar. En nuestro compose: gain en luma + atenuar croma del delta.
3. **HoldFrame en el panel** (congelar frame N, mover knobs, re-componer el MISMO frame): elimina la cámara de la ecuación del A/B — con nuestro Present-gate es directo (el gate ya re-compone lo que le pidas).
4. **Temporal smoothing del edit como knob de calidad** (no solo vista): EMA del gain con invalidación — el struct_accum que ya escribimos, promovido a control.
5. **Fade del edit en bordes** cuando workScale<1 (0..0.25).
6. **Tags en el split** (qué lado es cuál en screenshots) + swap de lados — trivial en el PS.
7. Guardas de highlight croma (cuando midamos los dumps R80).

## 8. Perfiles del modelo (contexto)

`DlssNrPreset/Style` (0 standard/1 natural/2 cinematic), `Pass2/Pass3*` overrides heredados, `LocalStructure/LocalTone`, skin: `AutoMask` (máscara nativa del runtime), `SkinStructure -1 = follow local structure`, `SkinDetail/Colour` + `Environment*`, `ShowSkinMask` (nuestro skin-port pendiente mapea 1:1 con estas keys). En danielblnc: `AmdRuntimeStyle`, `AmdToneCurve` (Reinhard/ACES), `AmdToneLift`, `AmdUseGameExposure`.
