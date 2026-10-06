# Mapa de pipeline NR — fork OptiScaler-DLSSNR-PreSR-Multipass

Fecha: 2026-09-29. Árbol analizado: `D:\proyectos\OptiScaler-DLSSNR-PreSR-Multipass\OptiScaler\`.
Todas las citas son `archivo:línea` sobre los archivos reales leídos. Sin especulación: solo lo que el código dice. Los fragmentos se describen por patrón, no se copian.

Fuentes primarias leídas: `shaders/dlssnr/DlssNr_Dx12.cpp` (3917 líneas), `shaders/dlssnr/DlssNr_DeferredSr.inl` (739), `shaders/dlssnr/DlssNr_Late.inl` (444), `shaders/dlssnr/precompile/dlssnr.hlsl` (1110), `shaders/dlssnr/precompile/dlssnr_residual.hlsl` (147), `shaders/dlssnr/precompile/dlssnr_finished_color.hlsl` (87), `shaders/dlssnr/DlssNr_Common.h` (315), `dlssnr/DlssNr_ExposureScan.cpp` (992), `dlssnr/DlssNr_Capture.h` (342), `dlssnr/ResidualFg.h` (178), `dlssnr/PassProfiles.h` (100), `Config.h` (sección DlssNr), `shaders/dlssnr/DlssNr_SeamClock.h`, `DlssNr_ResidualPair.h`, `DlssNr_Guides.h`, `DlssNr_ActiveColor.h`, `dlssnr/DlssNrNative.{h,cpp}` (parcial), `dlssnr/design/pre-sr-multipass.md`.

---

## 0. Vista general: dos mitades y varios modos de colocación

- El modelo NR es una **feature NGX** (feature no documentada 18 / snippet `nvngx_dlssnr.dll`) creada y evaluada, no un dispatch: `dlssnr/DlssNr.h:7-14`. La composición es un compute shader ordinario: `shaders/dlssnr/DlssNr_Dx12.cpp:1465-1528`.
- Puntos de enganche: `inputs/NVNGX_DLSS_Dx12.cpp:1180-1193` — `EvaluateBeforeUpscale` antes del SR del juego (línea 1181), el EvaluateFeature nativo del juego en medio (1183-1184), `EvaluateAfterUpscale` después (1192-1193). La selección SR/RR se hace por identidad de feature: `inputs/NVNGX_DLSS_Dx12.cpp:1122-1132`.
- El enrutador de modos es `EvaluateInternal` (`DlssNr_Dx12.cpp:3110-3564`). Modos de colocación:
  - **post-SR** (clásico): pass completo dentro de `Dispatch` (`DlssNr_Dx12.cpp:1660-3023`).
  - **pre-SR** (`DlssNrRunBeforeSr`, `Config.h:262`): mismo Dispatch sobre Color en vez de Output; compatibilidad validada en `DlssNr_Dx12.cpp:3240-3288` con fallback post-SR.
  - **DeferredDLSS** (`DlssNrDeferredDlss`, `Config.h:266`): NR pre-SR → residual → SR privado → composición post-SR. Es el modo "pre-SR multipass" central; enrutado en `DlssNr_Dx12.cpp:3205-3223`.
  - **FinishedPicture** (`DlssNrFinishedPicture`, `Config.h:263`): captura guías/residual en el seam y aplica en Present sobre el backbuffer; `DlssNr_Dx12.cpp:3142-3186` y namespace `Late` (`DlssNr_Late.inl`).
  - **ResidualAcrossRR** (`Config.h:272`): NR pre-SR deja Color intacto y acumula el delta con reproyección MV para aplicarlo tras RR+SR.

Identidad de frame en los seams: `DlssNrSeamClock` (`DlssNr_SeamClock.h:10-26`) — epoch lógico que sobrevive a que Present avance entre Before y After; los bridges usan su contador de frames enviados. Nada de esto es evidencia de submission GPU (`DlssNr_SeamClock.h:3`).

---

## 1. MAPA DE FLUJO frame-a-frame (modo DeferredDLSS, el núcleo del fork)

### 1.1 Before() — seam pre-SR (`DlssNr_DeferredSr.inl:269-533`)

Orden exacto de operaciones grabadas en la command list del juego:

1. **Detección de pending huérfano**: si quedó un `pending` del frame anterior (After nunca corrió), marca `reset` — `DlssNr_DeferredSr.inl:273-277`. `ResetOnGap` (RAII, 279-290) resetea hold/half si Before sale sin armar seam.
2. **Collect()**: retira generaciones jubiladas cuyo marcador GPU terminó — `DlssNr_DeferredSr.inl:126-129`, criterio `Idle()` en 56 (array `completed` mapeado, 39-42 y 165-166).
3. **Gates de configuración** (293-307): exige ApplyModel u FinishedPicture, sin proxy-backend/hold/debug/compare; lista directa de tipo DIRECT con estado restaurable.
4. **Extracción de recursos** del bloque NGX del juego: Color, Output, Depth, MotionVectors — 308-311. `wantsHalf` = ResidualFg activo (312-313); `sampleAndHold` = quiere half pero no hay MV (314).
5. **Validaciones**: recursos distintos, depth/motion presentes (315-319); offsets de subrect a cero (320-325); extensión activa pre-SR vía `PreSrColorExtent` (326-335, implementación `DlssNr_ActiveColor.h:16-33`); identidad device/queue direct (336-353).
6. **Generación**: si cambia device/queue/tamaño/formato/flags/modo, la generación actual se jubila (`retired`, 354-360); máximo 4 jubiladas esperando GPU (363). `Allocate` (144-168) crea: `edited` (formato de entrada), `residualInput`/`residualOutput` **RGBA16F**, `clean`/`composed` (formato de salida), `exposure` **R32_FLOAT 1x1**, query heap de 16 timestamps y readback mapeado.
7. **Época**: rechaza más de un upscale por epoch de submission (381-384); reserva un slot de marcador con `Use` (385-386; struct 76-99: EndQuery+ResolveQueryData en el destructor).
8. **Creación de la feature DLSS privada** (solo la primera vez, 387-423): parámetros propios Width/Height/OutWidth/OutHeight, flags filtrados a DepthInverted|MVLowRes|MVJittered (349-353), perf-quality heredado (399-400). Comentario clave: "LDR biased carrier, constant unit exposure, no auto-exposure/sharpening… NGX is called directly" (401-402). Inicializa `exposure=1.0` con el modo UnitExposure (407-410; shader `dlssnr.hlsl:529-533` escribe 1.0) y, en sample-and-hold, la guía `zeroMotion` R16G16F a cero (413-418). Espera un epoch de submission posterior antes de evaluar (425-426).
9. **Sample-and-hold** (428-439): si el hold del frame anterior es reutilizable (epoch exactamente +1), arma `pending` con `skipNr=true` y devuelve — el residual retenido se aplicará al SR limpio actual (comentario 435). Si no, resetea el hold y usa `zeroMotion` como MV (438).
10. **Half-rate** (441-447): `PrepareHalfRate` (ver §5.3). Si el frame anterior fue ancla (`havePrevious && previousWasAnchor`), arma pending con `skipNr=true, half=true` y sale — NR y SR privado se saltan, el SR del juego corre normal (448-453).
11. **Selección de MV**: `nrMotion` = anchorMotion compuesto si half con previo, motion normalizado si half sin previo, o el MV del juego — 454.
12. **Copia de Color → `edited`** (456-462): barriers arrival→COPY_SOURCE, `CopyActiveColor` con box explícito (`DlssNr_ActiveColor.h:37-48`), y vuelta de estado.
13. **FrameInfo** (464-481): BeforeUpscale/PrivateColorCopy=true, SubmissionEpoch, RenderSubrect, DepthInverted, ColourIsLinearHdr (flag IsHDR del juego **y** formato float que puede contener HDR lineal — 470-471; helper `FormatCanHoldLinearHdr` `DlssNr_Dx12.cpp:1311-1325`), Reset (472: reset del juego o interno o sample-and-hold o job privado), MvScale del juego (473-474; en half se fuerza a w,h — 475; en hold a 1.0 — 476), PreExposure con floor 1e-4 (477), ExposureTexture (478).
14. **Evaluación del modelo NR**: `g_compose->Dispatch(cmd, g.edited, depth, nrMotion, g.edited, frame, queue)` — 482-484. Esto ejecuta el pipeline completo del §1.4 **a resolución de render sobre `edited`**. Éxito detectado por delta en `successfulDispatches` (483-485).
15. **Encode del residual** (488-507): modo `EncodeResidual` (5) o, con FinishedPicture, el shader finished-colour modo 5 (495-502). Entradas: `color` (Color del juego) y `g.edited` (NR-editado); salida `residualInput` RGBA16F a w×h. Barriers de color y residualInput; `smallReadable=true`.
16. **Bloque de parámetros del DLSS privado** (508-525): Color=residualInput, Output=residualOutput, Depth=depth, MotionVectors=nrMotion, ExposureTexture=**g.exposure (la unidad constante)**, Reset combinado, **Jitter_Offset X/Y reenviados del juego** (517-518), MV_Scale del modo, **FrameTimeDelta × 2 si half con previo** (521-522), **DLSS_Pre_Exposure=1.0** (523), Exposure_Scale=1.0 (524), Sharpness=0 (525).
17. **Armado de `pending`** {cmd, source, output, epoch, preExposure, skipNr=false, half} — 526-529. `edited` vuelve a UAV (532).

### 1.2 El SR del juego (entre Before y After)

`inputs/NVNGX_DLSS_Dx12.cpp:1183-1184` — EvaluateFeature nativo del juego, sin tocar: corre sobre el Color **limpio** (Before nunca escribió Color; solo leyó). El residual viaja por buffers propios.

### 1.3 After() — seam post-SR (`DlssNr_DeferredSr.inl:560-715`)

1. **Consumo de pending** (562-575): match por identidad de cmd/caller/output, no por timing de Present (comentario 564-565). Sin match → `reset=true`.
2. **Gate de restauración de estado** (578-580) y reserva de marcador `Use` (581-582).
3. **Evaluate del DLSS privado** (584-589): `EvaluateFeature` sobre `g.feature` con el bloque del paso 16 de Before — **el SR privado escala el residual de w×h a outW×outH**. Salvo si `skipNr`.
4. **Rama FinishedPicture** (596-604): `Late::CaptureResidual` clona el residual escalado para aplicarlo en Present y **no compone nada ahora** — "Keep the game's SR output clean" (603).
5. **Rama FG (half con ancla)** (606-643): ver §5.4.
6. **Copia de Output → `clean`** (644-649): el frame SR limpio del juego se salva antes de componer.
7. **Composición ApplyResidual** (650-676): modo 6 (o 10 con suppression). `base` = `clean` (o `h.history[prev]` con su `historyScale` si half, 658-661); `residual` = `residualOutput` (o `h.interpolated` con `suppressionTexture` en modo 10, 662-666); `apply.ExposurePreMul = pair.scale` (preExposure del frame, 654) — la misma escala que dividió en el encode, así el round-trip la devuelve.
8. **Copia `composed` → Output** (677-690): el juego recibe SR+NR. Mensaje de estado según modo (684-688).
9. **Contabilidad temporal** (693-714): sample-and-hold `SampleSucceeded(epoch)` (695); half: copia `motion→previousMotion` (701-707), `havePrevious=ok`, `previousWasAnchor=!skipNr`, contadores nrAnchors/skippedNr (708-711), alternancia `writeIndex` (712).

### 1.4 Dispatch() — el pipeline interno del modelo NR (común a todos los modos)

`DlssNr_Dx12.cpp:1660-3023`. Orden:

1. Validaciones y estados de llegada del target según colocación (1692-1699); extent activo (1715-1729); regiones de guía vía `ResolveGuideRegions` (1736-1748; `DlssNr_Guides.h:33-39` — la región válida de MV se decide por **output** aunque NR corra pre-SR, fix de cmh1448 citado en 30-32).
2. Reset del juego → invalida historial del modelo y del acumulador residual (1762-1774).
3. WorkingScale (0.25..2.0, 1829-1835): el modelo corre a resolución reducida o supersampleada; el frame nunca se reduce.
4. Rebuilds por cambio de resolución/tuning/colocación — park diferido de features/superficies (1854-1905); `ReleaseSurfacesIfFormatChanged` (799-832).
5. Creación de features (2011-2076): la feature principal se crea en un frame y **se evalúa por primera vez en un epoch de submission distinto** (2060-2075, guard 2088-2098) — "Creating and evaluating a feature in the same command list is the dice-roll that hung the GPU" (2071-2073).
6. Courier de exposición (modo 3) si el juego da ExposureTexture y la fuente es 1 (2317-2337).
7. **Encode** (modo 0, 2425-2440): ver §2.2. Guarda `colorCopy` (proxy) y `hdrCopy` (frame intacto).
8. Reducción/ampliación del proxy al tamaño de trabajo (2456-2525): box-resample exacto por área (modo 2, shader 668-726, "exact area average rather than a bilinear tap") o supersample con filtro real Lanczos3 (2462-2498).
9. `ExposureScan::Tick` (2528; ver §3.4).
10. Guías legibles: clones tipados de recursos typeless (2530-2541; `ReadableGuide` 1262-1299 con rebuild por DRS).
11. Multipass: bucle de features por pass con ping-pong base→A→B→A (2595-2667; ver §5.1).
12. **Resolve/composición** (modo 1, 2737-2765, dispatch 2868-2869): ver §2.3.
13. Supersample down-leg a nativo antes del resolve (2830-2840).
14. Copia de seguridad del capture before/after (2924-2931) y restauración de estados (3001-3022).

### 1.5 Sincronización y fences (resumen)

- **DeferredSr**: sin fences propias; usa un query heap de 16 timestamps resuelto a un buffer readback mapeado (`DlssNr_DeferredSr.inl:74-99,155-166`). Un slot solo se reutiliza cuando la GPU escribió su timestamp (84). Las generaciones se liberan solo cuando `Idle()` (56,126-129). El lifetime de HalfRate lo protegen "generation's GPU completion markers" (23-27). Shutdown retiene sin liberar lo no completado (717-726).
- **Late (FinishedPicture)**: cada slot tiene su fence y su command list/allocator propios (`DlssNr_Late.inl:93-96`); la señal se emite tras `ExecuteCommandLists` del productor (243-244), nunca al grabar (comentario 242); `ApplyToFinishedPicture` hace `queue->Wait(slot.fence, ready)` antes de regrabar (319) y señal `done` al final (429-430); espera CPU con evento de 5 s en `WaitForFinishedPicture` (214-221).
- **Regla general**: "A later CPU frame/Present count alone does not prove a resource is no longer in flight" (`DlssNr_DeferredSr.inl:74-75`); los recursos del modelo se "parked" 32 evaluates antes de liberarse (`DlssNr_Dx12.cpp:736-785`).

---

## 2. Formato del residual

### 2.1 Portador (carrier)

- `residualInput`: RGBA16F a w×h (render); `residualOutput`: RGBA16F a outW×outH — `DlssNr_DeferredSr.inl:147-148`.
- Dominio de color: el residual se calcula sobre **color display-encoded del juego vs el mismo color tras NR** (Before pasa `color` y `g.edited` al encode, 501-504); no sobre luz lineal de escena. La exposición entra solo como escala reversible (ver abajo).
- En FinishedPicture con TransferStrength, el finished-colour shader codifica **cambios relativos** antes de FP16 (comentario `DlssNr_DeferredSr.inl:497`: "encode relative changes before FP16 storage").

### 2.2 Encode clásico (modo 5, `dlssnr.hlsl:503-511`)

Patrón (paráfrasis, sin copiar):
- `difference = edited − original` (sanitizado finito).
- `d = difference / max(ExposurePreMul, 1e-4)` — divide por la pre-exposición del frame.
- `carrier = 0.5 + 0.5 · d/(1+|d|)` — **compresión signada reversible alrededor del gris neutro 0.5**; alfa=1. Comentario: "Neutral 0.5 encodes zero; values below it carry darkening. A reversible signed compression avoids clipping negative edits at the DLSS input" (501-503).

### 2.3 Decode + composición sobre el output (modos 6/10, `dlssnr.hlsl:512-528`)

- `signedEdit = clamp(2·carrier − 1, −0.999, 0.999)` — límite en los polos porque "DLSS can ring outside the carrier's [0,1] range" (523).
- `edit = signedEdit/(1−|signedEdit|) · max(ExposurePreMul,1e-4)` — inverso exacto, re-multiplica la exposición.
- `result = max(base + edit, 0)` conservando el alfa del base (526). En modo 10 con textura de exposición viva >0, devuelve el base sin editar (516-521).
- El base es el SR **limpio** del juego (`clean`, copiado en `After` 646-649) o, en half-rate, el history del frame ancla con su `historyScale` (658-661). El apply usa `ExposurePreMul = pair.scale` = preExposure capturado en Before (654, 434, 451).

### 2.4 Variante finished-colour (`dlssnr_finished_color.hlsl`)

- **Encode (modo 5, 40-57)**: si la escena no es lineal, eleva a 2.2 (49-50); `gain = clamp(1 + (edited−base)/max(base, floor), 1/limit, limit)` con `floor = max(peak·0.02, exposureScale·1e-4)` y `limit = clamp(maxRatio,1,8)` (51-54); `carrier = 0.5 + log2(gain)/8` (55). Motivo: "An absolute signed residual around 0.5 loses small dark-scene edits when stored in FP16" (43-45).
- **Decode (modos 2/3/4, 58-75)**: `gain = exp2(clamp((carrier−0.5)·8, ±log2(limit)))` (66-67); decodifica el pixel según destino: 2 = gamma 2.2 (SDR), 3 = scRGB lineal, 4 = PQ→2020→709 (68-69); multiplica la luz por el gain y re-codifica (70-72). "no scene-linear delta is added to display code" (61-63).
- El clamps de ganancia: `MaxRatio` config default 2.0 (`Config.h:347`), clamp a [1,8] en uso (`DlssNr_DeferredSr.inl:500`, `DlssNr_Late.inl:347`).

### 2.5 Residual Across-RR (acumulador temporal, `dlssnr_residual.hlsl`)

- **Accumulate (modo 0, 110-133)**: `delta = edited − original`; reprojecta el historial con MV del juego (`prevUV = uv + motion·MvScale`, 115-120); validez exige historial primado, MV finito con `|motion| < 2` y `prevUV` dentro de [0,1] (122-123); `history_t = lerp(reproject(history_{t−1}), delta, gResidualBlend)` (132). Invalid → history 0 y el pixel reaparece al ritmo del blend (128-129). "The per-frame ray-trace noise term… averages to zero; the enhancement persists" (12-16).
- **Apply (modo 1, 136-144)**: `base + delta·gTransferStrength`, clamp no-negativo, delta muestreado bilinealmente a resolución de salida.
- Blend default 0.08 (`Config.h:275`), clamp 0.01..1 en uso (`DlssNr_Dx12.cpp:2883`).
- Host: resolve a `residualEdited` sin tocar Color (2846-2874), acumulador con ping-pong `residualHistory[2]` RGBA16F (2875-2899), y `ApplyResidualAcrossRr` en el seam post-RR con pairing por identidad cmd/params/output (`DlssNrResidualPair.h:11-22`; `ApplyResidualAcrossRr` `DlssNr_Dx12.cpp:3043-3101`).

---

## 3. Sistema de exposición completo

### 3.1 White point — orden de resolución (`ResolveWhitePoint`, `DlssNr_Dx12.cpp:1087-1161`)

1. Override del frame (FinishedPicture PQ/scRGB: `WhitePointOverride = 203/80` nits de referencia, `DlssNr_Late.inl:366-367`) — aplicado en 2341.
2. Buffer no-HDR (ya tone-mapped) → slider manual (1093-1094).
3. **Fuente 2 (scan)**: `AnchoredWhitePoint(BestValue(), inverted, trim)` (1119-1130).
4. **Fuente 1 (exposición del juego)**: `clamp(preExposure / gameExposure · trim, 0.01, 4096)` (1132-1150); trim clamp [0.25,4] (1147). Fundamento FSR: "frame / preExposure * exposure… undoing it gives the divisor this pass wants" (1100-1102).
5. Fallback: slider (1160). La medición estadística desde el frame fue **eliminada** por feedback loop (comentarios 2297-2301, 1152-1159).

### 3.2 Exposición viva en-shader (cero latencia, D3D12)

- `WhitePoint()` en `dlssnr.hlsl:271-283`: lee `gExposure.Load(0,0)` (t4); si `1e-6 < e < 1e6`, devuelve `clamp(gExposurePreMul / e, 0.01, 4096)`; si no, cae al valor CPU. `gExposurePreMul = preExposure · trim` (`DlssNr_Dx12.cpp:2347-2357`). "removes the 3-4 frame CPU-readback lag" (267-269). Vulkan compila fuera esta ruta (249-251).

### 3.3 Meter-courier (fuente 1 por readback)

- Dispatch de 1 thread (modo 3) que copia la ExposureTexture del juego al tile (0,0) (`DlssNr_Dx12.cpp:2317-2337`; shader `dlssnr.hlsl:622-636`) — "a courier, not a measurement" (2300-2301). Anillo de 4 readbacks con flag `meterExposureValid` que viaja con el slot (`DlssNr_Dx12.cpp:873-903,356-357`); consumo 3-4 frames después (1014-1045) solo si el frame tenía exposición ligada (1040); invalidación al re-encender la opción (1064-1074) porque un valor congelado cruzado producía el colour cast documentado (1050-1060). `gameExposure`/`gamePreExposure` se retienen entre huecos de textura (366-371).

### 3.4 ExposureScan (fuente 2: encontrar la exposición que el juego no entrega)

- **Descubrimiento**: hook de creación de recursos `NoteResource` (304-340) y de UAVs `NoteUav` (348-380, deliberadamente no gateado en el setting, 350-360). Filtro `LooksLikeANumber` (222-268): flag UAV obligatorio (227), texturas ≤256 texels (234), formatos float de 1-2 canales (85-109), buffers ≤128 bytes (258). Cap 64 candidatos (28).
- **Lectura**: `Tick` (382-566) copia cada candidato a un readback con anillo de 4 slots (stride 512, 39); asume estado UAV (519-526, riesgo documentado). Rango plausible [1e-6, 1e4] (43-44); el test de movimiento es por **ratio**: `highest > lowest·1.25` sobre muestras in-range (476-477).
- **Veredicto**: `Where` (573-594) Found/Watching/Barren (paciencia 1800 frames, 571); `BestValue` elige el candidato con mayor travel `highest/lowest` (675-711).
- **Anchors multi-punto** (743-938): tabla serializada "scan:white;…" máx 8 puntos (`Config.h:482`); `AnchoredWhitePoint` (840-888): 1 punto = ley de ratio (`white · (scanNow/scanRef)` o inverso, 852-861), N puntos = interpolación log(white) vs log(scan) con clamp en los extremos (865-885), todo clamp [0.01, 4096] con trim [0.25,4]. "Only ratios are used, so the units of the buffer never have to be known" (1111-1113).
- Liberación segura de referencias al teardown de feature: `ReleaseTrackedResources` (952-964) — corrige un use-after-free que tumbaba el device en Cyberpunk (940-951).

### 3.5 Exposición del DLSS privado (DeferredSr)

- Feature creada con exposición **unitaria constante** (`DlssNr_DeferredSr.inl:401-409`, modo 7 = escribe 1.0, `dlssnr.hlsl:529-533`) y `DLSS_Pre_Exposure=1.0`/`Exposure_Scale=1.0` al evaluar (523-524). Toda la exposición real del frame viaja dentro del residual vía `ExposurePreMul` (encode divide §2.2, decode multiplica §2.3), así el SR privado nunca reescala la señal.

---

## 4. Skin/face handling

### 4.1 Máscara de color `SkinColourWeight` (`dlssnr.hlsl:64-73`)

Patrón: convierte el RGB saturado a YCbCr (coeficientes 601), mide distancia cromática al punto de piel (Cb≈0.405, Cr≈0.600) normalizada por radios (0.090, 0.110); peso = `1 − smoothstep(0.55, 1.35, length(dist))` gateado por `smoothstep(0.02, 0.10, chroma)` (chroma = max−min de RGB). Advertencia propia: "Approximate skin-colour selection, not a face/skin segmentation network. Warm materials may be selected and coloured lighting can hide skin" (61-63). Preview expuesto a propósito (62-63).

### 4.2 Aplicación en el resolve (`dlssnr.hlsl:1074-1094`)

- Clasifica el **frame intacto** (`original`), nunca la salida recoloreada del NR (1076-1078: displayRgb del original; comentario "Classify the untouched frame, never NR's recoloured output").
- Mezcla por-máscara de dos juegos de controles: `detail = lerp(EnvironmentDetail, SkinDetail, mask)` y `colour = lerp(EnvironmentColour, SkinColour, mask)` (1080-1081).
- Luma objetivo `wantedY = lerp(baseY, editedY, detail)`; croma mezclado en espacio de croma-over-luma (`baseChroma = original/baseY`, `editedChroma = result/editedY`) y multiplicado por `wantedY` (1082-1091); salida por `ClampAp1` (1091).
- Endpoints exactos: detail=0 ∧ colour=0 → frame original bit-exacto; sin protección activa no toca nada (1088-1090). Vista de depuración de la máscara en 1092-1093.
- Controles CPU: `DlssNrSkinProtection/SkinToneEnabled/SkinDetail/SkinColour/EnvironmentDetail/EnvironmentColour/ShowSkinMask` (`Config.h:299-305`), clamp [0,1] al subirlos (`DlssNr_Dx12.cpp:2745-2752`).

### 4.3 Lado modelo (parámetros NGX, no máscara propia)

- `DLSSNR.SkinStructureStrength` default −1 = "follow local structure", no es un 0..1 (`Config.h:295-296`; `PassProfiles.h:13`).
- `DLSSNR.UseAutoMask` — pese al nombre es la máscara automática **de piel** del modelo, no de interfaz (`DlssNr_Common.h:303-304`).
- Se escriben al crear la feature (create-time, no evaluate): comentario "Read once, while the feature is built" (`DlssNr_Common.h:290-292`); firma de create en `DlssNr_Dx12.cpp:2026-2032` (tuning.skin, autoMask).

---

## 5. Multipass

### 5.1 Multipass de capas (1..N features NR encadenadas)

- Config: `DlssNrPasses` default 1 (`Config.h:523`), clamp 1..3 (o 30 con `DlssNrUnlockPasses`, `DlssNr_Dx12.cpp:1836-1838`); per-pass preset/style/intensity/structure/tone/skin/autoMask con herencia explícita (`PassProfiles.h:18-99`; `Config.h:289-323`).
- **Una feature NGX por pass, cada una con su historial temporal** (`DlssNr_Dx12.cpp:226-244`): "One feature run three times in a frame is told three frames passed… Separate features each see one frame per frame".
- Ciclo de vida: máx **una feature nueva por frame**, build-only, y nunca evaluar en el epoch que creó (2100-2184; guard de pending 2116-2129). Fallo de creación latcheado; solo el prefijo contiguo de passes listos corre (`effectivePasses`, 2584-2593) — nunca reutilizar la feature principal como fallback (comentario 2581-2583).
- Data flow: encode único; proxy base inmutable; ping-pong de respuestas `base→A→B→A` con dos superficies work-size (2606-2667); "The final answer is resolved once against the original base, so matched-residual transfer is the cumulative final-minus-base edit" (2606-2609). Local tone solo en pass 0 (`PassProfiles.h:50`).
- Corte de cámara / Reset del juego resetea todas las capas (passReset compuesto 2643).
- Diseño documentado en `dlssnr/design/pre-sr-multipass.md:15-42` (reglas de resource-state 43-47, guardrails 49-63).

### 5.2 Collect

- `DeferredSr::Collect()` (`DlssNr_DeferredSr.inl:126-129`): borra de `retired` las generaciones cuyo último marcador GPU completó (`Idle()`, 56). Se llama al inicio de cada Before (291) y en Shutdown (721). Una generación cambia por device/queue/resolución/formato/flags/modo (354-360) y se retira con todas sus superficies y su feature NGX (destructor 57-71).

### 5.3 Half-rate (NR cada dos frames + FG)

- Requisitos duros: MV low-res **no jitterado** (`DlssNr_DeferredSr.inl:211-213`) y opt-in explícito de cámara aproximada (216-217, "Never mark invented matrices as game-supplied camera data").
- `PrepareHalfRate` (207-267):
  - **NormalizeMotion (modo 8)**: MV del juego → `h.motion` RGBA32F en UV normalizadas (`MvScale/w`, `MvScale/h`, 232-239; shader `dlssnr.hlsl:539-547` con validez finita `|m|<2` y **sentinela 65504** para inválidos).
  - **ComposeMotion (modo 9)**: compone dos campos sucesivos al desplazamiento reproyectado → `anchorMotion` (242-248; shader 549-559: suma current+previous con validación en cadena y mismo sentinela).
  - **Cámara aproximada**: near/fear/FOV desde `FsrCameraNear/Far/VerticalFov` (250-255), proyección perspectiva RH (256), clipToPrevious/previousToClip = identidad (260-261) — marcadas explícitamente como aproximadas.
- Cadencia: frame ancla (NR+SR privado+FG-input) → frame interpolado (skipNr: ni NR ni SR privado; solo FG + compose sobre history) — decidido en `Before` 448-453 y contabilizado en `After` 707-711. FrameTimeDelta del DLSS privado se duplica en el ancla (521-522).
- **ResidualFg** (`dlssnr/ResidualFg.h`): wrapper de la FG de resource-output de NGX. Create comprueba `FrameGeneration.Available` (81-84) y fija formato backbuffer RGBA16F + internal size = guías (92-99). Evaluate (107-176) setea el bloque DLSSG.* completo: backbuffer=carrier residual, depth, MVecs=anchor/motion normalizado, OutputInterpolated, **OutputDisableInterpolation=suppression UAV**, cámara aproximada, jitter 0, MvecJittered 0, sentinela 65504, subrects de guía.
- **Suppression**: buffer UAV 256 B re-zeroado cada frame ancla (`DlssNr_DeferredSr.inl:611-613`); NVIDIA escribe un booleano en el primer byte; se copia a una textura R8_UNORM 1x1 para el shader "avoids CPU waiting and changing the game's predication" (629-641); el modo 10 la lee en t4 y si indica supresión deja pasar el base limpio (decode §2.3 + `DlssNr_Common.h:29`).
- **anchorMotion**: descrito arriba (modo 9). `previousMotion` se guarda al final de cada After (698-707); `previousWasAnchor` decide el skip del frame siguiente.
- Historias: `h.history[2]` al tamaño de salida con su `historyScale` (preExposure del ancla) — el frame interpolado compone sobre la historia limpia del ancla, no sobre el SR del frame actual (658-675).

### 5.4 Sample-and-hold (sin MV)

- Si no hay MotionVectors y se pidió ResidualFg: `sampleAndHold` (`DlssNr_DeferredSr.inl:312-314`); guía `zeroMotion` R16G16F (413-418); flags forzados a MVLowRes (352-353); Reset **cada frame NR** (472) porque no hay MV que garantice continuidad; `DlssNrResidualHold` (`DlssNr_Common.h:35-46`) permite reusar el sample solo si `epoch == sampleEpoch+1` (39) — "A successful sample may be reused only on the immediately following frame. Failed composition, cuts and gaps must not turn a two-frame hold into a freeze" (33-34). Estado del usuario: 684.

### 5.5 Hold de frame (calibración/A-B)

- `DlssNrHoldFrame` (`Config.h:340-343`): congela el input del encode copiando el output aparte en hold-on y restaurándolo encima del vivo mientras dure (implementación `DlssNr_Dx12.cpp:2359-2423`); la medición del white point se suspende con el snapshot (2411-2414). Diseño en `dlssnr/design/frame-hold.md`.

---

## 6. Jitter, MV y acumulación temporal

### 6.1 Jitter

- **Reenviado al DLSS privado tal cual** desde el bloque del juego: `Jitter_Offset_X/Y` (`DlssNr_DeferredSr.inl:517-518`). Es el único uso: el NR del modelo no recibe jitter propio.
- FG: `DLSSG.JitterOffsetX/Y = 0` y `MvecJittered = 0` (`ResidualFg.h:156,160-161`). El half-rate exige MV no jitteradas al juego (§5.3).

### 6.2 Motion vectors — dominios y escalas

- **Modelo NR (forwarder)**: MvScale del juego pasada por literal ("The game's own encoding, passed through", `DlssNr_Dx12.cpp:1756-1760`), re-escalada al tamaño de trabajo `mvToWork = workWidth/width` (2546-2551) y entregada al evaluate (2651).
- **DLSS privado (deferred)**: MvScale del juego; en half se pasa `(w, h)` porque los MV ya están normalizados a UV (`DlssNr_DeferredSr.inl:475`, con normalización previa 232-235); en sample-and-hold `1.0` (476).
- **Acumulador residual**: `MvScale = frame.MvScale / width(/height)` → UV de imagen (2889-2890), reproyección bilineal con sampler lineal clamp (shader 95, 119-125).
- **Región válida**: la región de MV se dimensiona contra el extent de **output** (no el de render) cuando MV no es low-res — fix documentado de cmh1448 (`DlssNr_Guides.h:30-38`).
- Subrects/base offsets del juego respetados en evaluate (depthBaseX…, 2648-2650); DeferredSr los exige a cero para su propio camino (320-325).

### 6.3 Acumulación temporal e invalidación

- **El resolve principal NO acumula**: el acumulador temporal del edit fue eliminado dos veces por contraproducente — "the model re-decides its detail with the framing, so an old answer does not belong to a new frame… The composition is re-anchored to the model every frame instead" (`dlssnr.hlsl:876-886`). La estabilidad temporal delegada al historial interno del modelo NGX (reset por corte) y al SR privado.
- **Invalidación por Reset del juego**: leído como FFXFeature lo lee (3417-3422), propagado a todas las features del modelo (passReset, 2643), al DLSS privado (`DlssNr_DeferredSr.inl:516`), y al acumulador residual (`residualHistoryPrimed=false`, 1762-1774). En half-rate: `h.Reset()` en hueco de epoch, reset del juego o `g.reset` (226).
- **Otros invalidadores del acumulador**: cambio de tuning (1863-1865), resolución/colocación/formato (1884-1904, `ReleaseSurfacesIfFormatChanged` 799-832), fallo de pairing post-RR (`ApplyResidualAcrossRr` 3049-3053), cambio de modo finished (3129-3146), pre-seam sin residual (3188-3195).
- **Acumuladores que sí existen**: (a) ResidualAcrossRR v2 (§2.5); (b) historias de half-rate (§5.3); (c) el hold de un frame del sample-and-hold (§5.4); (d) los historiales internos de las features NGX (una por pass, §5.1).
- **Envío de MV al FG**: anchorMotion (2 frames compuestos) en frame ancla, motion normalizado en el primero — 616-619.

---

## 7. Lo que este fork hace y nuestra pipeline (poc-dlss-standalone) NO hace

Contexto de "nuestra" pipeline: runner con NR enhance MLX-DLSS (float32 [0,1]) + SR propio + FG de pesos extraídos, sin sistema de exposición ni residual (`FINDINGS.md:3-47`, `README.md:10-33`). Ranking por impacto estimado en calidad de imagen:

1. **Residual pre-SR escalado por SR privado + composición post-SR** (§1.1-1.3, §2): el edit NR se calcula a resolución de render, se comprime signado alrededor de 0.5 (inversible, sin clip de edits negativos), se escala con una feature DLSS propia con exposición unitaria, y se suma al SR limpio del juego. Nosotros aplicamos NR al frame y encadenamos SR encima sin portador residual; ningún equivalente del carrier 0.5±d/(1+|d|) ni del round-trip de exposición. `DlssNr_DeferredSr.inl:147-148,492-526,650-676`; `dlssnr.hlsl:503-528`.
2. **Sistema de exposición completo con tres fuentes y anclaje multi-punto** (§3): white point derivado de preExposure/exposición del juego (clamp 0.01..4096), exposición viva en-shader sin latencia de readback, y un escáner heurístico de buffers candidatos con veredicto por movimiento-ratio e interpolación log-log de puntos de calibración. Nosotros no tenemos noción de exposición: el contrato NR es fijo [0,1]. `DlssNr_Dx12.cpp:1087-1161,2317-2357`; `DlssNr_ExposureScan.cpp:222-268,382-566,840-888`.
3. **Codificación de color del proxy reversible y hue-preserving** (§2.2 y encode modo 0): soft-knee, Neutwo (un escalar sobre el canal pico, [0,∞)→[0,1) sin punto de clip) e híbrido identidad-bajo-rodilla, todas con decode exacto; composición por ratio de luminancia con hue reconstruido en OkLab y ClampAp1 de gamut. Nosotros alimentamos el frame tal cual. `dlssnr.hlsl:318-463,949-1072`.
4. **Acumulador temporal MV-reprojected para el residual (Across-RR)**: separa el término de ruido por-frame (promedia a cero) de la mejora persistente, con invalidación por MV fuera de rango/cortes. Nosotros no acumulamos nada. `dlssnr_residual.hlsl:110-144`; `DlssNr_Dx12.cpp:2871-2900,3043-3101`.
5. **Multipass de capas con feature e historial por pass**: hasta 3 (30 desbloqueadas) evaluaciones encadenadas base→A→B con tuning por capa, sin reutilizar feature ni evaluar en el epoch de creación. Nosotros: una pasada NR. `DlssNr_Dx12.cpp:226-244,2100-2184,2595-2667`; `PassProfiles.h`.
6. **Protección de piel por máscara de color con controles duales** (§4): SkinColourWeight YCbCr + mezcla detail/colour separada skin/entorno sobre luma/croma, clasificando el frame intacto. Nosotros no tratamos piel. `dlssnr.hlsl:64-73,1074-1094`.
7. **Half-rate + FG de NVIDIA sobre el residual** (§5.3): NR cada dos frames, interpolación del residual por DLSSG con MV ancla compuestos, flag de supresión leído en GPU, historias limpias por ancla. Nuestro FG interpola frames finales, no residuales. `DlssNr_DeferredSr.inl:207-267,606-643`; `ResidualFg.h:107-176`.
8. **WorkingScale con transfer matched-residual y supersample**: modelo a 25-200% con box-resample exacto por área a la baja, Lanczos3 real a la alza, y transfer que reconstruye el proxy del frame a resolución completa para que solo el edit viaje desde el raster pequeño (elimina el color shift dependiente de resolución). `DlssNr_Dx12.cpp:2456-2525,2830-2840`; `dlssnr.hlsl:888-947`.
9. **Disciplina de sincronización GPU**: marcadores timestamp por seam, generaciones jubiladas con lifetime protegido, fences por slot en Present, parks de 32 evaluates, nunca evaluar una feature en el epoch que la creó. Relevante si portamos algo de lo anterior. `DlssNr_DeferredSr.inl:74-99,126-129`; `DlssNr_Late.inl:231-253`; `DlssNr_Dx12.cpp:736-785,2071-2098`.
10. **Frame hold + capture before/after emparejado** (instrumentación A/B del mismo frame): congelar el input del encode y volcar pares before/after crudos con manifest. `DlssNr_Dx12.cpp:2359-2423`; `dlssnr/DlssNr_Capture.h:37-130`.

---

## 8. Notas de verificación

- Lecturas completas (sin truncar): todos los .hlsl, DlssNr_DeferredSr.inl, DlssNr_Late.inl, DlssNr_Capture.h, DlssNr_ExposureScan.cpp, ResidualFg.h, PassProfiles.h, headers comunes. `DlssNr_Dx12.cpp` leído en tres tramos (1-2000, 2001-3000, 3001-3917) — cubre todo el archivo.
- `DlssNrNative.cpp` y el sub-sistema NVFP4 hybrid (`DlssNrHybridAssets.h`, `DlssNrHybridBuilder.h`) solo se revisaron por búsqueda dirigida (SetPrecision/NVFP4): es un camino de precisión experimental (config `DlssNrPrecision`, `Config.h:277`) ortogonal al pipeline de color; no se incluyó en el mapa por no afectar al flujo de ruido.
- Las líneas citadas de `inputs/NVNGX_DLSS_Dx12.cpp` provienen de búsquedas con contexto sobre ese archivo (1122-1132, 1180-1193); el flujo Before/SR-juego/After ahí descrito coincide con lo declarado en `dlssnr/DlssNr.h:16-19`.
