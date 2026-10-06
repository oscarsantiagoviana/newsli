# 28 — Segunda auditoría fps/latencia (deleg_6f37e90b) y plan R90

Fecha: 2026-10-03. 3 auditores (ENGINE / HOST / TRANSPORTE+ABI+PANEL), solo lectura.
Reglas grabadas en las misiones: LEY 1 (mismo frame, nunca stale), LEY 2 (calidad),
cero ghosting/flicker, sin inventos para pocos fps. 0 hallazgos con
ghosting_flicker_risk=true. **Nada implementado aún — plan presentado primero.**

## Diagnóstico central (la caída 8→4-5)

El gate espera DENTRO del Present del juego a que el engine entregue el delta
del frame N exacto (`offload_session.cpp:2627-2726`, `nrGateWaitMs=0` = sin
límite). Es la única espera legal del diseño (LEY 1) y convierte el fps del
juego en: `periodo_juego ≈ trabajo_propio + latencia_engine`. Con evaluate
medido de 113-130 ms, el juego no puede ir más rápido que el engine. La GPU1
al 8%/750 MHz no es "el juego es ligero": es el game thread BLOQUEADO en
nuestra espera, con la GPU starving.

Corrección factual al auditor de transporte: dijo "el ini ya vive en
workScale 0.25" — **falso, verificado: nrWorkScale=1.000 en el gamedir**. El
modelo corre a dims completas. La "componente fija de 15-25 ms que ws no
toca" salía de esa creencia → inválida.

La cuenta que cuadra: 5.4 evaluates/s = 185 ms ≈ 60 (trabajo propio) + 125
(evaluate). El modelo vendor es ~100 ms de los 113-130 (auditor ENGINE,
`breakdown_113ms`: nuestro código ≈8 ms, vendor ≈100, sin medir 17).

**Palanca legal: bajar la latencia del engine.** Cada ms que baja el evaluate
baja el periodo del juego 1 ms. Presentar sin NR (frameskip) está vetado.

## Hallazgos ordenados por impacto (todos ghosting_flicker_risk=false)

### R90 — engine (latencia evaluate = la palanca LEY 1-legal)

| # | Hallazgo | Evidencia | Ahorro est. |
|---|----------|-----------|-------------|
| 1 | **PerfQualityValue hardcodeado a 2.0** (balanced), sin exposure en Tuning/ctl/ini/panel. 0=MaxPerf: el runtime elige passes internos más baratos. La medición R82g (accidente MaxPerf, respuesta al 43% de la altura) prueba que el knob mueve la pata modelo | `nr_vendor.h:723-732` (verificado) | **-45-70 ms** de la pata modelo (el mayor margen honesto) |
| 2 | Produce-wait por polling `Sleep(1)` + `GetTickCount64` — sin `timeBeginPeriod` en todo src, cada wake puede costar un quantum (15.6 ms). Fix estructural: `SetEventOnCompletion` sobre el produceFence ya compartido + wait con timeout 60 s (mismo semántico: timeout no avanza frame, poison = UINT64_MAX) | `loop.cpp:384-392` (verificado) | -0.5-15.6 ms de jitter/frame |
| 3 | 3 FlushSeg/frame (in/vendor/out), cada uno Close+ECL+Signal+wait CPU. Las copias in/out pueden vivir en la MISMA lista que el vendor (la GPU serializa; el vendor ya tolera comandos previos: las guías se graban en el seg 2). Coste: se pierde la atribución fina de error 6/7 (el catch-all cubre) | `loop.cpp:329-347,474-563` | -1-3 ms |
| 4 | Depth-zero-proxy se copia CADA frame siendo constante por diseño (R82f: plano de ceros). Doble desperdicio: host `RecordDepthCapture` (~1.7 MB zeros/frame en la cola del juego) y engine copyGuide (~4.8 MB/frame). Fix: rellenar UNA vez por slot en arm (fence antes de Live) y fuera del camino caliente, EN AMBOS lados. Guard: si el host algún día escribe depth real, la copia vuelve | `offload_session.cpp:1298-1308` + `nr_vendor.h:887-908` | -0.4-1.2 ms GPU + 9.5 MB/s PCIe |
| 5 | `FlowPeakPx()` incondicional cada frame aunque `nrFlowReset=0` (resultado descartado). Mover bajo `if (thr > 0)` | `loop.cpp:441-444` | -0.1-0.3 ms CPU |
| 6 | Alocaciones por frame: 3 `std::vector<float>` de 43200 elems en UpdateGainNorm + 1 en FlowPeakPx (~500 KB churn). Preallocar como miembros (patrón tileLums) | `nr_vendor.h:1092,1127-1129` | -0.1-0.2 ms CPU |

### R91 — host (game thread)

| # | Hallazgo | Evidencia | Ahorro est. |
|---|----------|-----------|-------------|
| 7 | Compose flush: `WaitForSingleObject(fenceEvt, 2000)` tras el compose DENTRO de Present, solo para reciclar 1 allocator. Fix: anillo de 2-3 allocators + recycle no-bloqueante (esperar solo si va realmente detrás). La detección de colgazo ya vive en el watchdog 10 s; el orden del flip lo garantiza el FIFO de la cola, no el flush | `offload_session.cpp:2563` | -0.5-6 ms/Present NR |
| 8 | Hook ECL: SRWLock exclusivo + GetDesc + bucle n×8 en CADA submit de TODAS las colas; y el LogRate del contador corre DENTRO del lock (I/O de fichero con lock tomado, pico cada 5 s). Fix: filtrar cola DIRECT antes del lock; LogRate fuera | `nvngx_host.cpp:189-234` | -5-25 µs/frame + mata picos |
| 9 | Log: CreateFileA+WriteFile+CloseHandle POR LÍNEA (x2 con rotación) bajo 1 CS. Handle persistente + rotación barata | `log.cpp:25-117` | mata picos 0.3-3 ms/5 s |
| 10 | Comentarios mienten: dicen "nuestra propia cola" para el compose; el código usa (correctamente) la cola del juego — el FIFO ordena el flip. Solo doc (estilo R89d), NO tocar la cola | `offload_session.cpp:2452-2460` vs :17-19 | 0 (evita futuras rupturas) |
| 11 | Health poll por evaluate (`WaitForSingleObject(pi.hProcess,0)`, ~1-3 µs): es RUIDO y es el check A3 de R89a — **no tocar** (seguridad > µs) | `offload_session.cpp:2240` | 0 (decisión: conservar) |

### Lo que NO se hace (rechazados con motivo)

- **Copy queue / async compute para las copias**: mismo enlace PCIe, fences
  extra cuestan más de lo que ahorran (~1-2 ms). Sin medir = no tocar.
- **Eliminar el transporte de depth / meter depth real**: veto R82f (envenena
  el runtime) + cambiaría lo que el runtime lee (fuera de contrato sin A/B).
- **flowReset hacia arriba**: 0 ms de ganancia (el reset es arg del evaluate,
  mismas pasadas) y devuelve el ghost desplazado (medido R84: umbral 15 dejó
  pasar medio paneo). El reset ES el anti-ghosting. NO TOCAR.
- **gate-wait como estrategia de fps**: hoy inerte (0 ms: el delta ya está al
  llegar Present cuando el engine es más rápido). Sigue siendo válvula
  opcional, nunca objetivo (LEY 1 + verbatim user).
- **Reconfig inmediata ante submit mismatch**: hoy nunca ocurre (numeración
  host==N correcta desde c12aee9); añadiría un latch nuevo sin caso medido.
  Documentado, diferido.
- **Hazard latente documentado**: el engine indexa el anillo de guías con su
  contador privado (`loop.cpp:418 ++frame`) y el sello con el del host. En
  lockstep coincide; si algún knob pusiera al juego POR ENCIMA del engine, el
  engine evaluaría el slot equivocado (la compuerta host==N impide componer
  la mezcla: degradaría a nativo). Fix barato cuando toque: indexar con
  `sealedFrame % 3` (la variable ya existe, R89a fixup). Lo incluimos en R90
  (#12) porque los knobs de R90 acercan ese escenario.

### Knobs: tabla honesta (auditor TRANSPORTE)

| Knob | Gana fps | Coste |
|------|----------|-------|
| **PQV 2→0** (nuevo, R90) | -45-70 ms de evaluate | NR más suave, nunca desplazado; flowReset intacto |
| **workScale** (ya expuesto, hoy 1.000) | escala la pata modelo con píxeles | raster más suave |
| detail/colour/autoSkin/gainBound | **0 ms** (scalars del decode) | solo calidad/A-B; gainBound>2 reabre ghost |
| flowReset | **0 ms**; subirlo = ghost | reset = anti-ghosting |
| gateWait | **0 ms hoy** | solo muerde si engine<juego |

## Plan de ejecución

- **R90** (engine): #1 PQV expuesto (ctl id 40, CTL_FIELD_COUNT 40→41,
  Tuning reserved8→perfQuality float @8, re-arm ~1 s, fila de panel honesta
  con coste de calidad; default sigue 2 — el user decide con A/B) + #2
  produce-wait por evento (estructural) + #3 un FlushSeg + #4 depth-zero una
  vez en arm (ambos lados) + #5 guard FlowPeakPx + #6 prealloc vectors + #12
  anillo indexado por sealedFrame%3.
- **R91** (host): #7 anillo de allocators del compose + #8 hook ECL afilado +
  #9 log con handle persistente + #10 corrección de comentarios.
- **Validación A/B** (protocolo): misma escena, PQV 0↔2 desde el panel
  (re-arm ~1 s), leer `vendor frame N: DELTA (ms)` + composes/120 s + overlay
  fps/1% low. Criterio de éxito: evaluate ≤90 ms y fps del juego sube con el
  engine. Criterio de parada: CUALQUIER ghosting/flicker → revert (vetado).
  Si PQV baja el evaluate pero el juego sigue a 5 fps → el lockstep queda
  refutado, la pared es el propio juego (CPU-bound) y se reporta honesto:
  el offload deja de ser el limitador.
- Estimación total honesta: evaluate 113-130 → **~50-80 ms**; juego NR de
  4.4 → **7-9 fps** SI el modelo de lockstep es correcto. Números a A/B, no
  promesas (anti-inventos).

## Addendum — reconciliación con worklog 33 (análisis del mecanismo, 39a8255)

El worklog 33 confirma el diagnóstico (evaluate = cómputo del modelo a res
display; transporte NO es cuello; el gate estrangula el juego). Añade 3 piezas
que entran al plan y una discrepancia que el A/B resolverá:

1. **A/B inmediato SIN código** (protocolo §validación, orden):
   - `nrPreSr=1` (ctl 30, ya en panel desde R89c): sello de color a res
     RENDER (1.23 Mpx vs 2.76) → modelo ~2.25× más barato (≈50-60 ms) y
     transporte color+gain 42→18.9 MB. El decode ya compensa el jitter.
     Opt-in desde R82h por el veredicto de jitter (calidad), no por coste —
     re-abrir el A/B con la escena actual.
   - `nrWorkScale 0.5`: hoy BLOQUEADO por calidad (divergence #1: el
     downsample lee blur como headroom) hasta tener matched-residual (#2).
   - PQV: requiere la exposición de R90 #1 (hoy hardcodeado).
2. **Matched-residual (R90b, estructural, referencia NeuRotic/ficha
   Cost-Scaler)**: `edit = smallOut − smallIn` en dominio display, componer
   el EDIT y no la imagen → desbloquea ws=0.5 legal (engine ~15-25 ms, el
   juego vuelve a cadencia nativa). Verificable OFFLINE con el replay_feeder
   antes de tocar el juego. Es el "THEY-BETTER #1" nunca portado; 0 hits de
   `residual` en src/ (verificado en el worklog).
3. **Output ring 2-3 slots (R91b, wiki 22 §2.1, patrón UNCANNY)**: hoy `out`
   es UN buffer — el engine no puede empezar N+1 hasta que el gate stageó N.
   El ring desacopla producción y consumo; con skip-to-newest evita evaluar
   frames viejos. Entra cuando los knobs de R90 acerquen el engine al ritmo
   del juego (misma condición que el hazard del anillo de guías #12).
4. **Discrepancia PQV**: worklog 33 §2.4 estima efecto "pequeño" (coste
   geométrico); los auditores ENGINE/TRANSPORTE estiman -45-70 ms con la
   evidencia del accidente R82g (PQV caído a MaxPerf → respuesta al 43% de
   la altura ≈ 0.18× píxeles internos). La evidencia empírica (accidente
   medido) pesa más que la extrapolación geométrica, pero AMBAS son
   estimaciones: el A/B PQV 0↔2 decide y el plan no promete número.

Techo honesto compartido (worklog 33 §5 + auditores): el offload no acelera
el juego por encima de su nativo — deja de estrangularlo. Si tras R90 el
evaluate entra bajo el tiempo de frame del juego y los fps NO suben, la pared
es el propio RDR2 (CPU-bound) y se reporta como tal.
