# 39 — Golpe 3b: de-jitter EN EL SELLO (encode) para rescatar el pre-SR

Fecha: 2026-10-04 (madrugada). Padre: worklog 38 (pre-SR cerrado por
oscilación; causa = jitter residual del raster en el sello).

## La idea en una frase

Si el frame sellado llega AL MODELO ya estabilizado (muestreado con el
offset de jitter aplicado en contra), el modelo ve el mismo encuadre
sub-píxel que POST-SR — y se rescatan los 34-43 ms demostrados sin la
oscilación medida.

## Anatomía del jitter hoy (verificado en código)

- Fuente: `a.jitterX/Y` = `NVSDK_NGX_Parameter_Jitter_Offset_X/Y` del
  bloque evaluate del juego (nvngx_host.cpp:484-485), unidades píxeles
  RENDER (guideW).
- Host: `scalars->jitterX/Y = nrPreSr ? a.jitterX : 0` (offload_session:
  2573-2576) — solo viaja en pre-SR.
- Consumo HOY (motor): DOS lugares reciben el jitter SIN compensar nada
  upstream:
  1. EVALUATE (nr_vendor.h:1083): `jitterX * workW/guideW` al forwarder —
     el runtime del modelo lo usa para su reprojection interna.
  2. DECODE (nr_vendor.h:1142): `jitScaleX/Y` — el decode muestrea la
     respuesta desplazada (-jx,-jy) para que el edit caiga en el píxel
     un-jittered.
- El ENCODE (muestreo del color sellado → BGRA8 del modelo) NO recibe
  jitter: muestrea el frame jitterado tal cual (Load 1:1 o box-average).

## El cambio

**Un solo sitio: el ENCODE muestrea desplazado por el jitter.**

- `EncCb` crece: +2 floats `jitX, jitY` (unidades WORK = jitter *
  workW/guideW — mismo factor que usa el decode hoy).
- `nr_encode.hlsl`: el punto de muestreo se desplaza `-jit` (1:1: Load →
  Sample bilinear con el offset; box: las esquinas del footprint se
  desplazan — el box ya interpola, el offset entra limpio).
  - t0 (texInColor) necesita sampler LINEAR (hoy es Load puro; 1:1 con
    offset ≠ 0 requiere Sample). El box-path puede seguir con Loads
    desplazados (redondeo) + mejor Sample por esquina — decisión de
    implementación: Sample bilinear en el centro del footprint en ambos
    caminos, documentando el cambio de kernels (el box exacto era para
    ws<1; con jitter el sample sigue siendo el centro del box + offset).
- **Coherencia evaluate**: el modelo YA recibe el jitter (1083) para su
  reprojection — si su input está de-jitterizado, su jitter-declarado debe
  pasar a **0** (si no, re-proyecta un shift que ya no existe). Cambio:
  evaluate recibe `0,0` cuando el de-jitter del encode está activo.
- **Coherencia decode**: la respuesta del modelo ya está en el encuadre
  un-jittered (su input lo estaba) → el decode NO debe re-desplazar:
  `jitScale` → 0 con de-jitter activo. El compose recibe el mismo dominio
  que POST-SR. La regla R82g.6 queda cubierta por el dominio estable.
- Escalar por frame: `scalars->jitterX/Y` siguen viajando (fuente de
  verdad); el MOTOR decide dónde compensar (toggle → 0 en todos lados =
  comportamiento pre-3b exacto).

## El toggle (REGLA ORO PANEL)

- `nrDeJitter` uint (hueco Tuning: nrDepthMode @92 usado; siguiente
  reserved… buscar hueco real con lectura completa; si no hay, extender
  con tail-align como nrGateWaitMs) — **1 = de-jitter al encode
  (default tras validar), 0 = compensación en decode (comportamiento
  actual)**. Ini + ctl 43 + mirror 44 + fila panel checkbox junto a
  Pre-SR con texto honesto.

## Validación (feeder PRIMERO, juego después)

1. Feeder con jitter simulado: nueva env `SLI_FEEDER_JITTER="x,y"` que
   escriba scalars->jitterX/Y variables por frame (p.ej. rotación de
   8 fases ±0.5 px). Gate: con de-jitter ON, los deltas de la MISMA
   captura deben ser ~bit-idénticos entre fases de jitter (la señal que
   entra al modelo no depende del jitter) — es exactamente el
   anti-oscilación probado numéricamente.
2. Con de-jitter OFF: los deltas DEBEN diferir entre fases (control
   negativo — demuestra que el jitter simulado muerde).
3. In-game: pre-SR + ws1.0 + de-jitter ON → cámara quieta vista NR:
   oscilación muerta = victoria. Luego ws0.25 para el coste.

## Riesgos

- R1: el jitter del juego en pre-SR no es el Offset_X/Y declarado (ej.
  RDR2 jittera interno y declara 0 — el viejo log "jitter (0.0000,
  -0.1667)" sugiere valores pequeños PERO no cero; el R82g.7 throttle
  existe). MITIGACIÓN: el feeder prueba el mecanismo con jitter REAL
  simulado; in-game, si con de-jitter ON la oscilación persiste, el
  jitter real no viaja en el parámetro → fallback: de-jitter por
  estimación (fuera de alcance 3b, documentar).
- R2: cambiar Load→Sample en el 1:1 altera el look un píxel-frac
  (filtrado bilinear). Aceptable: es exactamente el filtrado que el
  POST-SR ya aplica al de-jitterizar.
- R3: interación con el box de ws<1: el sample con offset sobre box
  promedio = muestreo bilinear del downsample — coherente con el
  objetivo (estabilidad) aunque pierda el box exacto teórico. Documentar.

---

## EJECUCIÓN + CIERRE (2026-10-04 ~01:00-01:30) — NEGATIVO CON HALLAZGO MAYOR

### Incidente flashes (causa raíz doble, ambos corregidos)

1. **Root sig 9 vs 12**: Num32BitValues quedó en 9 con 12 pushed —
   jx/jy/dej leían basura → offsets de de-jitter aleatorios/frame →
   flashes a toda velocidad. (La trampa R82e documentada EN el propio
   comentario del código.) Fix 9e75ccb: Num32BitValues=12.
2. **Deploy de build/ raíz (stale sep-30) en vez de build/deploy/** —
   el engine viejo leyó Tuning nuevo con layout antiguo: nrOn=1036831949
   (el float 0.1 de nrColour leído como uint), workScale 2.00 (gainBound
   2.06). REGLA NUEVA: binarios SOLO de build/deploy/.

### Experimentos (user en partida, cámara quieta, líneas rectas)

| config | resultado |
|---|---|
| pre-SR + ws 1.0 + dej ON | vibra (igual que sin dej) |
| pre-SR + ws 0.5 + dej ON | vibra un poco menos — visible |
| pre-SR + ws 1.0 + dej SIGNO INVERTIDO | algo menos — sigue visible |
| **POST-SR + ws 0.25 + dej OFF** | **ESTABLE — user: "ahora ok"** |

Jitter declarado por el juego (log R82g.7): tabla en bucle
(0.31,0.20)→(0.13,0.28)→(0.03,0.43) — real y variable.

### Veredicto físico — por qué NINGÚN signo mata la vibración

El jitter de cámara produce desplazamiento DEPENDIENTE DE LA
PROFUNDIDAD (paralaje): medio píxel de cámara mueve lo cercano casi
medio píxel y lo lejano ~nada. Una traslación global (lo único que un
shader de encode puede hacer) cancela el shift de UN plano de
profundidad; el residuo por-profundidad vibra en barandas/bordes.
POST-SR no lo sufre: el SR del juego ya de-jitteriza consciente de
profundidad (reproyección interna con MVs+depth).

### HALLAZGO MAYOR — el confound del golpe 3 deshecho

La velocidad del golpe 3 NUNCA fue del pre-SR: la dio el workScale.
Nunca se había probado POST-SR + workScale bajo:

| config | DELTA |
|---|---|
| POST-SR + ws 1.0 (histórico) | 130-169 ms |
| pre-SR + ws 1.0 | ~78 ms |
| pre-SR + ws 0.5 | ~75 ms |
| pre-SR + ws 0.25 | 36-43 ms (oscila) |
| **POST-SR + ws 0.25 (FINAL)** | **~55 ms, ESTABLE** |

Diseño del fork confirmado (OptiScaler: modelo al output del SR +
WorkingScale reducido). Default estructural nuevo: **nrPreSr=0,
nrWorkScale=0.25** — ~2.4× la cadencia histórica sin pérdida de
estabilidad (user validó). ini y defaults actualizados.

### Piezas restantes

- nrDejitter queda como knob documentado (inerte con POST-SR: jitter=0
  por construcción). Extensión natural si algún día se reabre pre-SR:
  muestrear con -jitter escalado POR DEPTH — no traslación global.
- Sign-test revertido a -j (valor teórico correcto) antes del commit.
- Commits: d8db2d2 (3b) → 9e75ccb (root sig fix) → cierre (este).
