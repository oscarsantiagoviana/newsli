# 38 — Golpe 3: PRE-SR input (nrPreSr=1) — A/B de coste y calidad

Fecha: 2026-10-03 (noche). Go user (tras descartar skip-to-newest por ahora).

## Contexto

- POST-SR (actual): el sello copia el OUTPUT del SR (display-res 2560×1080,
  de-jitterizado) tras el evaluate nativo. Modelo a 2.76 Mpx.
- PRE-SR (opt-in, ctl 30, checkbox panel desde R89c): sello del color de
  entrada al SR (render-res 1707×720, jitterado). Modelo a 1.23 Mpx →
  **~2.25× más barato**; transporte color+gain 42→18.9 MB.
- Historia del veto (R82h): "el origen del vaivén era alimentar el modelo
  con el frame PRE-SR jitterado" → POST-SR por defecto. PERO la cadena
  ha cambiado desde entonces:
  - R82i.5: jitter units audit — DecCb convierte a unidades WORK.
  - R82k.5: jitter scaling restaurado + veredicto "fork parity literal".
  - El decode ya compensa el jitter del input PRE-SR (nr_delta.hlsl
    dModelTop/jitScale path).
  - wiki 28 addendum YA recomendaba: "re-abrir el A/B con la escena actual".
- A/B histórico in-game (R82h, ws 0.5): post 11 fps vs pre 19.7 fps
  (delta engine 56-73 ms). El coste es real y grande.

## El experimento (no-code: knobs existentes)

1. Confirmar verificación base: juego vivo con POST-SR (fps overlay +
   cronómetro engine DELTA + produce/s + composes/s) — 2 min de partida.
2. Panel: checkbox "Pre-SR input" ON → Apply (live, re-arm al cambiar
   placement — el sello cambia de fuente y dims; verificar en log
   "arming 1707x720" (render dims) vs "2560x1080").
3. Misma escena 2 min: fps overlay + DELTA + produce/s + composes/s.
4. Calidad (LEY 2, user delante): vista NR con cámara QUIETA (diff ~0 =
   sin vaivén) y en MOVIMIENTO (detalle pegado, sin ghosting). El jitter
   del input PRE-SR es compensado por el decode; cualquier vaivén
   residual = revert.
5. Veredicto: si calidad OK y DELTA cae ~2×: PRE-SR pasa a default
   estructural (ini nrPreSr=1) + panel texto actualizado. Si no: revert
   y queda opt-in documentado.

## Notas técnicas
- El knob es LIVE (host-side, re-arm al cambiar: placement cambia dims
  del transporte).
- depthInverted/guías no cambian (siempre render-res).
- El gate/watchdog no cambian.
- Riesgo R1: vaivén residual en movimiento → revert (checkbox), sin
  código dañado.
- Riesgo R2: compat con depth real (golpe 2, cerrado hoy): independiente
  (depth viaja por su anillo a render-res en ambos modos).

---

## EJECUCIÓN + VEREDICTO (00:2x-00:38) — OSCILACIÓN CONFIRMADA, CAUSA AISLADA

- Secuencia del A/B (todo por panel, sin código):
  1. POST-SR + ws1.0 (Apply previo del user): DELTA 130-169 ms, base.
  2. PRE-SR + ws0.25: work 427×180, **DELTA 34-43 ms (~4×)**, motor 17.1/s,
     0 degrades — el coste prometido ES real.
  3. PRE-SR + ws1.0 (user subió para quitar la máscara de ruido): work
     1707×720, DELTA 64-83 ms — **oscilación visible con cámara quieta**.
  4. + History reset shift 50 (matrícula del test): resets por flow a 0,
     historial del modelo descartándose — **la oscilación persiste** →
     el modelo y su reprojection temporal QUEDAN EXCULPADOS.
  5. POST-SR de vuelta (30=0): **la oscilación muere** → causa = la cadena
     de sellado pre-SR.
- DIAGNÓSTICO: jitter residual del raster pre-SR. El juego jittera su
  render ±medio píxel por frame (DLSS-style); el sello captura ese frame
  temblón a render-res y el decode compensa con el jitter DECLARADO —
  queda una desviación sub-píxel residual (aplicado vs declarado, o
  redondeo entero en la fuente) que se ve como temblor fino. R82h tenía
  razón en el síntoma; la causa exacta ahora está acotada al sello, con
  el modelo exculpado por primera vez.
- VEREDICTO golpe 3: **CERRADO SIN ADOPTAR** — pre-SR queda opt-in
  documentado (coste demostrado ~4×; bloqueado por calidad). LEY 2 manda.
- NEXT (golpe 3b, propuesto): DE-JITTER EN EL SELLO — muestrear el color
  capturado con el offset de jitter del frame en el encode (GPU, una
  constante más en el shader), entregando al modelo un frame estable a
  render-res sin esperar al SR. Rescataría los 34-43 ms con la estabilidad
  del POST-SR. Diseño pequeño, verificable con feeder (misma captura,
  jitter simulado) antes del juego.
