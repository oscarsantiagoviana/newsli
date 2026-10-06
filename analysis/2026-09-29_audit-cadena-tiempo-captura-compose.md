# Auditoría forense: cadena captura→delta→compose de new-sli vs OptiScaler-DLSSNR-PreSR-Multipass

Fecha: 2026-09-29 · Ámbito: cadena de tiempo/alineación/acumulación del delta NR (pre-multipass).
Método: lectura completa de `src/host/offload_session.cpp` (2281 L), `shaders/compose_delta.hlsl`, `shaders/nr_delta.hlsl`, `shaders/nr_encode.hlsl`, `src/engine/nr_vendor.h`, `src/engine/loop.cpp` vs `DlssNr_DeferredSr.inl`, `DlssNr_Late.inl`, `DlssNr_Dx12.cpp`, `dlssnr_residual.hlsl` (precompile), `DlssNr_ResidualPair.h`, `DlssNr_ExposureScan.cpp`, `Config.h` del fork. Números de línea medidos sobre el árbol actual (HEAD cf83394).

## Tabla resumen

| # | Punto | Veredicto | Resumen causal |
|---|-------|-----------|----------------|
| 1 | Jitter / origen de captura | **[OK-equivalente]** (+2 notas) | El modelo NR del fork TAMPOCO recibe jitter; su acumulador no usa jitter (solo MV). Nuestro jitter sellado es código muerto inofensivo; la MV de reproyección SÍ es la del frame K correcto. |
| 2 | MV dominio y escala | **[OK-equivalente]** | Misma fórmula exacta del fork (`mvscale/renderW` → UV, guard `\|motion\|<2` en UV también en el fork). `mvScale*workScale` del evaluate ≡ `mvToWork` del fork por producto. |
| 3 | Delay-line (entrega K con retraso) | **[MEJORABLE] estructural** | El fork compone EN el frame N (Before/After misma lista); nuestro delta K se aplica sobre el output N con K ≈ N−3..5 ⇒ contribución NR desalineada por 3-5 frames de movimiento en los frames APPLY (y parcialmente en los ACCUMULATE). Mitigable encadenando reproyección con mvRing. |
| 4 | Acumulador residual | **[OK-equivalente]** (+3 menores) | Blend 0.08, invalidación, ping-pong y reset por corte son fork-exactos. Diferencias menores: alpha sobrescrito a 1.0, hist no se resetea al reconstruir knobs, apply sin reproject (ver #3). |
| 5 | Exposure / pre-exposure | **[OK-equivalente]** + **[BUG] condicional** | Encode/decode comparten la MISMA E en el mismo frame ⇒ el delta es autoconsistente (ventaja estructural). `preExposure` del juego ignorado (muerto). [BUG] real: el tile-grid de la mediana se dimensiona a RENDER pero se llena a WORK ⇒ mediana sobre VRAM sin inicializar cuando `nrWorkScale<1`. |
| — | Extra fuera de los 5 puntos | **[MEJORABLE]** | Guías del engine leídas a (frame−1)%3 cuando la señal de produce ya garantiza la del frame N (color N + MV/depth N−1 = set incoherente de 1 frame). Scalars = mailbox mono-slot leído hasta ~4 frames tarde. Overrides de panel jitterX/Y/mvScale muertos en esta cadena. |

---

## 1. Jitter / origen de captura (sincronización N vs K)

**Nuestro.** `EvaluateInner` sella los scalars del frame N ANTES del seal: `s.scalars->jitterX/Y = a.jitterX/jitterY` en `src/host/offload_session.cpp:2030-2031`; `mvScaleX/Y` en `:2032-2033`; `reset` en `:2034`; `preExposure` en `:2042`. El seal del color/MV del frame N va en `RecordSealFrame` (`:1044-1315`, llamada en `:2048`).

**Fork.** El jitter SOLO entra en el DLSS SR **privado** que escala el residual, con el jitter del frame que se está componiendo: `p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X/Y, Float(source, ..., 0))` en `DlssNr_DeferredSr.inl:517-518`, dentro del `Before()` del frame N (captura de color N en `:456-462`, evaluate en `After()` `:586`, compose sobre el output N en `:646-682`). El **modelo NR nunca recibe jitter** (no existe parámetro jitter en el ABI DLSSNR; grep de `Jitter` en `DlssNr_Dx12.cpp` → 0 hits en el camino NR) y el **acumulador residual tampoco lo usa**: `dlssnr_residual.hlsl:115-123` reproyecta con `prevUV = uv + motion` — sin término de jitter.

**PREGUNTA CLAVE 1a (¿desfase jitter sellada vs frame compuesto?).** No hay bug: en nuestra cadena el jitter sellado es **código muerto** — `scalars->jitterX/Y` no lo lee nadie en `src/engine/` (grep `jitter` → 0 hits; `nr_vendor.h` no lo pasa al evaluate: firma `PFN_FwdEval` `nr_vendor.h:64-70` sin jitter). El único consumidor de jitter del sistema es el DLSS del juego, que lo recibe del juego directamente (no sustituimos inputs, `offload_session.cpp:17-18`). Equivalente al fork: su modelo NR tampoco ve jitter. **[OK-equivalente]**.
- Nota A: los controles de panel "Jitter X/Y" (`src/panel/panel.cpp:147-157`) prometen "sub-pixel sampling offset of the temporal pass" — en esta cadena son **placebo** (no llegan ni al engine ni al compose). O se eliminan o se re-etiquetan.
- Nota B: nuestro acumulador muestrea el MV sin compensación de jitter, exactamente como el fork (`compose_delta.hlsl:135-136` ≡ `dlssnr_residual.hlsl:119-120`). Si el juego declara MVJittered, ambos arrastran el mismo defecto — paridad.

**PREGUNTA CLAVE 1b (¿la MV de reproyección corresponde al frame correcto?).** Sí. El delta entregado se etiqueta con K = `scalars->engineHostFrame` (escrito por el engine con `produceFence->GetCompletedValue()` en `src/engine/loop.cpp:563`, o sea el frame del host que el engine acaba de procesar), y la MV que se carga en `texMv` es `mvRing[K%8]` con verificación de tag `mvRingFrame[K%8]==K` (`offload_session.cpp:1247-1283`); si el slot fue sobreescrito → `histValid=false` (`:1289`) en vez de reproyectar con MV ajena. Es el mismo principio del fork (la MV capturada en el mismo seam que produjo el residual: `DlssNr_Late.inl:121-138`, `DlssNr_Dx12.cpp:2892` pasa el `motionIn` del frame del residual). **[OK-equivalente]**. El ring de 8 slots cubre el lag medido ~3-5x (`offload_session.cpp:361-366`).

## 2. MV — dominio y escala

**Nuestro (compose).** `ComposeCb` en `offload_session.cpp:2132-2149`: `cMvScaleX/Y = gsx/s.inW, gsy/s.inH` (`:2147`) donde `gsx` = `s.scalars->mvScaleX` del juego (`:2141-2142`). En el shader, la MV se muestrea en render-res (`gMv.Load` bilinear 2×2, `compose_delta.hlsl:124`), se multiplica por la escala → UV, y `prevUV = uv + motion` sobre el hist a display-res (`:135-136`). El dominio display del hist no introduce error: motion queda en UV (independiente de resolución).

**Fork.** `accum.MvScaleX = frame.MvScaleX / (float)width` (width = RENDER) en `DlssNr_Dx12.cpp:2889-2890`; shader: `motion = gMotion.Load(...).xy * float2(gMvScaleX, gMvScaleY)` con muestreo de la MV en la guía a coordenada render + subrect base (`dlssnr_residual.hlsl:115-119`). Misma fórmula, mismas unidades. El guard de invalidación `all(abs(motion) < 2.0)` está en unidades UV **también en el fork** (`dlssnr_residual.hlsl:122`) — nuestro `compose_delta.hlsl:137-139` es literal. Idem `prevUV ∈ [0,1]` y finite-checks.

**Evaluate del modelo (workScale).** Nosotros pasamos `mvScaleX*workScale` al evaluate (`nr_vendor.h:717`) con MV sin reescalar; el fork escala el CONTENIDO de la MV (mvToWork) y mantiene `mvScale` sin tocar — de hecho prohíbe explícitamente doblar la escala: "scaling by the resolution ratio on top counts it twice" (`DlssNr_Dx12.cpp:1756-1758`). Producto `mv·mvScale·workScale` idéntico por conmutatividad ⇒ misma MV normalizada en dominio work. **[OK-equivalente]**.

Diferencias cosméticas no-bug: (a) nuestro tap de MV/delta es bilinear manual, el fork usa Load nearest en la guía; (b) nuestro delta se reescalea render→display bilinear en el acumulador, el fork lo escala el DLSS privado (upscaler mejor que bilinear) — softness menor de la contribución NR. **[MEJORABLE] estético**.

## 3. Delay-line (entrega del delta con retraso)

**Nuestro.** Delay-line de "1 evaluate" en teoría (`offload_session.cpp:1979` "deliver the previous delta… R69j"), pero el retraso REAL lo pone el engine (~3-5× más lento que el juego, comentario en `:1676-1677`; gate de entrega `done > lastDeliveredDone` `:1998`). Cadena por frame N del juego: Evaluate N entrega delta K→`texDelta` + MV_K→`texMv` (seal list `:1219-1302`), y `AfterRealEvaluate` N compone sobre el output N — con el delta ESTACIONADO el frame anterior (la seal list corre DETRÁS de la lista del frame, así que el compose registrado en la lista N lee lo que stages la seal N−1: comentario explícito `:1214-1218` — par (delta,MV) coherente, sin carrera; orden GPU lista N−1 → seal N−1 → lista N).

**Fork.** Composición **same-frame**: `Before()` captura color N (`DeferredSr.inl:456-462`), encode del residual N (`:492-504`), `After()` evalúa el DLSS privado y aplica el residual sobre el output N en la MISMA lista (`:586`, compose `:644-682`). Su variante asíncrona (`ResolvePrivate :537-558`) es camino muerto documentado ("async NR was removed in v0.7.1"). Incluso su ruta MÁS retardada (finished-picture, `DlssNr_Late.inl`) está acotada por epoch: descarta slots con `epoch - SubmissionEpoch > 1` (`DlssNr_Late.inl:302-307`).

**PREGUNTA (¿error sistemático que el fork evita?).** Sí, uno estructural, de dos partes:

1. **Frames APPLY (entre deltas):** `out_N = clean_N + hist·boost` muestrea el hist en `uv` **sin reproyección** (`compose_delta.hlsl:77-99`), pero el contenido del hist está alineado a la escena del frame K+1 (último accumulate) mientras `clean` es el frame N. Con K ≈ N−3..5, la contribución NR queda desplazada 3-5 frames de movimiento respecto de la imagen — exactamente el fingerprint "mejora que arrastra detrás de los objetos en movimiento", proporcional al lag del engine y a `boost`. El fork no lo padece porque su apply corre same-frame (hist recién reproyectado con MV del frame, `dlssnr_residual.hlsl:136-144` con la escala aplicada en el accumulate del MISMO frame).
2. **Frames ACCUMULATE:** la reproyección encadena hist(K)→(K+1) con MV_{K+1} — internamente correcta (la cadencia del engine entrega K, K+1, K+2… consecutivos) — pero el resultado `hnew` (alineado a K+1) se aplica sobre `clean_N` con N−(K+1) ≈ 2-4 frames de desfase residual.

**[MEJORABLE] estructural** (coste inherentemente ligado al offload; el fork lo evita por diseño in-process). Mitigación posible sin romper la paridad: en modo apply, reproyectar el hist con las MV acumuladas K→N (tenemos `mvRing[8]` con tags; encadenar 3-5 taps) o como mínimo con la MV del frame actual; y documentar que `boost>1` amplifica el desfase visible.

Cosas que SÍ están bien y no tocar: el gate `done > lastDeliveredDone` (regla R69u, `:1998-2000`), la no-sustitución de inputs del juego, y el fallback a frame limpio con nrOn=0 (`:2091-2092` — paridad con "clean SR frame retained" del fork `DeferredSr.inl:690`).

## 4. Acumulador (residual pair / ping-pong)

Comparación término a término — `compose_delta.hlsl` + `offload_session.cpp` vs `dlssnr_residual.hlsl` + `DlssNr_Dx12.cpp`:

| Mecanismo | new-sli | Fork | Veredicto |
|---|---|---|---|
| Blend | `0.08f` hardcode ("fork ResidualBlend", `offload_session.cpp:2146`); `a = clamp(blend,0,1)` (`compose_delta.hlsl:144`) | default `0.08f` (`Config.h:273-275`), clamp(0.01,1) al despachar (`Dx12.cpp:2883`), `clamp(gResidualBlend,0,1)` en shader (`residual.hlsl:130`) | ✔ igual (solo falta el suelo 0.01 y exponerlo como knob) |
| Invalidación | `cHistValid && isfinite(motion) && abs(motion)<2 && prevUV∈[0,1]` → hist=0 (`compose_delta.hlsl:137-143`) | idéntico literal (`residual.hlsl:122-126`) | ✔ |
| Reset por corte | `gameReset → histValid=false` (`offload_session.cpp:2037-2038`) | `frame.Reset → primed=false` (`Dx12.cpp:1762-1767`) | ✔ |
| Ping-pong | SRV t2=hist[1] / UAV u1=hist[0] en el mismo dispatch (`offload_session.cpp:954-967`, regla R73c) + swap por COPIA tras accumulate (`:2198-2213`) | índice prev/cur sin copia (`Dx12.cpp:2875-2899`) | ✔ equivalente (la copia cuesta un full-screen copy por accumulate; el flip de índice la evita — [MEJORABLE] perf menor) |
| Priming | `histValid ? 1 : 0` (`:2148`), se arma tras el primer accumulate (`:2190`) | `residualHistoryPrimed` (`Dx12.cpp:2884,2896`) | ✔ |
| Aplicación | `out = max(clean + hnew·boost, 0)` en el MISMO dispatch del accumulate (`compose_delta.hlsl:150`) y modo apply separado para frames sin delta | fork: accumulate y apply son dispatches separados (`residual.hlsl:110-133` vs `:136-144`); apply = `max(base + delta·strength, 0)` | ✔ (fusión nuestro-save-un-pass; el apply sin reproject es la diferencia real, ver #3) |
| Clamp de señal | delta clamp [-1,1] y finite-guards en el ENGINE (`nr_delta.hlsl:86-92`); hist sin clamp, solo finite | delta sin clamp (finite-sanitize), strength clamp(0,1) en apply (`Dx12.cpp:3080`) | ✔ equivalente por diseño de dominio [-1,1] display |
| Boost | `nrBoost` hasta 16 (`offload_session.cpp:608`), default 1.0 | strength ≤ 1 siempre | A/B knob propio, documentado como no-calidad (`nr_delta.hlsl:27-28`) — OK como herramienta, default fork-exact |

**Tres desviaciones menores encontradas [MEJORABLE]:**
1. **Alpha de salida sobrescrito**: `gOut = float4(v, 1.0)` (`compose_delta.hlsl:98,164`) pisa el alpha del frame del juego; el fork lo conserva (`residual.hlsl:142` `float4(..., base.a)`). En RDR2 (RGBA16F/R10G10B10A2 sin alpha relevante) inocuo, pero preservar `clean.a` es gratis y fork-exact.
2. **Hist no se resetea al reconstruir knobs**: `RebuildKnobs` (`nr_vendor.h:799-839`, `seq=0` ⇒ reset del modelo) no toca `compose.histValid`; el hist acumula deltas del modelo viejo-knobs mezclados con los nuevos. El fork resetea `primed` cuando el tuning cambia (`Dx12.cpp:1863-1865`). Falta la equivalencia: bump de `nrParamSeq` ⇒ `histValid=false` (host).
3. **`forceReset` del panel** (`loop.cpp:489`) resetea el modelo pero no el hist del host — el "reset history" del POC perdió su mitad host.

El contrato tint (alpha 1.0 real / 2.0 sólido, gate `(0,1.5]` en `compose_delta.hlsl:128`) es herramienta propia de verificación, sin equivalente en el fork — correcto que no interfiera (alpha 0 del eco ⇒ motion gateado ⇒ blend de delta 0 = decaimiento suave, no corrupción).

## 5. Exposure / pre-exposure

**Nuestro.** `expoScale = 0.18/median(tileLums)` con mediana CPU sobre ~5k tiles (`nr_vendor.h:773-784`), lag 1 frame por construcción (los tiles son del encode anterior), guard `med>1e-4` (si no, conserva la E previa = hold implícito anti-flicker). La MISMA `expoScale` alimenta encode (`:690`) y decode-delta (`:747`) del mismo frame ⇒ `delta = model_disp(E) − encode(orig·E)` es **autoconsistente en E por construcción** — el término E se cancela dentro del frame. `s.scalars->preExposure = a.preExposure` (`offload_session.cpp:2042`) no lo consume nadie (grep engine: 0 hits).

**Fork.** White point anclado: `clamp(gamePreExposure/gameExposure·trim, 0.01, 4096)` (`Dx12.cpp:1145-1149`); exposición del juego con **hold** entre frames ("a fallback to 1.0 on the gaps would be a flicker source", `:367-371`); opción zero-latency recomputando in-shader con la textura de exposición del juego (`:2343-2357`); su camino por defecto es meter-CPU con 3-4 frames de lag (`:2345`). ExposureScan = anclaje multipunto persistente para estabilidad (`DlssNr_ExposureScan.cpp:743-790`). En DeferredSr, `encode.ExposurePreMul = frame.PreExposure` (`DeferredSr.inl:493`) y `apply.ExposurePreMul = pair.scale` (`:654`) — mismo valor llevado por el par.

**PREGUNTA (¿drift/flicker?).** No hay mecanismo de flicker por E en nuestra cadena: E cambia ⇒ encode y decode cambian juntos ⇒ el delta permanece ~0 para modelo pasivo y acotado para activo. Nuestro lag de 1 frame es MEJOR que el meter-CPU por defecto del fork (3-4 frames). Diferencias reales:
- **[BUG] condicional — tile-grid mixto render/work**: `tilesX = (w+7)/8` con w = RENDER (`nr_vendor.h:343-344`), pero el dispatch del encode es a WORK (`:690,694`) y el readback + mediana recorren `tilesX·tilesY` completos (`:758-759, :776-778`). Con `nrWorkScale<1`, la fracción no cubierta del grid es VRAM de DEFAULT heap **nunca escrita** ⇒ mediana sobre basura ⇒ `expoScale` arbitrario ⇒ encode corrupto. A workScale 1.0 (default) no se dispara. Fix: dimensionar tiles a work, o llenar lo no escrito, o acotar n a los tiles escritos.
- **[MEJORABLE]**: `preExposure` del juego ignorado. La E nuestra se ancla a la mediana del contenido visto (auto-compensa parcialmente la exposición pre-aplicada del juego), pero en transiciones de eye-adaptation la frontera del guard de saturación (`enc≥0.985 → d=0`, `nr_delta.hlsl:84-85`) se mueve con E — los márgenes del guard pueden parpadear en escenas brillantes. Anclar la E al `preExposure` del juego (como `Dx12.cpp:1149`) o exponer trim sería la paridad.
- El guard de saturación ≥0.985 es invención propia heredada del POC (R70b "fork Mode 6"); el fork maneja highlights con MaxRatio clamp 1..8 en finished-color (`DeferredSr.inl:500`). No hay equivalente 1:1 en su camino residual-across-RR — mantener, pero documentado como propio.

## Extras fuera de los 5 puntos (encontrados durante la lectura)

1. **[MEJORABLE] Guías 1 frame más viejas de lo necesario**: el engine lee `bufGuideD/M[(frame-1)%3]` (`loop.cpp:491-492`) cuando la señal de produce N ya está GPU-ordenada DETRÁS de la seal list que escribió `guideM[N%3]` (`offload_session.cpp:2246-2252`: Execute(sealCmd) → Signal(produce,N)). El set del engine es color N + depth/MV N−1 — el comentario "the lag native NGX sees" (`:350-352`) no describe al NGX nativo (el juego pasa sus guías del frame actual al evaluate). Una frame de desfase color↔guías = smear temporal dentro del propio modelo. Cambiar a `frame%3` es seguro por el orden de cola.
2. **[MEJORABLE] Scalars = mailbox mono-slot**: el host reescribe `s.scalars` cada evaluate (`:2028-2043`) y el engine lo lee hasta ~4 frames después (`loop.cpp:398-402,489,494`) — mvScale/reset/renderW pueden ser de un frame posterior al color sellado. Benigno con mvScale estático; viola la regla POC "lag the WHOLE input set, scalars included". Ring de scalars (o al menos mvScale/reset/renderW por frame) lo cierra.
3. **[MEJORABLE] Overrides muertos del panel**: `jitterX/Y` y `mvScaleX/Y` (panel `panel.cpp:143-157`, ctl `offload_session.cpp:583-586`) no llegan a ningún consumidor: el compose usa `scalars->mvScaleX` del juego (`:2141-2142`) y el engine `scalars->mvScaleX` (`loop.cpp:494`) — el bloque Tuning nunca se consulta para esos campos. O se cablean (compose debería leer `tuning.mvScaleX>=0 ? tuning : game`) o se marcan como game-only.

## Conclusión

La **cadena base es fork-exacta en lo que decide la calidad del acumulador** (escala MV, invalidación, blend, reset, ping-pong, gate de entrega) — los puntos 1, 2 y 4 no arrastran bugs al multipass. El riesgo real de la cadena está en dos sitios: el **desfase estructural apply-side del punto 3** (contribución NR posicionalmente anticuada por el lag del engine, invisible a boost 1.0, creciente con boost y con el lag — candidato a explicar "smear" residual reportado si se confirma en vivo), y el **[BUG] del tile-grid** si alguien baja `nrWorkScale` (corrupción de exposición). Ninguno de los dos es un fallo de "captura o salida" en el sentido de transporte: el par (delta K, MV K) llega coherente y sellado al píxel.
