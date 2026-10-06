# R90 — Análisis del mecanismo de offload y formas más eficientes (2026-10-03)

Mandato user: analizar el mecanismo completo y buscar formas más eficientes de
conseguirlo, con referencias y docs. Código leído COMPLETO: offload_session.cpp
(2862), loop.cpp (657), nr_vendor.h (1249), nvngx_host.cpp (777). Fichas:
DLSSNR-Cost-Scaler, NeuRotic, neural-coprocessor, new-sli. Docs: MS Learn
shared-heaps / multi-adapter / waitable swapchain. Números verificados por
cálculo (ver chat).

## 1. Dónde está realmente el tiempo (datos duros R89d + cálculo)

- Motor 113-130 ms/delta POST-SR (2560×1080 = 2,76 Mpx). El modelo vendor a
  render res media 38-53 ms @ 1,23 Mpx → escala 2,25× = 85-119 ms: el delta
  actual ES cómputo del modelo, no transporte ni copies.
- produce 5,4/s, compose 4,4-5,2/s, GPU del juego ~8%/26 W: el juego NO está
  GPU-bound; está estrangulado por el gate same-frame (Present espera el delta
  → el bucle del juego corre a velocidad del motor → 4-8 fps).
- Transporte: 56,4 MB escritos/frame (color 21,1 + gain 21,1 + depth 4,7 +
  MV 9,5), ~113 MB de tráfico PCIe ida+vuelta ≈ 10 ms agregados. NO es el
  cuello. PERO: MS Learn shared-heaps confirma que un heap SHARED_CROSS_ADAPTER
  vive en D3D12_MEMORY_POOL_L0 (sistema) aunque se cree DEFAULT → cada frame
  cruza PCIe dos veces por dirección. Correcto como diseño, medible como coste.
- Compose en Present: draw fullscreen + flush 2 s-bound en cola propia —
  trivial (<1 ms) frente al delta.

Conclusión: la latencia del offload = cómputo del modelo vendor a resolución
display dentro de una ley same-frame que bloquea el Present. Todo lo demás es
ruido secundario.

## 2. Palancas que YA existen (sin código nuevo, solo A/B)

1. `nrPreSr=1` (R82h, opt-in): sella color a RENDER res (1,23 Mpx) → modelo
   ~2,25× más barato (≈50-60 ms) + transporte color/gain 42→18,9 MB. Coste:
   input jittered (el decode ya compensa jitter, DecCb jX/jY). A/B calidad
   pendiente — era default hasta R82h por el veredicto de jitter, no por coste.
2. `nrWorkScale` (0,25-2,0): 0,5 = 0,69 Mpx → base del modelo 10-13 ms.
   BLOQUEADO por calidad: sin matched-residual el downsample lee blur como
   headroom (divergence #1). Ver §3.1.
3. `nrGateWaitMs` (R88): válvula, no cura — con motor a 113 ms TODO frame
   llega tarde → todo nativo → NR invisible. Útil solo cuando el motor entra
   bajo el tiempo de frame.
4. `PerfQualityValue` 2.0 (create-time, nr_vendor.h:731): bajar a 0/1 es un
   A/B de una línea de ctl. Sin medir; el coste del vendor es geométrico
   (ficha Cost-Scaler), el efecto esperado es pequeño.

## 3. Cambios pequeños con receta de referencia (orden de ejecución propuesto)

1. **Matched-residual (el "THEY-BETTER #1" nunca portado)** — verificado:
   `residual` = 0 hits en src/ y shaders/ (solo docs). Receta completa en el
   fork (Dagherbou/NeuRotic) y ficha Cost-Scaler: `edit = smallOut − smallIn`
   en dominio display, componer edit, NO la imagen. Desbloquea ws=0,5 →
   motor ~15-25 ms → el gate deja de mandar y el juego vuelve a su cadencia
   nativa. Verificable OFFLINE con el feeder antes de tocar el juego (regla).
2. **Un solo FlushSeg por frame**: hoy hay 3 sincronías CPU-GPU por delta
   ("in" / "vendor-frame" / "out"); la copia de seal puede caber en la MISMA
   lista del ProcessFrame (barrier COMMON→SRV dentro). Ahorra 2 round-trips
   de sync por frame (~5-15 ms según cola).
3. **Output ring 2-3 slots avanzado por fence** (wiki22 §2.1, patrón UNCANNY):
   hoy `out` es UN buffer — el motor no puede empezar N+1 hasta que el gate
   terminó de stagear N. Ring = desacopla producción y consumo.
4. **Skip-to-newest** (ficha neural-coprocessor: sin él, 83% del trabajo
   neural descartado en su rig): con ring, el motor salta a frame+reciente en
   vez de envejecer la cola. Complementa el gate-wait.
5. **Carrier BGRA8 sellado en GPU0** (opcional, refactor mayor): el encode
   HDR→BGRA8 podría correr en la seal list (GPU0) y transportar 4 B/px en vez
   de 8 (−10,5 MB/frame; el modelo come BGRA8 de todos modos). Requiere mover
   exposure/tiles — solo si §3.1-3.4 no alcanzan.

## 4. Lo que NO tocar (veredictos cerrados)

- Texturas compartidas cross-adapter: el driver las rechaza (medido, comment
  offload_session:167); buffers placed sobre heap compartido = forma válida.
- Barreras sobre texturas del juego dentro de SU lista (R69q), fences por
  nombre en vez de handles duplicados (R85d), same-frame mixing (R79: ley).
- El wedge F6 sigue abierto (sin repro post-sonda ECL) — orthogonal a esto.

## 5. Techo honesto y relación con la línea de destilación

- Con el vendor: motor óptimo realista ws=0,5+ring+1 FlushSeg ≈ 15-30 ms →
  el juego vuelve a su cadencia nativa (15-25 fps en RDR2 CPU-bound) con NR.
  El offload NO acelera el juego por encima de su nativo: solo deja de
  estrangularlo.
- Un student destilado (línea R90-anterior) que denoisee en 5-15 ms a display
  res haría el same-frame casi gratis y permitiría ws=1.0 de calidad — es el
  paso 2 del roadmap si el vendor ajustado no basta.
- Fuentes: wiki/22 §0-2 (los 4 puntos ya estaban identificados; este análisis
  los confirma con el código actual y añade el orden), fichas
  neural-coprocessor (fence/costes), DLSSNR-Cost-Scaler (matched-residual),
  NeuRotic (receta), MS Learn shared-heaps (L0).

Estado: solo análisis, cero cambios de código. Deploy intacto (e122314).
