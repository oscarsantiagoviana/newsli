# 40 — Golpe 4: matched-residual — el "THEY-BETTER #1" del fork, portado a nuestra cadena

Fecha: 2026-10-04 (madrugada). Padre: 38/39 (golpe 3+3b: POST-SR + ws 0.25
estable a ~55-72 ms). Referencias: dlssnr.hlsl:780-940 (fork), ficha
DLSSNR-Cost-Scaler §Matched Residual.

## El problema que resuelve (por qué existe esto)

Hoy, con ws 0.25, la composición compara:

- `modelDisp`: la respuesta del modelo, muestreada bilinealmente desde
  640×270 (work) hasta 2560×1080 (render), y
- `enc`: el proxy del frame reconstruido a RESOLUCIÓN COMPLETA en el
  decode (R84 B1: soft knee + sRGB aplicado al texel del HDR original).

Ese par DISCREPA por dos razones mezcladas: (a) el edit del modelo, y
(b) el BLUR del downsample+upsample (la respuesta pequeña, ampliada, es
más suave que el proxy reconstruido a full). La composición lee (b) como
"headroom que el frame tiene" → shift de color dependiente de la
resolución (la "divergence #1" del worklog 33; el fork lo midió al 50%
como corre de color). A ws 0.25 (4× de ratio) el término (b) es GRANDE.

## La receta del fork (dlssnr.hlsl:860-905)

```
edit     = model - proxySmall           // AMBOS a work dims
fullProxy = SoftKnee(original)          // reconstruido a FULL res (la
                                         // encode es función pura)
model'    = fullProxy + edit            // el edit sube; el proxy baja el
                                         // blur: las dos fotos que entra a
                                         // la composición son full-res
```

- La composición (re-anchor de luma + chroma + guard) queda IGUAL — solo
  cambia qué `model` y qué `proxy` reciben.
- "At the same rate the arithmetic collapses" (ws=1: P+(m-p)=m) → el
  fork SOLO toma el camino cuando modelRanSmall (bit-idéntico a classic
  en ws=1 — por eso puede default ON).
- Cubo-scale del residual (CubeScaleResidual, hhkbble): limita cuánto
  puede viajar el edit antes de salir del cubo [0,1] — sin él, picos
  saturan y roban rango al resto.

## NUESTRO caso: qué es cada pieza en nuestra cadena

| fork | nosotros |
|---|---|
| proxy small (encode output BGRA8) | `texColor` (slot 2, BGRA8 work) — NO viaja al decode hoy |
| model answer | `texOutModel` RGBA8 (slot 5) |
| original | `gOrigHdr` = texInColor 16F render (slot 6) |
| SoftKnee(full) | ya EXISTE en el decode: `enc` (R84 B1) es exactamente SoftKnee+sRGB del texel original — es `fullProxy` ya calculado |
| edit | modelDisp − encSmall, donde encSmall = el mismo proxy reconstruido PERO muestreado a work dims (bilinear, misma ventana del decode) |

Clave estructural nuestra: **nuestro decode YA reconstruye el proxy full
(`enc`) y YA muestrea la respuesta del modelo bilinealmente desde work
(`modelDisp`)**. Lo único que falta es **encSmall** — el mismo proxy pero
a work dims, para restar el BLUR y quedarnos con el edit puro. Y
encSmall es OTRA VEZ función pura del texel original muestreado en la
posición work (la misma bilinear del modelDisp) → **un bloque de
muestreo extra en el shader, cero transporte nuevo, cero ABI nueva.**

## El cambio (todo en nr_delta.hlsl + 1 constante)

1. `DecCb` +1 float `dResidual` (0 off / 1 on). 17→18 consts; root sig
   Num32BitValues 17→18 (¡LA TRAMPA 3b! declarar y empujar el MISMO
   número — el incidente flashes fue exactamente esto).
2. En CSDecodeDelta, tras modelDisp (post-median) y tras `enc` (full):
   ```hlsl
   float3 edit;
   if (dResidual > 0.5 && modelRanSmall) {
       // encSmall: mismo soft-knee+sRGB aplicado sobre la MISMA bilinear
       // que llenó modelDisp (los mismos 4 taps + pesos) — así el blur
       // del downsample está en AMBOS lados de la resta y se cancela.
       float3 encSmall = Encode(recon4tap);      // recon4tap = modelDisp
       // PERO modelDisp pasó por la MEDIANA 9x9 — para la resta hay que
       // usar la bilinear PURA (sin mediana): guardar preMed antes del
       // bloque de mediana.
       edit = modelDisp - encSmall;
       modelDisp = CubeScaleResidual(enc, enc + edit);
   }
   ```
   - La mediana se mantiene sobre la RESPUESTA (mata parches del modelo);
     la resta usa la bilinear pura pre-mediana para que proxy y modelo
     compartan exactamente el mismo kernel de muestreo (si no, la resta
     no cancela el blur, lo desplaza).
   - `modelRanSmall = (dModelW != dWidth || dModelH != dHeight)` — ya
     computable con las consts actuales.
   - CubeScaleResidual portado literal del fork (dlssnr.hlsl:459).
3. Default: **ON** cuando modelRanSmall (fork behavior) — knob
   `nrResidual` (0=off rollback, 1=matched-residual). Panel "Residual
   compose". Ini nrResidual=1. ctl id 44, mirror 45, Tuning tail @108
   (sizeof 112→116→120 8-align — VERIFICAR con static_assert real).

## Coste y riesgo

- Coste GPU: una codificación más (soft-knee+sRGB) por píxel render +
  la resta — trivial frente a 55 ms de evaluate.
- Riesgo R1 (ghosting): LEY 2 manda — A/B con user. El edit ampliado
  sigue pasando por TODO el pipeline de guard (re-anchor, floor, guard,
  chroma bound) — no es bypass de nada.
- Riesgo R2: la mediana 9x9 corre SOBRE modelDisp (respuesta): si el
  edit contiene stripes 1-px del modelo, la resta podría moverlas —
  la mediana sobre la respuesta las mata igual (el edit hereda la
  respuesta mediana menos proxy limpio).
- Validación OFFLINE PRIMERO (regla): feeder con la MISMA captura,
  A/B residual on/off: el gain resultante debe ser ~idéntico en ambos
  (con captura estática el edit≈0 no cambia nada — el test real es que
  NO rompe); luego live A/B de calidad con user.
- NOTA de escala: el fork mide la divergencia crecer con el ratio; a
  ws 0.25 (4×) el fix es MÁS necesario que a su 0.75 recomendado.

## Pasos

1. ABI/ctl/panel/ini (knob nrResidual).
2. nr_delta.hlsl: preMed preservado + encSmall + CubeScaleResidual +
   rama residual (todo tras la mediana).
3. Root sig decode Num32BitValues 17→18 + DecCb 18 (TRAMPA 3b: contar
   dos veces, grep cruzado push/declare).
4. Build (golpe1_build.bat, BUILD_EXIT echo) + ctest.
5. Feeder offline: residual on vs off, misma captura — gain debe ser
   estable y finito; diff numérico esperado (no bit-idéntico: la rama
   cambia el math), sin NaN/Inf, medias razonables.
6. Deploy (build/deploy/, NUNCA build/ raíz — regla 3b) + kill + md5 +
   relaunch + ini nrResidual=1.
7. A/B live user: vista NR + calidad final, cámara quieta y enmovimiento.
8. Worklog ejecución + wiki si procede.

## Éxito esperado

- ws 0.25 mantiene ~55 ms (el coste es shader-trivial).
- La corrección de color dependiente de resolución muere → posible
  subida de ws a 0.5 (~90 ms) con calidad OK si el user lo pide.
- El ghosting residual del verdict del modelo (si existía por blur) baja.

---

## EJECUCIÓN + VEREDICTO (2026-10-04) — ESTRUCTURAL, EFECTO INVISIBLE

### Implementada y validada offline (feeder, ANTES del juego)

- ABI: Tuning.nrResidual@108 (sizeof 112 sin cambio), ctl 44, mirror 45,
  CtlMsg 480. Panel "Residual compose" (default ON, live).
- nr_delta.hlsl: origBil (bilinear del original a los MISMOS taps de
  work-res que muestrean la respuesta), EncodeProxy (función pura =
  encode), edit = modelDisp - smallProxy, CubeScaleResidual (hhkbble)
  aplicado: modelDisp' = fullProxy + edit*a. Solo dispara con ws<1.
- DecCb 17→18 consts; root sig 11→18 DECLARED==PUSHED (hallazgo: el
  decode pre-4 empujaba 17 sobre 11 declarados y ESTE driver entregaba
  de todos modos — los knobs R82k funcionaron por casualidad; ahora es
  correcto por construcción).
- Feeder: SLI_FEEDER_WORK / SLI_FEEDER_RESIDUAL.
- Offline: rama dispara (TINT 39% píxeles difieren, diff 0.009), gain
  de CALIDAD invariante (los guards ya absorbían el edit pequeño en
  replay estático), sin NaN, rc=1, 0 franjas.

### LIVE (boot 01:42, user en partida)

- DELTAs 23-26 ms (menú DRS 1505×635) / 69-84 ms (2560×1080 ws 0.5).
- Veredicto user con config exagerada (split 500, boost x4, detail x4,
  colour 2, bound 4): "veo cambios en la vista NR pero en lo que
  presenta el juego apenas aprecio cambios".

### CIERRE

Nuestra composición (gain = ratio re-anchored + floor + bound + chroma
clamped) ya era conservadora donde el fork compone aditivo — la
divergencia que el residual cura allí apenas tenía dónde morder aquí.
**Residual queda ESTRUCTURAL (default ON)**: no puede empeorar (a ws=1
colapsa a bit-idéntico; offline gain invariante), legaliza workScale
bajos, y su efecto en nuestro compose es invisible — como el offline
predijo. El ws 0.5 desbloqueado queda corriendo (69-84 ms).

### Config ideal aplicada y persistida (user request)

- intensity/localStructure/localTone/skinStructure 1.0 (estaban a 2.0
  máx de toqueteos), colour 0.25 (estaba 2.0 = 100% del chroma del
  modelo — el más peligroso), gainBound 2.0, boost 1.0, detail 1.0,
  workScale 0.5, split 0.
- Panel: R89e WM_ACTIVATE recarga sli.ini (fix desalineación fantasma);
  R89f un solo botón Apply = apply+save.

Commits: 3cec5b7 (plan) → 7c879da (deploy) → 69671f9 (R89e) →
43aebc1 (R89f) → cierre (este).
