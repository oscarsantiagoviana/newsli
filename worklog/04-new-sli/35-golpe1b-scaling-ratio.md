# 35 — Golpe 1b: soltar la válvula ScalingRatio (PQV real)

Fecha: 2026-10-03 (tarde-noche). Padre: worklog 34 (golpe 1).

## Contexto

El A/B PQV post-probe fue DECISIVO y NEGATIVO: knob 2→0 viaja (panel→host→
engine→create `pqv 0.0`), se ALMACENA (self-check uint rc OK), y el modelo
sigue a ~137-153 ms (n>700). El dial es inerte con el código actual.

Contraste con referencias (regla de oro, user-mandated) — 3 hallazgos:

1. **enum real** (nvsdk_ngx_defs.h del fork): 0=MaxPerf, 1=Balanced,
   2=MaxQuality, 3=UltraPerf, 4=UltraQual, 5=DLAA. Nuestro default 2.0
   histórico = **MaxQuality** (no "balanced" como decía el panel).
   Hay DOS niveles más baratos (0 y 3) por debajo de lo que siempre pagamos.
2. **PerfQualityValue no es un dial de coste directo**: es el INPUT de
   `DLSSNR.ScalingRatio`, que el propio modelo calcula mediante callback
   (`DLSSNRComputeScalingRatioCallback`, publicado por el snippet vía
   PopulateParameters_Impl). El precio real lo manda el ratio (el modelo
   trae kernels `_ds`/`_upsample` para trabajar por dentro a resolución
   reducida NATIVAMENTE).
3. **Nosotros clavamos `DLSSNR.ScalingRatio = 1.0` en el create**
   (nr_vendor.h:747) — el comentario propio lo confiesa: "pinned 1.0 keeps
   the model at work dims regardless". Herencia del fix R82g del
   "43%-height". La válvula está soldada por nosotros: PQV no puede
   abaratar nada.
   - El fork en create NO escribe ni PerfQualityValue ni ScalingRatio:
     deja que el modelo los calcule (dlssnr_forwarder.cpp:835-856).
   - Nota: el forwarder desplegado YA exporta
     `dlssnr_query_scaling_ratio` (verificado tabla de exports).

## Hipótesis

Si soltamos ScalingRatio (no escribirlo) y entregamos un PQV válido, el
modelo calculará su ratio propio para ese nivel y ejecutará su interior a
resolución reducida → evaluate más barato. La respuesta (texOutModel)
llegará con geometría distinta (área menor), y nuestro decode la integra a
render dims YA HOY vía workScale... PERO la geometría de la respuesta a
ratio<1 NO está verificada (el accidente R82g midió "43% de la altura" con
PQV accidental MaxPerf — consistente con ratio≈0.43 o el modelo escribiendo
solo la parte superior).

## Plan paso a paso

### Paso 1 — Sonda ratio (solo lectura, sin riesgo)
Añadir a nr_vendor.h (tras getCap + discover_float_slot, antes del create):
- Resolver `dlssnr_query_scaling_ratio` del forwarder.
- Llamarla para q=0..5, loguear `DLSS-NR scaling ratios: MaxPerf=…
  Balanced=… MaxQuality=…` (patrón fork DlssNr_Dx12.cpp:1100-1131).
- También leer de vuelta `DLSSNR.ScalingRatio` tras create (¿lo escribió
  el modelo? ¿con qué valor?) y tras N evaluates.
- ESTO RESPONDE: ¿el modelo de este driver publica ratios? ¿cuáles?

### Paso 2 — Feeder: knob SLI_FEEDER_PQV + SLI_FEEDER_RATIO_PIN
- `SLI_FEEDER_PQV=<float>` → tuning->nrPerfQuality (default 2).
- `SLI_FEEDER_RATIO_PIN=<float>` → NUEVO campo tuning (ver paso 3) para
  A/B: pinned=1.0 (comportamiento actual) vs suelto (omitir la escritura).
- El feeder ya valida identidad bit-exacta y captura deltas; añade
  impresión de la geometría de la respuesta (bounding box no-cero) para
  verificar dónde escribe el modelo a ratio suelto.

### Paso 3 — ABI: campo nrRatioPin (float) en Tuning
- reserved16 @16 (R89b renombrado sin mover offset) → `nrRatioPin`.
- 0.0 = suelto (no escribir ScalingRatio; el modelo manda) [DEFAULT
  ESTRUCTURAL tras validar].
- >0 = pin a ese valor (debug/rollback). Panel: slider "Scaling pin" 0-1.5
  con explicación honesta (0 = el modelo elige su proporción de trabajo
  según Perf quality).
- static_assert offsetof == 16.

### Paso 4 — Engine create: soltar condicionalmente
```cpp
if (tuning->nrRatioPin > 0.0f)
    set_flt(P, fslot, "DLSSNR.ScalingRatio", tuning->nrRatioPin);
// else: NO escribir — el modelo calcula el suyo para el PQV dado
```
+ RebuildKnobs igual (pqv + pin viajan por nrParamSeq).
+ Log tras create: leer `DLSSNR.ScalingRatio` de vuelta →
  "vendor: model ratio = X.XX (pqv Y)".

### Paso 5 — Decode con geometría variable
Hoy el decode asume respuesta full-work-dims (modelLiveW/H = workW/H,
modelTop=0). Con ratio suelto la respuesta puede llegar:
  a) full dims pero contenido solo en la fracción superior (R82g), o
  b) dims físicas menores del buffer (subrect).
- Paso 5a (detector): tras N evaluates, readback de texOutModel (solo
  offline/feeder) → bounding-box no-cero → log. Decide a) vs b).
- Paso 5b: DecCb ya soporta modelLiveW/H/modelTop (heredado del fix
  R82g) — parametrizarlos con la geometría detectada + el ratio leído
  del bloque tras create. La integración work→render del decode es la
  misma (coverage box ya generaliza).
- RIESGO controlado: geometría mal leída = imagen rota (no ghosting);
  visible en feeder delta dump y capturas; NUNCA se despliega a ciegas.

### Paso 6 — Panel: corregir etiquetas PQV + fila nueva
- "Perf quality" descripción corregida: escala real 0=MáxRendimiento…
  4=UltraCalidad; "2 = máx calidad (lo que siempre ha corrido)".
- Fila "Scaling pin" (0=suelto/1.0=clásico) con texto honesto: "0 deja
  que el modelo trabaje a su resolución interna preferida según Perf
  quality — la fuente del ahorro; 1.0 lo fuerza a resolución completa
  (el comportamiento de siempre)".

### Paso 7 — Validación offline (feeder)
1. SLI_FEEDER_PQV=0 RATIO_PIN=0 → log ratios + ratio post-create +
   bounding box + identidad de delta en zona fuera-de-respuesta.
2. Igual con PQV=2 → comparar ratios y bounding boxes.
3. Si geometría = caso a) (fracción superior): DecCb modelLiveH=ratio*H
   (misma forma que R82g fix) → re-ejecutar feeder → delta correcto.
4. Si caso b): DecCb sobre subrect (modelLiveW/H = dims físicas).
5. Timing: comparar DELTA ms PQV2-pin1 vs PQV2-suelto vs PQV0-suelto.

### Paso 7b — Validación in-game (con user delante)
A/B triple: pin1/pqv2 (base) → suelto/pqv2 → suelto/pqv0. Capturas x2 +
fps overlay. Criterio LEY 2: calidad debe ser indistinguible o mejor;
si el modelo entrega su "resolución interna" con remontaje nativo, la
imagen NO debe perder vs pin=1.

### Paso 8 — Docs + panel + commit + deploy
- wiki 28 addendum: PQV no era dial directo; ScalingRatio soldado por
  nosotros; ratios del modelo medidos.
- worklog 35 ejecución.
- Panel con filas nuevas + textos corregidos (REGLA ORO PANEL).
- Commit único "golpe 1b" + deploy + relaunch (kill previo, md5s).

---

## EJECUCIÓN (21:0x) — RESUELTO: la válvula no existe en este driver

- Implementado TODO el plan técnico (sonda q=0..5, válvula nrRatioPin
  ABI@16, ctl 41 + mirror[42] (CtlMsg 456), create condicional con
  asentamiento post-probe, readback post-create, panel "Scaling pin" +
  textos PQV corregidos, feeder SLI_FEEDER_PQV/RATIO_PIN). Build 0
  err/warn, ctest 3/3.
- **Paso 1 (sonda) respondió de fuente primaria**: el callback del propio
  modelo devuelve **ratio = 1.0000 para TODOS los niveles** (MaxPerf,
  Balanced, MaxQuality, UltraQual, DLAA; UltraPerf "refused"). En este
  driver/versión del modelo NO existe el interior a resolución reducida.
  El hipotético ahorro de PQV no existe aquí.
- Feeder (válvula ABIERTA, capturas reales POC): G2 contenido 9/10,
  G3 estable; DELTA ~41-43 ms (offline, sin juego). PQV 0 vs 2:
  **idéntico** (41.5-43.6 ms ambos) — consistente con ratios todos-1.
- Post-create readback: ratio ausente (rc BAD00010) — el modelo no
  reescribe la clave; el create corre con lo que el callback dejó
  (nuestro asentamiento). Sin efecto porque todos los ratios = 1.
- CONCLUSIÓN (hONESTA, LEY 2): PQV/ScalingRatio NO es palanca de coste
  en esta build del modelo. El pin=1.0 histórico NO estaba estrangulando
  nada. Golpe 1b se cierra como hallazgo forense: knob honesto en panel
  (Perf quality explica la escala real; Scaling pin documentado como
  válvula de debug), knob inerte documentado como tal.
- Deploy final: nvngx 448711c6… / engine 3e8b6069… / panel e4009a2a…,
  juego relanzado 21:00.
- SIGUIENTE (golpe 2): depth — hipótesis user: la guía no se entrega en
  el formato/dims que el modelo espera (contrastar referencias ANTES).


## Criterios de éxito
- Engine log muestra ratios reales del modelo (q=0..5) y ratio post-create.
- Feeder valida geometría de respuesta + delta correcto a ratio suelto.
- In-game: evaluate cae de ~140 ms proporcionalmente al ratio (si MaxPerf
  ratio≈0.5 → esperado ~60-70 ms); fps del juego sube en la misma medida.
- Calidad in-game sin pérdida visible (LEY 2) — verificado con user.

## Riesgos
- R1: el modelo ignora el ratio suelto (no publica callback / create
  falla) → paso 1 lo revela antes de tocar nada; el pin queda default.
- R2: geometría de respuesta distinta a a)/b) → detector de bounding box
  la mide; decode generaliza o abortamos (no inventos).
- R3: calidad visible peor a ratio suelto → LEY 2 manda: documentar,
  revertir a pin=1 por defecto, cerrar el hallazgo honestamente.
