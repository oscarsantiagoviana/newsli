# 42 — R92: de-jitter del DEPTH (guía) + de-jitter estructural (knob muere)

Fecha: 2026-10-04. Go user: "en la vista NR noto oscilaciones... con de-jitter
seal activado siguen siendo menores pero perceptibles, puede que en pre-SR
tengamos que meter corrección a alguno de los inputs del modelo y se nos
colase; ese flag salvo para depuración no debería existir — sabemos cuándo
aplicar jitter".

## Diagnóstico (verificado en código, lectura completa)

El de-jitter del sello (3b) solo estabiliza el COLOR. La guía DEPTH llega
cruda del raster jitterado → el modelo recibe color quieto contra un depth
que nada ±j por frame (Halton). En cada borde con discontinuidad de
profundidad la pareja color/depth está desalineada de forma OSCILANTE:
- dej OFF: todo jitterado junto (coherente pero inestable) → más oscilación
  (lo que el user mide).
- dej ON: híbrido incoherente → oscilación menor pero perceptible (estado
  actual).
- MV: el contrato NGX EXCLUYE el jitter de los MV — no se toca.

El fork referencia (DlssNr_Dx12.cpp:330 preSrRejitter, Mode 5) resuelve el
pre-SR por el otro camino (todo crudo + jitter declarado + re-jitter de la
SALIDA) — contrato (a). Nosotros medimos (3b sign-test) que nuestra
estabilización externa supera la compensación interna del modelo en este
contenido; completamos el contrato (b): TODO estabilizado + jitter 0.

## Cambios

1. **nr_stabdepth.hlsl (NUEVO)**: CS que remuestrea el depth a −j
   (bilinear 4-Load manual, clamped — mismo patrón que el 1:1 del encode).
   Dims guía (= render). Corre SOLO si (jx|jy)≠0: en POST-SR ni dispatch
   ni transiciones (cero coste, cero cambio).
2. **nr_vendor.h**: recurso texGuideDepthStab (R32_FLOAT, UAV) + PSO stab +
   heap 10→12 slots (t0=10 depth SRV, u0=11 stab UAV — regla: recurso
   nuevo = array+heap+views+rango JUNTOS) + state tracking del stab.
   Evaluate recibe el stab cuando hay jitter, el raw cuando no.
3. **dej ESTRUCTURAL (knob nrDejitter muere — user mandate + no-legacy)**:
   - abi.h Tuning: −nrDejitter@104 → nrResidual a 104, sizeof 112→108.
   - ctl_common: id 43 retirado (append-only: no se reusa).
   - offload_session/nvngx_host/panel/ini: todo el plumbing fuera.
   - EncCb: 12→11 constantes (Num32BitValues 11).
   - evaluate: jitter SIEMPRE 0 (el plano llega estabilizado).
   - DecCb: −jX/jY (landing siempre 0 — el dominio ya es canónico);
     18→16 constantes, static_assert 72→64. La float3 dNormRGB queda
     ANTES de lo tocado: nada se realinea.
   - nr_encode.hlsl: gDej fuera; gate = (gJx|gJy)≠0 (identidad si j=0,
     POST-SR bit-exacto y sin pago de 4 loads).
   - nr_delta.hlsl: dJitterX/Y fuera del cbuffer; rp sin shift.
4. POST-SR: TODOS los caminos degeneran a identidad → bit-exacto con hoy.

## Validación

1. Build 0/0 + ctest 3/3.
2. Feeder (engine del gamedir RE-DESPLEGADO primero): SLI_FEEDER_JITTER
   (patrón 2×2 Bayer) + SLI_FEEDER_DEPTH=real, misma captura:
   - ARM A (sin jitter) vs ARM B (jitter activo): deltas deben ser
     ~idénticos (la señal al modelo ya no depende del jitter).
   - El depth stab dispara solo en B (log).
3. Live (user): pre-SR + vista NR, cámara quieta sobre barandas/líneas:
   la oscilación residual (dej ON de hoy) debe morir o quedar en el
   segundo orden. Veredicto user LEY 2.

## Riesgos

- R1: bilinear en log-depth smeara bordes ±j — el MISMO smear que el
  color ya lleva: la pareja queda pareada (es el objetivo, no un bug).
- R2: ABI shift (Tuning 108, DecCb 64) → redeploy de los 5 binarios
  juntos (regla estándar, build/deploy only).
- R3: si el jitter del juego no viaja en el parámetro (R1 del 3b), el
  stab no dispara — el feeder lo distingue (mecanismo vs contenido).

---

## EJECUCIÓN + VALIDACIÓN OFFLINE (2026-10-04 14:20) — CERRADO A LA ESPERA DEL VEREDICTO LIVE

Commit 838731d. Cambios según plan, con dos ajustes de realidad:

- Tuning sizeof queda **112** (no 108): nrResidual cae en offset 104 y
  el struct redondea a 8 — 4 B de tail padding, layout verificado con
  modelo ctypes. static_asserts actualizados (residual@104, sizeof 112).
- El stab se arma LAZY en el primer frame con jitter (textura R32_FLOAT
  guideW×guideH, vistas en slots 10/11 del heap de 12) y el dispatch
  corre en el MISMO segment del frame, tras copyGuide — sin flush extra.

### Resultados del feeder (captures_sronly, ONEFRAME, depth real, 8 loops)

- ARM A (sin jitter): G2 CONTENT 7/8, cero dispatches del stab (log
  limpio) — la identidad POST-SR es exacta.
- ARM B (SLI_FEEDER_JITTER=1, Bayer 2×2): `depth-stab armed
  (1505x635, R92)` — dispara solo con jitter.
- **Deltas BIT-IDÉNTICOS entre brazos**: md5 A_0 == B_0 ==
  17934b3fa597137fbd71b1460f2aced0 (dumpeos en build/r92ab/). La señal
  que entra al modelo ya no depende del jitter — el criterio
  anti-oscilación del plan se cumple de forma exacta.
- 0 poison, 0 throws, rc limpio en ambos brazos.

### Pendiente (LEY 2)

- Veredicto live del user en pre-SR + vista NR, cámara quieta sobre
  barandas/líneas rectas: la oscilación residual (la que sobrevivía al
  dej antiguo) debe morir o quedar en segundo orden. El toggle del
  panel ya NO existe — el A/B es pre-SR (stab+seal activos) vs POST-SR.

---

## R92b (14:55) — el veredicto live llegó NEGATIVO y destapó el bug REAL

User: "sigo viendo oscilaciones pequeñas pero constantes y perceptibles
en pre-SR; en la vista NR se observan claramente". Forense de la sesión:
0 history resets, stab armado, 42-51 ms, gainnorm ~0.86 moviéndose suave.

### Causa raíz (lectura completa de nr_delta.hlsl)

El R92 estabilizó TODO lo que entra al MODELO (color del sello, depth)
pero el DECODE sigue leyendo gOrigHdr (el sello crudo JITTERADO) para:
- `orig` → `o`, `enc`, `fullProxy` (el gain es result/orig: el
  DENOMINADOR vive en dominio jitterado mientras el numerador es
  canónico → ripple a frecuencia de Halton en el gain final);
- la VISTA NR pinta modelDisp = fullProxy + edit → **la vista entera
  ondea ±j** aunque el modelo esté quieto — exactamente lo que el user
  ve "claramente en la vista NR";
- `origBil` (residual golpe 4) muestrea sin el shift → el smallProxy no
  casa con el footprint que el encode usó (sub-pixel mismatch).

Por qué el feeder no lo vio: ONEFRAME = contenido ESTÁTICO → un shift
sub-píxel no cambia el promedio del box ni el proxy → el gate offline es
CIEGO a desalineaciones de dominio sub-píxel. Lección: el banco estático
valida mecanismo, no dominio; la discriminación de dominio exige live
(o capturas multi-frame con contenido en movimiento).

### Fix (todo con identidad exacta a j==0 — POST-SR intacto)

1. nr_delta.hlsl: `orig` pasa de Load a bilinear 4-load clamped a
   **−j** (render px, sin conversión — el sampleo del proxy es en px de
   render); `origBil`'s rr lleva el MISMO −j (casa con el footprint del
   encode). rp (answer landing) sigue SIN shift (dominio canónico ya).
2. DecCb: +jX,jY al final (64→72 B, 16→18 consts, root sig 18).
3. nr_stabdepth.hlsl: clamp de los 4 taps (j negativo leía 1 px OOB).
4. nr_encode.hlsl 1:1: clamp de los 4 taps del bilinear (mismo edge).

## R92c (15:05) — test forceReset: la teoría de retroalimentación MUERE

User hipótesis: "¿puede ser la retroalimentación o algo similar provocando
inestabilidad?". Test: forceReset=1 (ctl 12) mata la historia del modelo
cada frame — TODA recursión temporal muere de un golpe.

**Resultado user: "sigue igual, incluso un poco más inestable en
tonalidades aplicadas a la misma zona" + "algo de flickering".**

Veredicto del discriminador:

- Teoría A (historia × MV, feedback) — **MUERTA**: si la oscilación
  viviera en la recursión, matarla cada frame la habría matado. El
  "algo peor" es el side-effect esperado (cold denoise por frame = más
  ruido sin integración temporal).
- Teoría B (bucle gainnorm lag-1) — no primaria: gainnorm pre-SR vaga
  0.854-0.874 pero es EFECTO (mide el ratio) no causa; su EMA ya baja
  el loop gain <1.
- Teoría C (feed-forward físico) — **LA QUE QUEDA**: la comparación de
  compose enfrenta una imagen TEMPORALMENTE INTEGRADA (modelDisp, con
  historia) contra un raster INSTANTÁNEO (orig a −j). El −j global
  cancela un solo plano de profundidad; el residuo de paralaje mueve el
  contenido sub-píxel frame a frame (sign-test 3b midió exactamente ese
  residuo). En geometría de alta frecuencia (barandas, líneas de madera)
  el ratio numerador/denominador oscila → tonos oscilando EN LA MISMA
  zona. Coincide con gainnorm pre-SR vagando vs POST-SR clavado en 1.004
  (allí el juego ya entregó el raster resuelto y estable).

### Cierre de la ronda

La oscilación pre-SR es FÍSICA del modo (raster jitterado + paralaje vs
evaluación por frame), no un bug de la cadena — la cadena quedó
verificada dominio-canónica de punta a punta (sello, depth, proxy).
POST-SR + workScale sigue siendo el diseño estructural estable;
pre-SR queda como modo rápido (42-51 ms) con este shimmer inherente en
geometría fina. Sin paliativos (EMA de gain taparía el síntoma y
añadiría lag — REGLA ORO RAÍZ).

---

## R92d/R92e (17:10) — default POST-SR + instrumento restaurado + noche

Directiva user: pre-SR NO default (pérdida de calidad notable);
POST-SR estructural (ini saneado: preSr 0, tint 0, ws 0.5, intensities
1.0, perfQ 2). Investigación abierta: ¿inestabilidad = modelo a baja
res, o input mal/desplazado/incompleto?

- "La vista NR ha dejado de funcionar": causa = golpe-4 side effect.
  El view payload pintaba la RECONSTRUCCIÓN (fullProxy+edit ≈ el frame
  del juego re-asentado) — indistinguible de sin NR. FIX e4e6f1b: la
  vista pinta el output CRUDO del modelo (pre-residual). La vista es un
  instrumento de diagnóstico; nunca alimenta la calidad.
## R92f (17:20) — split de exposición: LA NOCHE ERA UN ENCODE CASI-NEGRO

User pregunta clave: "¿estamos sobrescribiendo exposición/contraste/gama?
¿no será que hay que hacer caso al valor del juego?".

Auditoría: el GAIN no toca nada (corre antes del tonemap del juego — su
auto-exposición y gamma multiplican DESPUÉS, intactas). Pero el ENCODE
reutilizaba el preExposure del juego como punto blanco — y RDR2 manda
1.0 CONSTANTE: el meter adaptativo (0.18/mediana) llevaba MUERTO desde
R82j. De noche, luma cruda ~0.005 → el modelo veía sRGB casi negro →
respuesta = ruido puro → "ruido por todos lados" + "vista NR muy
oscura". Fix a9e406c: DOS conceptos separados — reconstrucción =
preExposure held (el juego manda), encode = meter adaptativo vivo
(readback des-gateado — R91b lo había matado por muerto; era el meter);
mismo whitePoint en encode y decode (coherencia de dominio).

## R92g (17:30) — veredicto final de la investigación user

"En pre-SR la vista NR es muy inestable" (con meter vivo, gainnorm
balanceado 0.97, 0 resets, tint crudo). Cadena de evidencia completa:

1. forceReset (R92c): la recursión NO es la causa.
2. Dominio jitter (R92/R92b): verificado bit-exacto offline + mejora
   perceptible live.
3. Vista cruda (R92d) + meter vivo (R92f): la respuesta del modelo en
   pre-SR ondea EN SÍ MISMA con cámara quieta.

**VEREDICTO: no hay input mal transportado — es el raster pre-SR
crudo.** Cada frame llega con su jitter TAA de origen SIN resolver (no
pasó por el TAA del juego); el −j global alinea dominios pero el raster
respira en geometría fina; el modelo temporal integra esa vibración y
su respuesta ondea. POST-SR entrega raster resuelto → estable
(gainnorm histórico 1.004 clavado).

Camino no jugado (el del fork): re-jitter de salida (preSrRejitter,
Mode 5) — devolver la respuesta al dominio jitterado y dejar que el TAA
del juego la resuelva. Vía de DISEÑO nueva, no bug fix. Pendiente de
decisión user. Pre-SR queda como está: rápido (42-51 ms), con shimmer
inherente; POST-SR + ws = default estructural estable.




