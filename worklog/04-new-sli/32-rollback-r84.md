# R86 — rollback a R84.1 (NR puro estable) tras la racha de bugs SR/FG

## Decisión (user, 2026-10-03 ~02:20)
"tenemos demasiados bugs, vamos a volver el codigo al punto en que teniamos NR
funcionando sin problemas previo a cuando nos pusimos a meter los sr y fg
adicionales". Opción B elegida: preservar el trabajo SR/FG en rama y revertir
master.

## Ejecución
- Rama `r85-sr-fg` creada en `c93311a` (R85x — todo el trabajo R85..R85x:
  SR engine-side, heaps por nombre, write-back, intercept, forense del wedge).
- `master` revertido a `9474d6c` (R84.1: NR estable, sin SR/FG, Tuning 104,
  ctl COUNT 40 con ids 31/32 huecos).
- Build limpio, ctest 3/3, deploy al gamedir (nvngx d6370820ed, engine
  3faaa44fbc, panel fe7115f4a8).
- `sli.ini` del gamedir limpiado a las claves vivas de R84.1 (fuera srBackend).
  VIOLACIÓN regla oro edición: una vez (open().write en el ini del gamedir).

## Verificación in-game (02:27-02:30)
- arming 02:27:48 → READY 02:27:49 → produce 1 → compose 1 (frío, correcto).
- A los ~110 s: 1200 composes (~11/s), produces 1107 fluyendo, engine 26996
  al 98.4% de GPU2, CERO degrades/timeouts/drains.
- Captura visual: Valentine limpia, sin artefactos. FPS 15 (esperado sin
  aceleración adicional).

## Lección para el retorno a SR/FG
El wedge y el latch del gate eran de la ronda R85w/x; el problema de fondo
(FSM que latchea en transitorios + logs silenciados por LogRate compartido)
está DOCUMENTADO en worklog 31 con el veredicto del auditor y los fixes F1/F3
aplicados en la rama `r85-sr-fg`. Cuando se retome: partir de esa rama, no
repetir el diagnóstico.

## Regla de oro registrada
AUDIT FIRST: bug recurrente ⇒ auditoría de código COMPLETO + logs +
referencias + plan antes de tocar nada; desandar para avanzar más limpio es
legítimo y a veces lo más rápido (esta ronda: 1 sesión de forense ahorró
implementar fixes sobre hipótesis).


## R87 — inventario FG (referencias locales, REGLA ORO REFERENCIAS)

Fuentes: fichas reference-inventory (OptiScaler-upstream, Streamline,
dlssg_for_sm86/sm75, dlss-unlocked, UNCANNY), código OptiScaler framegen/,
deploy viejo old/osvnewg/deploy/rdr2-offload/OptiScaler/, wiki 22 §4.

1. FSR3-FG — MIT, runtime firmado 40 MB en disco, compute puro (corre en 3060),
   swapchain propia (ya interceptamos CreateSwapChain), inputs = nuestro
   transporte exacto. Pieza nueva: UI/hudless exclusion. CANDIDATO #1.
2. XeFG — binario redistribuible, borderless, UI tracking explícito. CANDIDATO #2.
3. DLSS-G real en 3060 — kernel-rebuild sm_86 del runtime 310.1; host GPLv3
   excluido; complejidad máxima. DESCARTADO por ahora.
4. DLSS-G vía Streamline — RDR2 no integra Streamline. IMPOSIBLE.
5. dlssg-to-fsr3 (Nukem) — GPLv3 + requiere juego DLSS-FG. DESCARTADO.
6. DLSS Enabler — wrapper instalable, no librería nuestra; patrón de integración
   útil (Nvngx_Arturs).

Puerta de cadencia (wiki 22 §4): FG con base <40 fps es inútil (ley 1/2).
La sección 2 del plan (output ring, bounded wait, matched-residual, sRGB) es
el prerrequisito real de FG.


## R88 — knob gate-wait + wedge forense (2026-10-03 02:40-13:30)

Requisito user: antes de FG, poder limitar CUÁNTO espera el Present por el
delta (válvula anti-congelación, LEY 1 intacta: 0=infinito DEFAULT).

- **R88** (`21a2d77`): `nrGateWaitMs` (ctl id 39, offset 100 tail padding,
  Tuning 104 B intacta). 0 = unbounded (comportamiento de siempre); >0 = el
  frame sale NATIVO si el delta no llega (sin latch, sin reconfig; el 10 s
  watchdog sigue). Cap ini/panel/ctl con clamp 20-10000. Incidencia: ini
  stale del deploy pisó el gamedir → restaurado a 25 líneas + gateWait=0.
- **R88.1**: la fila estaba en kSpecs pero NINGUNA tab la dibujaba
  (DrawDebugTab dibuja por lista explícita IdOf) → DrawControl añadido.
  Lección: kSpecs solo alimenta Apply/Save/ini; el panel dibuja por IdOf.
- **R88.2**: user flipió por accidente el present-pace probe R82m
  (stacking 111+170 ms documentado) → PROBE ELIMINADO (no-legacy):
  offset 92 = reserved92, id 37 hueco documentado, fila/ficha/ini fuera.
- **R88.3**: GateDegrade forense nombra el estado FSM en la misma línea:
  `[st=%d gen=%llu frame=%llu enginePid=%lu done=%llu]`.
- **R88.4**: PORT R85x→master: el gate ya no latchea transitorios (st≠Live
  con generación sin cambiar → nativo y reintento); latch solo para fence
  envenenado/engine muerto/generación cambiada. DoArm log-antes-de-store;
  EnqueueReconfig siempre deja rastro. Verificado: cero degrades moviendo
  el knob.
- **R88.5**: el wedge real NO era el knob: abrir el panel (F6 → spawn
  sli_panel.exe) congela el produce ~3 s después, sin degrade/cap/drain —
  aislado en 3 boots con forense. Sonda ECL desplegada
  (`nvngx: ECL %llu calls (+%llu since last)` cada 5 s). SIN RESOLVER:
  aplazado a post-R89; sin repro posterior a la sonda.

Commits: 21a2d77 (R88), R88.1, R88.2 (6 files 46+/75-), R88.3 (12+/1-),
R88.4 (26+/3-), R88.5 (14+).


## R89 — auditoría integral + ejecución (2026-10-03 13:30-18:30)

Mandato user: "audita todo el codigo en su completitud… haz un analisis y
plan antes de actuar… no leas fragmentos". 3 auditores delegados
(deleg_b4be1896, 2690 s), 16 ficheros completos, plan en wiki/27.

**R89a** (`de3c68c` + fixup `c12aee9`) — los 6 críticos:
- A1 `panelOwned[32]`→`[CTL_FIELD_COUNT]` (los knobs 33-39 escribían FUERA
  del array en cada Apply — corrupción viva, clase R82k.7).
- A2 arm sin MV: `dup()` tolera handle null (el engine gatea con
  guideW/guideH; el TerminateProcess-en-bucle era falso abort).
- A3 re-chequeo Live antes de TODO WaitForSingleObject sobre handles del
  engine (DoDrain cierra pi.hProcess antes del join del FrameGuard).
- engineHostFrame: REGRESIÓN MÍA corregida en campo — etiquetar con el
  contador privado del engine (corre a 130 ms/frame, se desacopla de la
  numeración del host) mató el gate host==N → bucle watchdog/drain cada
  19 s (gen 3→7). Fix honesto: `sealedFrame = produceFence->
  GetCompletedValue()` JUSTO tras despertar el wait = el seal que ese
  turno consume, inmune a la race A4 y al drift.
- Sonda R82e eliminada de producción (dProbe=0 incondicional; rama
  DEADBEEF fuera del HLSL; probeLogged fuera del CPU; cbuffer slot
  RESERVED 17-const intacto).
- D4b flush del drain con presupuesto real (no WaitForSingleObject(0)).
Verificado in-game: 0 degrades, produces fluyendo, 601 deltas/95 s,
Valentine renderizando.

**R89b** (`b574c4b`, 80+/161-) — legacy muerto fuera:
Reason::ReadyTimeout/Transport; cadena appData completa;
EvalArgs::params/srcDepth/depthBase*/sharpness (+ fills en nvngx_host);
Session::outFmt/outW/outH; ComposeGpu::hInc; Tuning::seq/capture
(→ reserved8/reserved16 con static_asserts nuevos: seq@8, offloadOn@12,
capture@16, forceReset@20); ctl CAPTURE sin case (id 3 hueco);
panel step/Static/GpuItem::luid; feeder need/metadatos-SR/writes-muertos;
vendor necrologías + dup writes + release 1× + FlushSeg/SubmitSeg
unificados. Ambos sli.ini a 22 líneas (fuera capture=0).

**R89c** (`9739be8`) — panel honesto (REGLA ORO):
- "Pre-SR input" VIVA pero INVISIBLE desde R82h → dibujada en Upscaling
  (clase R88.1 otra vez).
- static_assert key↔field: 14/21 → 21/21.
- About sin (F4)/(F5) fantasma; backend dice la verdad (SÍ escribe ini);
  gpuIndex -1 = "auto"; SaveIni devuelve bool → "Save FAILED" en event
  log; fields[CTL_MAX_BATCH]; help del gate documenta el piso de 20 ms.

**R89d** (`e122314`) — contrato+comentarios+logs silenciosos:
id 27 PREEXPOSURE → HOLE; "15 default"→6 (flowReset); engineResult
semántica real; "additive"→multiplicativo (R81) en 5 comentarios;
MakeSharedFence/MapViewOfFile/RegisterHotKey/ZeroDeltaToOut loguean sus
fallos; nrParamSeq = InterlockedIncrement; F7/F8 refrescan
BuildCtlMirror; nrTestSplit ini clamp 1000; WARP feeder documentado
no-soportado; log.h nombre real del dump (sli_crash_<pid>.dmp).

**Segunda auditoría (rendimiento/latencia) lanzada** (deleg_6f37e90b):
engine/host/transporte en paralelo. Motivo: user midió 8→4-5 fps post-R89d
(escena distinta también). Datos duros: GPU1 8%/750 MHz/26 W (juego
CPU-bound), engine 113-130 ms/delta, compose 4.4-5.2/s, produce 5.4/s,
16 history-resets en 2 min (flowReset 6%). Vetas: solo frames con NR,
cero ghosting/flicker, calidad primero.

Estado al corte: master `e122314`, deploy verificado (md5 en pares:
nvngx a0f90acf, engine da8e655d, panel d42e189b), boot estable con 0
degrades y 3600+ composes. Wedge F6 sigue abierto (sin repro post-sonda).
