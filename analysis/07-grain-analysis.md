# Análisis GRANO en escenas oscuras — síntoma, descomposición y diseño para new-sli

Fecha: 2026-09-28 noche (sesión live RDR2, PID 1648, engine 25944, Colter/noche + interior armería). Regla de oro aplicada: fork leído (`dlssnr_residual.hlsl` v2 completo) antes de teorizar; A/B en vivo con verificación de cadena por diag.

## 1. Síntoma

"En las escenas oscuras tenemos un granulado muy presente" (user). Medición: hp-std (high-pass 3×3 sobre luma) en máscara oscura (luma<20).

## 2. Descomposición medida (A/B en vivo, cadena verificada viva por `delta diag done=7480 last=7479 engRes=1 nrOn=1`)

| Brazo | hp-std oscuro | Aporte |
|---|---|---|
| Nativo (offload OFF, DLSS del juego) | **5.51** | BASE: grano de película del juego (animado: diff 20.6/255 entre 2 capturas a 2 s — es grano temporal, no estático) + reconstrucción DLSS a render res triturada |
| Cadena nuestra, nrOn=0 (as-found) | 5.29–6.56 | ≈ nativo: el grano dominante NO es nuestro delta |
| pre-SR NR, boost 1.0 | 6.13 | **+0.02 vs boost 0** — a boost 1.0 el aporte del delta es despreciable en esta escena |
| pre-SR NR, boost 6.0 | 6.64 | **+0.51 vs boost 1** — el delta amplificado SÍ genera grano, escala con boost |
| R70d (histórico, noche real): post-SR delta CRUDO | 11.6 | delta K sin acumular aplicado sobre output N = puro ruido |
| R70d: pre-SR | 6.5 | +0.6 sobre nativo |
| R70d: nativo | 5.9 | coincide con hoy (5.5) |

**Conclusión A/B**: el granulado "muy presente" = **~90% base del juego** (film grain animado + DLSS a baja res, presentes también en nativo) **+ nuestro delta pre-SR aplicado CRUDO** (aporte +0.0–0.6 a boost 1.0, crece lineal con boost: +0.51 a 6×, ~+1.5 est. a 16×).

## 3. Causas raíz en nuestro código (verificadas línea a línea)

1. **El camino presentado (pre-SR `CSDecode`, nr_codec.hlsl) aplica el delta RAW cada frame: `v = orig + d*boost`** — SIN acumulación temporal. El ruido incorrelado frame-a-frame del delta NO promedia a cero: se imprime tal cual en la entrada del SR.
2. **El acumulador fork-exact SÍ existe pero solo en el camino muerto**: `nr_compose.hlsl` (post-SR) tiene `hist = lerp(reproject(hist_{t-1}), delta, cBlend=0.08)` con reproyección MV y ping-pong — pero R73c probó que el post-SR nunca se presenta en RDR2. El diseño fork quedó a medias: acumulación en el camino invisible, crudeza en el visible.
3. **Carrier BGRA8 del delta**: el delta viaja en display-domain cuantizado a 8 bits (paso 1/255) → dithering por frame que en oscuros pesa más (delta visible ∝ 1/luma, regla R70d: "A/B de día NO valida noche").
4. **El boost amplifica señal Y ruido por igual** (medido: +0.51 de 1→6): diseñado para A/B, no para calidad — a 16× el grano propio sería claramente visible.

El fork lo dice en la cabecera de su shader (leído completo): *"The per-frame ray-trace noise term of (edited − original) is temporally uncorrelated and averages to zero; the enhancement term follows geometry and persists"* — la acumulación MV-reproyectada con blend 0.08 es el mecanismo que separa ruido de mejora.

## 4. Por qué "en oscuros"

- Film grain del juego: enmascarado por luma, más visible sobre fondo oscuro (y siempre animado).
- DLSS a render triturado (DRS baja en noche por coste de luz): historia temporal más ruidosa en zonas oscuras.
- Delta ∝ 1/luma: la misma corrección absoluta es proporcionalmente mayor en un fondo a luma 10 que a 110.
- Carrier 8-bit: tras gamma 1/2.2 los oscuros ocupan MÁS código digital (0–0.25 del carrier) pero el paso 1/255 relativo a la señal del delta es mayor; el guard de saturación (enc≥0.985) no protege los oscuros (protege altas luces).

## 5. Implicaciones de diseño para new-sli (se cablean al plan maestro D1/D2)

1. **El acumulador temporal va DONDE se aplica el delta** — no es opcional ni del camino muerto: `apply = base + acumular(delta)` con reproyección MV + blend 0.08 + invalidación (off-screen/|mv|≥2 → hist=0, rebuild progresivo). Es exactamente lo que hace el fork en su camino vivo.
2. **Boost = knob de verificación A/B solamente** (default 1.0, documentado que >1 amplifica grano linealmente — no es un control de calidad).
3. **Delta-tint para verificación** (ya en el plan): el tinte también debe leer el hist ACUMULADO, no el delta crudo.
4. **Con D1 (pre-SR eliminado, delta-only)** el grano propio desaparece de RDR2 por construcción: la pantalla vuelve al baseline nativo (5.5) — el grano restante es del juego, y el NR futuro (juego presentador o vía in-SR residual) nace ya con acumulador.
5. Código muerto detectado hoy: `host_texdelta.rgba16f` del game dir está STALE (saturado a 512 = badge de test viejo) — borrar, no usar como evidencia.
6. Nota de estado: ini del user dice `nrOn=1` pero el runtime llevaba `nrOn=0` al llegar (preexistente a mis pruebas) — el panel muestra "aplicado" pero F8/toggle lo bajó en algún momento; el A/B de hoy se hizo con estado verificado por diag, no por ini.

## 6. Evidencia cruda

- Capturas: `grain_asfound.png`, `grain_A2.png` (animación), `grain_B_native.png` (control), `grain2_b{0,1,6}_*.png` (sweep) en scratch.
- Diag de cadena vivo: `done=7480 last=7479 engRes=1 nrOn=1` (22:12).
- Primer sweep INVÁLIDO documentado: mi `nrOn=1` se envió antes de que el re-arm (por cambio de nrStage vía ctl) terminara → el re-arm lo pisó a 0 (trampa R55 resucitada); repetido con orden correcto + verificación por diag.
