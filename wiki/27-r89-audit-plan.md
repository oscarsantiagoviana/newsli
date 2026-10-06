# R89 — Plan de auditoría y limpieza integral (2026-10-03)

Tres auditores forenses, 16 ficheros leídos completos (offload_session.cpp/.h,
panel ×5, engine ×3, shared ×3, tools ×3). Los hallazgos consolidados, sin
duplicados, con el plan de ejecución. NADA se toca hasta aprobar el plan.

## 1. CRÍTICOS — defectos reales (arreglar primero)

| # | Dónde | Qué | Acción |
|---|---|---|---|
| A1 | offload_session.cpp:453 | `panelOwned[32]` vs `CTL_FIELD_COUNT=40` — los knobs 33-39 escriben FUERA del array (corrupción real de memoria; el mismo bug R82k.7 repetido) | ARREGLAR → `panelOwned[CTL_FIELD_COUNT]` |
| A2 | SpawnEngine :1529-1557 | arm sin MV (late-guides) → guías `nullptr` → `dup()` hace TerminateProcess en bucle (spawn abort → cooldown → reintento infinito) | ARREGLAR: dup tolera null + engine abre condicional |
| A3 | DoDrain :1871-1905 vs game thread | reap del engine (CloseHandle) ANTES del wait de g_frameBusy → el game thread puede esperar sobre un handle cerrado (UB) | ARREGLAR: re-chequear Live antes de los waits |
| D4b | DoDrain :1917 | budget agotado → `WaitForSingleObject(timeout 0)` → log falso "flush timeout" sin haber esperado | ARREGLAR: presupuesto mínimo de flush |
| 8 (engine) | loop.cpp:584 | `engineHostFrame = GetCompletedValue()` puede taggear un frame FUTURO (el host sella N+1/N+2 durante el evaluate) — el gate exige `host == N` | ARREGLAR: usar `frame` tras confirmar contrato |
| 11 (vendor) | nr_vendor.h:1038 | sonda R82e (`seq==2 → dProbe=1.0`) dispara en el frame 3 de CADA arm en producción: contamin UpdateGainNorm | ELIMINAR (0.0 incondicional) |

## 2. LEGACY MUERTO — eliminar (regla no-legacy)

Host: `Reason::ReadyTimeout`/`Transport` (:141-142), cadena `appData` completa
(campo + SetAppDataPath + 2 llamadores), `ComposeGpu::hInc`, `EvalArgs::params/
srcDepth/depthBaseX/Y/sharpness` (+ relleno en nvngx_host), `Session::outFmt/
outW/outH`, slots PSO 1-3 (kMaxPso=5 → 2; el LRU del comentario nunca existió),
flag `capture` sin consumidor (id 3 → hueco), Tuning::seq (solo lo bumpéa F7,
el engine lo ignora).
ABI: `Handshake::hid/him` (16 B muertos, sin ABI cruzada que preservar).
Panel: campo `step` (23 inicializadores), `Ctrl::Static` sin filas,
`GpuItem::luidLo/Hi` write-only.
Feeder: `LoadRaw::need` muerto, metadatos SR legacy (df/mf/ow/oh/flags/mw/mh),
writes muertos a Tuning (offloadOn/gpuIndex).
Vendor: comentario-necrología L149-150, L938-942, L1217-1221, "1.355 ratio" L3;
dup writes PerfQualityValue/ScalingRatio en Init; `dlssnr_call_release`
resuelto 3× (guardarlo una).
Duplicación: FlushSeg≡SubmitSeg (un solo lambda), 3 catch idénticos (helper).

## 3. PANEL — REGLA ORO (texto honesto, ningún control muerto)

- fila kSpecs "Pre-SR input" (ctl 30) VIVA pero INVISIBLE — no se dibuja en
  ninguna tab (mismo fallo que el gate-wait en R88.1) → dibujarla en Debug
- "(F4)" y "(F5)" del About: atajos que no existen → quitar
- backend: "no ini key" es falso (Save sí escribe `backend`) → corregir texto
- gpuIndex -1 = "vendor default" (es AUTO) → etiqueta por fila
- static_assert key↔field cubre solo 14 de 21 filas → extender con las 7
- SaveIni ignora el retorno → "Save failed" en el event log
- fields[8] literal → CTL_MAX_BATCH
- documentar el piso de 20 ms del gate wait en el help

## 4. CONTRATO/LIMPIEZA

- id 27 PREEXPOSURE: declarado vivo, el host no tiene case → marcar HOLE
  (append-only) como 31/32/37
- ctl_common.h:117 "15 default" para flowReset → 6
- abi.h:46 engineResult: documentar semántica real (1 = entregado OK)
- "additive" obsoleto en 5 comentarios (modo = multiplicativo desde R81)
- root signature 2 SRVs sobre heap de 1 → 1 descriptor
- comments: "same bind shape compute path", "spec order game-override rows",
  log.h dump name, log.cpp psapi, host_smoke 1.5 s vs 3000 ms
- MakeSharedFence/MapViewOfFile(HOST)/RegisterHotKey/ZeroDeltaToOut/loop:133:
  fallos sin log → log
- seq++ de 3 threads → InterlockedIncrement; mirror stale tras hotkeys →
  BuildCtlMirror en F7/F8
- WARP del feeder roto (LUID {0,0}) → documentar no-soportado
- nrTestSplit ini sin clamp → paridad con ctl
- self-check param-block por-Init → mover a diagnóstico explícito

## 5. Orden de ejecución propuesto

1. **R89a — críticos**: A1 (1 línea), A2, A3, D4b, engine:8, vendor:11
2. **R89b — legacy host+ABI**: sección 2 completa (deletions puras)
3. **R89c — panel**: sección 3 completa
4. **R89d — contrato+comentarios+logs**: sección 4
5. Cada ronda: build 0 warnings, ctest 3/3, deploy, commit separado.
   Verificación in-game tras R89a y al final.

## 6. Verificaciones sin hallazgo (constancia)

Sin referencias vivas a srBypass/srRatio/nrModelH/present-pace/intercept R85g.
static_asserts ABI cuadran campo a campo (104 B Tuning, 144 B Handshake, 440
CtlMsg). Formats/footprints engine↔feeder consistentes. Panel: tag re-arm
ámbar correcto en las 22 filas dibujadas; snapshot applied atómico.

## Nota sobre el wedge F6

La auditoría no cubrió nvngx_host.cpp a fondo (solo cruces). El produce-freeze
tras F6 sigue abierto con la sonda ECL (R88.5) — la limpieza de A1/A3 también
pasa por código que el wedge toca (ctl thread, drains), así que R89a se
re-valida contra el repro del panel antes de darlo por cerrado.

---

# EJECUCIÓN COMPLETADA (2026-10-03 16:30-18:30) — estado por hallazgo

## Ronda R89a — críticos (commits `de3c68c` + fixup `c12aee9`)

| # | Estado | Notas |
|---|---|---|
| A1 | ✅ | `panelOwned[CTL_FIELD_COUNT]` + comentario del porqué |
| A2 | ✅ | `dup()` tolera null (legal late-guides); fallo REAL sigue abortando |
| A3 | ✅ | Live re-check ×2 (evaluate health poll + done wait) |
| D4b | ✅ | flush con presupuesto propio (mín. 2000 ms aunque el drain esté agotado) |
| 8 | ⚠️✅ | la primera fix era INCORRECTA: el contador privado del engine se desacopla del host → bucle watchdog en campo. Fix final: `sealedFrame` = valor de la produce fence justo tras despertar el wait (el seal que el turno consume; inmune a la race A4 Y al drift) |
| 11 | ✅ | dProbe=0 incondicional; rama DEADBEEF fuera del HLSL (cbuffer RESERVED); probeLogged fuera |

**Lección R89a**: el contrato host==N exige la numeración DEL HOST. El
contador privado del engine corre a model-speed y se queda atrás para
siempre cuando produce > evaluate rate. La única etiqueta correcta es la
fence leída en el punto de consumo.

## Ronda R89b — legacy (commit `b574c4b`, 80+/161-)

Todo lo de la sección 2, MÁS lo descubierto al ejecutar: capture (fila
panel + assert + ini en ambos lados), id 3 hueco documentado. Pendiente
de esta lista: **slots PSO 1-3 NO se tocaron** (kMaxPso sigue 5: el coste
es 3 ComPtr nulos, la "reducción" era cosmética y arriesgaba tocar el
compose por estética — decisión: dejarlo, anotado como deuda menor);
**3 catch idénticos NO unificados** (los contexto difieren: re-arm vs
degradar; el helper ahorraba 6 líneas y perdía claridad).

## Ronda R89c — panel (commit `9739be8`)

Sección 3 completa EXCEPTO la ubicación de Pre-SR: se dibujó en
**Upscaling** (decide dónde lee el input el modelo), no en Debug como
decía el plan original — revisión de ubicación, no cambio de fondo.

## Ronda R89d — contrato/logs (commit `e122314`)

Sección 4 completa EXCEPTO: "self-check param-block por-Init → diagnóstico
explícito" (no tocado: el self-check es el que detectó el bug del float
slot en R82g; moverlo sin una necesidad real es riesgo sin beneficio).
Root signature: no se cambió el heap a 1 descriptor "porque sí" — el
rango t0-t1(2) sobre heap(1) ya era correcto (t1 out-of-heap = negro,
que es lo que el path tint!=2 quiere); ahora lo EXPLICA el comentario.

## Verificación final

- Build 0 warnings /W4 /WX, ctest 3/3 en cada ronda.
- Deploy md5 en pares: nvngx a0f90acf, engine da8e655d, panel d42e189b.
- Boot 18:05 (post-R89d completo): 0 degrades, 0 drains, 3600+ composes,
  produces fluyendo, sesión estable en partida.

## Siguiente: segunda auditoría (RENDIMIENTO)

User midió 8→4-5 fps tras R89d (escena distinta también; GPU1 8%/750 MHz/
26 W = juego CPU-bound). a delegated audit: 3 auditores (engine 113-130 ms/
frame desglose, host game-thread µs, transporte+knobs honestos). Vetas:
LEY 1 (solo frames con NR; gate-wait = válvula existente), cero
ghosting/flicker, calidad primero (LEY 2).
