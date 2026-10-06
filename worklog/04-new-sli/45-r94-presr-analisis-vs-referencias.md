# R94 — Análisis pre-SR: frames presentados sin SU NR (vs referencias)

User: "no me gusta eso de que presentemos frames a los que no hemos pasado NR".
Directiva: mirar detalladamente lo nuestro y lo de los proyectos de referencia.

## Lo que hacen las referencias (leído completo, no de memoria)

### OptiScaler-DLSSNR-PreSR-Multipass (DlssNr_DeferredSr.inl, 739 L)
- TODO vive en la MISMA command list del juego, mismo frame, misma GPU:
  Before() → copia color a `edited` → modelo NR in-list → encode residual
  (baja res) → arma `pending`. After() (tras el SR del juego, misma lista) →
  evaluate de un DLSS PRIVADO que upscalea el residual a display res →
  compose `clean(copia del output del SR del juego) + residual` → copy de
  vuelta al output del juego.
- El veredicto aplicado es SIEMPRE el DEL MISMO FRAME: no hay age. El
  "deferred" del nombre = el residual se aplica después del SR, no en otro
  frame. Cero round-trip cross-GPU: el modelo cuesta ms in-list.
- Su modo half-rate (NR cada 2 frames + FG residual) ES cadencia dividida —
  y lo declara en status: "NR every second frame + NVIDIA residual FG; SR
  delayed 1 frame". Su sample-and-hold: "each NR residual applied to 2
  current frames" — acepta age-1 SOLO como fallback sin motion vectors.
- Fallo → "clean SR frame retained": presenta el frame SIN NR (fail-open),
  nunca un veredicto viejo. El estado (Say) siempre dice la verdad.
- Marcadores de completado GPU por seam (Use/MarkerCount): nunca reusan un
  slot sin prueba de completado — la misma clase de garantía que nuestro
  staging por engineHostFrame (R93e/f).

### NeuRotic (DlssNr_Dx12.cpp, pre-SR)
- `EvaluateBeforeUpscale` reemplaza el color ANTES del upscaler del juego,
  in-list, mismo frame. De-jittera para el modelo (`preSrRejitter`) y
  re-jittera para el upscaler (equivalente exacto de nuestro +j en decode).
- Publicación por serie: `successfulEvaluations` / `completedPipelineEvaluations`
  — "Advances only after every requested healthy layer has composed. Pre-SR
  readiness keys on this": un frame NO se publica como NR si su pipeline no
  completó sano. Anti-flicker estructural.
- Exposición: lección del meter (Cyberpunk: exposure = pixel rojo del frame,
  moviéndose 272×, white point 0.18→74 = flash de luminancia entera) —
  holdings por slot, no por frame.

## Lo nuestro (golpe 5 deferred, desplegado)
- Cada evaluate: seal → compose color(N) × gain(freshest done) in-list →
  swap Colour → SR del juego consume texNr. Cobertura 100% (todos los frames
  componen), pero el gain tiene age 4-5 frames (~75 ms) porque el veredicto
  vive en GPU2: DELTA 60-73 ms + cola de transporte.
- El ciclo estructural (probado en R93 con la coreografía de fences): el
  seal del frame N se graba EN la lista del evaluate N → el veredicto de N
  no puede existir antes de que esa lista ejecute → una espera GPU en la
  MISMA lista por el veredicto de N = deadlock. El compose same-frame-own-
  verdict con offload cross-GPU es IMPOSIBLE sin serializar la cola del
  juego contra GPU2 (fps = ritmo del engine).

## Veredicto del análisis
1. Las referencias NO presentan frames con veredicto ajeno en operación
   normal: mismo frame, in-list, misma GPU. Su fail-open (frame limpio) es
   el ÚNICO caso de frame sin NR — error, no cadencia.
2. Nuestro lag no es un bug de implementación: es el precio arquitectural
   del offload cross-GPU. El suelo alcanzable es lag-1 (gain del frame
   anterior), NO lag-0.
3. PERO nuestro age actual (4-5 frames) NO es el suelo: con el engine
   tunado (ws=0.5, pqv default) el veredicto cabe en <1 frame (en POST-SR
   con gate ilimitado a ws=0.5 el conjunto sostenía 69-85 fps ⇒ veredicto
   ~12-15 ms; el transporte pre-SR 1707×720 es 0.44× del POST-SR). Age-1
   (~16 ms) es alcanzable HOY sin tocar la estructura.
4. Menu honesto:
   a) lag-1 tight: tunear engine (ws 0.5, pqv 2) → age ~1 frame; medir age
      real en logs (timestamp seal→compose por frame); A/B paneos.
   b) GPU-wait in-list por veredicto propio = deadlock/serialización —
      descartado (R93, ciclo probado).
   c) POST-SR (default vigente): mismo-frame-por-construcción vía gate —
      el único modo que cumple LEY 1 al 100% hoy.
5. Coincidencia a adoptar de referencias (independiente del modo):
   - estado textual siempre honesto estilo Say() (ya es regla de oro nuestra);
   - fail-open = frame limpio, nunca gain viejo tras fallo (nuestro neutral
     ya lo hace en zero-delta; auditar el caso "engine cae" → neutral, no
     último gain);
   - publicación por serie sana (NeuRotic) como idea para la vista viewer.

## Próximo propuesto
R94a: tunear engine pre-SR (ws=0.5, pqv=2), medir age del gain en logs
(sello→stage por frame), validar paneos. Sin cambios de estructura.
