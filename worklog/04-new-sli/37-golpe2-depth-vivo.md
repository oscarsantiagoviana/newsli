# 37 — Golpe 2 fase viva: depth REAL del juego por la cadena de transporte

Fecha: 2026-10-03 (noche). Padre: worklog 36 (comparativa + feeder). Go user.

## Base

R82f refutado en feeder (worklog 36): depth log real del juego fluye limpio
(rc=1, delta lleno, sin mono-stripe) con el etiquetado invertido del propio
create del juego. La maquinaria viva de captura EXISTIÓ hasta R82f (R69t):
clone idéntico + CopyResource + plane-0 → guideD. Se recupera de git
(fc4810a~1) — regla de oro: el código se recupera de git.

## Diferencias contra la versión R69t que se recupera

1. R69t clonaba el recurso TAL CUAL (desc idéntica: formato crudo R32G32_SINT
   + flag DSV) y sacaba plane 0 como R32_FLOAT al buffer. El feeder HOY
   demostró que el runtime acepta bien un plano R32_FLOAT puro con el canal
   par. El plane-0 de un R32G32_SINT ES el canal par — misma cosa.
2. La tubería guideD ya no transporta el footprint del color: tiene su
   PROPIO pitch (PitchFor(guideW,4)) desde siempre en el arm del host.
3. EvalArgs perdió srcDepth/depthBase en la limpieza R82f — hay que
   re-añadirlos (offload_session.h) y volver a leerlos en
   OfferFrameToSession (nvngx_host.cpp:375 existed pre-R82f: GetParamResource
   NVSDK_NGX_Parameter_Depth "DLSSD.Depth" + subrect base X/Y).
4. RecordDepthCapture actual (zeros) queda como FALLBACK cuando el juego no
   trae depth (menus/cinemáticas) — pero coherente: pasa a llenar con 1.0
   (near) para casar con depthInverted=1 (worklog 36 hallazgo de
   inconsistencia). Modo knob: nrDepthMode (0=real-si-hay, 1=siempre
   proxy) — default 0 ESTRUCTURAL tras validar (regla user: lo necesario
   estructural, no opcional; pero primera ronda live = A/B).

## Pasos

1. abi.h: Tuning.reserved reutilizado → nrDepthMode uint @ (hueco tras
   forceReset: offset 20 está forceReset, 24 nrParamSeq... buscar hueco
   REAL en el layout actual con read_file completo + static_assert).
   Simplificación: reutilizar hueco id ctl 2 (reserved) → CTL_FIELD_NR_DEPTHMODE=2.
   Ojo regla: "IDs ctl append-only, huecos NO se reutilizan" — NO. Usar id
   42 nuevo + mirror[43].
2. offload_session.h: EvalArgs + srcDepth/depthBaseX/depthBaseY +
   depthDesc-vía-GetDesc-per-frame (sin Arm desc: leer desc cada frame como
   hace 2066 pre-R82f — el arm ya guarda hasDepth implícito por srcDepth!=null).
3. nvngx_host.cpp OfferFrameToSession: leer Depth resource + subrects (líneas
   pre-R82f 375-376, 433-435 verbatim).
4. offload_session.cpp:
   a. Arm: depthDesc = a.srcDepth->GetDesc() cuando llegue (guía dims YA se
      arman por MV; depth usa mismas guideW/H).
   b. Session: depthClone ComPtr + creación en DoArm si hasDepth (verbatim
      R69t 1628-1643) + Reset en DoDrain.
   c. RecordDepthCapture: versión R69t verbatim (DW→COPY_SOURCE→
      CopyResource→plane0→guideD[N%3]) con guard srcDepth==nullptr →
      fallback zeros→1.0 plano (buffer fill una vez, no cada frame).
   d. Knob nrDepthMode: 0 = real (si srcDepth llega; sino fallback), 1 =
      proxy 1.0 siempre (A/B + rollback). Host: ini + ctl case + mirror +
      panel fila + textos honestos.
5. Engine: NADA (la textura guide ya existe; solo cambia el CONTENIDO del
   buffer compartido). depthInverted ya viaja correcto (bit 3 create).
6. Build + ctest + commit + deploy (kill juego primero — está corriendo).
7. Validación live: arranque → menú (fallback activo por sin depth) →
   partida con cámara en movimiento → A/B panel Depth mode 0↔1 con user:
   calidad (LEY 2, split view + capturas ×2) + cronómetro DELTA + produce/s.
8. Worklog + wiki update.

## Criterios de éxito
- Log host: "depth guide = REAL (clone)" en partida (y proxy en menús).
- Sin degrades/watchdogs nuevo; engine vivo.
- A/B calidad con user DELANTE (LEY 2): real ≥ proxy, o revert documentado.
- Cronómetro: medir si evaluate abarata (reprojection vs recompute).

## Riesgos
- R1: el juego NO expone Depth en el bloque evaluate (solo MV) → todo esto
  es no-op; se detecta en el primer boot (log "srcDepth null → proxy").
  Pre-R82f lo exponía (el clone existía), así que esperado: sí llega.
- R2: dispatch death vivo pese al feeder (contenido dinámico ≠ replay) →
  revert knob a 1 (panel), forense después.
- R3: el clone DW→COPY_SOURCE en el list del juego perturba al render del
  juego (R69t lo midió seguro — era el diseño) — vigilar fps nativo.

---

## EJECUCIÓN LIVE (21:2x-21:3x) — REAL DEPTH FLUYENDO EN PARTIDA

- Deploy f5e8aa3b (lazy clone) tras fix R1 (arm one-shot sin depth en el
  título → clone perezoso al primer sello con DSV real).
- Boot 21:22:49, partida: "depth clone ok (1707x720 fmt 19 = D32) - REAL
  depth guide live" 21:31:23. 0 degrades nuevos (1 benigno de shutdown),
  0 watchdogs, composes 3600/3832 frames = 94% NR-presented (LEY 1).
- DELTA 54-69 ms — SIN premio de velocidad (hipótesis secundaria refutada
  con elegancia: el coste del modelo no depende del guide).
- User en partida: "no veo nada raro" — mitad del veredicto LEY 2 (no
  rompe). A/B live depth real vs proxy pendiente del user con cámara en
  movimiento (toggle panel "Depth source", live sin rebuild).
- Banner de arm corregido (decía "poisons the runtime" — mentira
  histórica); ahora imprime el modo real.
- Estado: golpe 2 ESTRUCTURALMENTE cerrado si el A/B confirma; el camino
  de todas las referencias (depth del juego tal cual) restaurado.

## VEREDICTO USER (21:4x) — GOLPE 2 CERRADO

- A/B en partida con cámara en movimiento: "no percibo mucha diferencia
  visual en la imagen final, pero en la vista NR sí es más apreciable" →
  pregunta dirigida: ¿cuál se ve mejor? → **REAL (sin marcar)**.
- Composición del veredicto: (1) calidad final igual-o-mejor (diferencia
  sutil), (2) salida del modelo visiblemente distinta = el depth se
  consume de verdad, (3) REAL gana en la vista NR, (4) coste neutro
  (54-69 ms igual), (5) 0 degrades, 94% NR-presented.
- **DECISIÓN: depthMode 0 (real) queda como ESTRUCTURA PERMANENTE** —
  paridad con todas las referencias, la vía por defecto del código.
  El knob Depth source (checkbox) se queda documentado como A/B/rollback.
- Golpe 2 CERRADO. R82f enterrado con evidencia triple (feeder refutado,
  live estable, calidad user-verificada).
