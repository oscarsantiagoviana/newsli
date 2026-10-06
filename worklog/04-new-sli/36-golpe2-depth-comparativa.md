# 36 — Golpe 2: análisis comparativo del guide DEPTH contra los proyectos de referencia

Fecha: 2026-10-03 (noche). Manda user (verbatim): "en cuanto al punto 4 no me
creo, contrastalo con los proyectos de referencia, estoy casi seguro de que lo
que pasa es que no se la estamos dando bien o en el formato que espera o con
las dimensiones correctas".

## 0. Qué entregamos hoy (verificado en código, no de memoria)

| Campo | Valor hoy | Fuente |
|---|---|---|
| Resource | textura local engine R32_FLOAT (fmt 41), flags NONE, nace SRV, ciclo SRV↔COPY_DEST | loop.cpp:260 MakeLocalTex(guideDepthFmt=41) |
| Dims | 1707×720 (render = guideW/H del juego) | log engine "guides open ... 1707x720" |
| Contenido | **PLANO DE ZEROS** — upload ring memset 0 copiado cada frame (RecordDepthCapture) | offload_session.cpp:1433-1444 |
| DepthInverted | 1 (de srFlags bit 3 del create DLSS del juego) | offload_session.cpp:1719; log "depthInverted=1" |
| Subrects depth | (0,0, guideW, guideH) — explícitos, sin offset | nr_vendor.h evaluate call |
| Fallback sin depth | NO existe path "sin recurso depth" — siempre textura+zeros | — |

Nota crítica: la textura de ZEROS se entrega con **DepthInverted=1**. Bajo
etiquetado invertido, 0.0 = **LO MÁS LEJOS** (cielo). Estamos diciéndole al
modelo "todo el universo está en el horizonte, no hay nada cerca". El feeder
offline usa 1.0 (near) con el mismo flag — **el feeder y el juego NO prueban
la misma hipótesis** (inconsistencia descubierta en este análisis).

## 1. Qué piden las referencias (verbatim, archivo:línea)

### Fork NeuRotic (OptiScaler-DLSSNR-PreSR-Multipass) — la referencia maestra
- **Entrega el depth DEL JUEGO tal cual** ("depth/MV del juego al evaluate",
  comparativa 2026-09-29 fila Guías; DlssNr_Dx12.cpp:5852 lee el recurso del
  bloque evaluate del juego y lo pasa directo).
- **Clona typeless→typed**: `R32_TYPELESS→R32_FLOAT`, `R16_TYPELESS→R16_UNORM`
  etc. (TypedGuideFormat, DlssNr_Dx12.cpp:1883-1905) — "NGX requires its
  inputs in NON_PIXEL_SHADER_RESOURCE at evaluate time, which is a documented
  contract" (comment 1928).
- **DepthInverted se deriva de los create flags DLSS del juego**
  (DlssNr_Dx12.cpp:5852-5856: `frame.DepthInverted = createFlags &
  NVSDK_NGX_DLSS_Feature_Flags_DepthInverted`). Nosotros: igual (bit 3).
  ✔ paridad.
- **NUNCA re-etiqueta el contenido**: ni invertir, ni clamp, ni rescale. Si el
  juego usa log-depth, el runtime del modelo lo espera así (fue entrenado con
  games reales).
- **Subrects depth/mv por guía** con guideWidth/Height reales
  (dlssnr_forwarder.cpp:906-911 + origins opcional). Nosotros: ✔ paridad
  ((0,0,gw,gh)).

### El resto del inventario
- **dlss5-video-player** (cebo NGX): ni siquiera pasa depth en create — el
  modelo NR corre SIN recurso depth (create "sin recursos, sin depth, sin
  Backbuffer", ficha DLSS5-NeuralScreen.md:15; wiki 07:51-54: sin depth el
  runtime hace passthrough seguro PERO los pesos cargan).
- **addon-dlssnr-linux**: Depth/MVec subrects a render_w×render_h
  (ficha :84) ✔ mismo patrón nuestro.
- **OptiScaler_DLSSNR (ficha)**: evaluate-time setea Depth + DepthInverted +
  subrects por guía ✔ mismo patrón nuestro.
- **dlss5-webcam-demo**: authors a proxy normalizado (near=1) que él mismo
  fabrica — pero es un WEBCAM demo: no hay depth real disponible por diseño.

**Consenso de TODAS las referencias: el depth del juego se entrega TAL CUAL
(typed-view si typeless), con el flag que el juego declara. NINGUNA entrega
zeros.** El único host que fabrica un proxy (webcam) es porque no existe depth
real.

## 2. La historia R82f re-examinada (arqueología git + wiki 07)

El veredicto original ("RDR2's log depth poisons the runtime's reprojection
into a mono-channel answer in ANY labelling — measured raw / inverted /
clamped / rescaled") se midió en la cadena POC de entonces:
- wiki 07:288-294: **depth real R32G32_SINT = dispatch death por FORMATO**
  (no por datos); depth convertido R32_FLOAT vía CS = "dispatch survives;
  evaluate rc content-dependent — **test with real sealed guides before
  concluding**"; stub 0.5+DepthInverted=1 = validado.
- El "poison" medido fue: **mono-channel stripe en la RESPUESTA** con depth
  real en UNA configuración de la cadena de entonces (encode/decode previos a
  la corrección del swizzle R82f, 9x9 median, slot-pick). Commits 7be08ff →
  a6f0590 muestran que EN ESA MISMA RONDA se descubrieron: swizzle
  byte0=B,R byte1=R, byte2=G en la respuesta, franjas de 1px, half-height
  live region. Es decir: **la cadena de lectura tenía 3 bugs graves
  simultáneos cuando se midió el "poison"**.
- La advertencia estaba escrita y fue ignorada por prisa: wiki 07:292
  "**test with real sealed guides before concluding**" — nunca se hizo con
  la cadena POST-fixes.

**Hipótesis de trabajo (golpe 2):** el "poison" era un artefacto de lectura
de la cadena R82e (swizzle/median/slot), no del depth. El user tiene razón
a priori: no existe evidencia de que el modelo rechace log-depth — TODAS las
referencias lo entregan crudo.

## 3. Veredicto por campo (nosotros vs contrato de referencia)

| Campo | Referencia | Nosotros hoy | Veredicto |
|---|---|---|---|
| Recurso depth | textura del juego (o clone typed) | textura propia R32_FLOAT con ZEROS | ✗ DIVERGE (proxy) |
| Formato | typed view del juego (R32_FLOAT tras clone si typeless) | R32_FLOAT | ✔ OK |
| Dims | guía a render res + subrects explícitos | 1707×720 + subrects (0,0,gw,gh) | ✔ OK |
| DepthInverted | flag del create DLSS del juego | bit 3 de srFlags | ✔ OK |
| Estado recurso | NON_PIXEL_SHADER_RESOURCE en evaluate | SRV↔COPY_DEST → SRV | ✔ OK |
| Contenido | depth real crudo (log o lineal, como venga) | plano de zeros | ✗ **DIVERGE — el campo del user** |
| Consistencia flag/contenido | flag describe el dato real | **flag=1 (inverted) + zeros=far → "todo lejísimos"; feeder usa 1.0=near con mismo flag → inconsistente** | ✗ BUG LATENTE |

**Formato y dimensiones están BIEN** (paridad con todas las referencias).
Lo que diverge es el **CONTENIDO** (zeros vs depth real) y la coherencia
flag/valor. La intuición del user apuntaba al campo correcto pero al atributo
equivocado: no es el formato ni las dims — es qué hay DENTRO.

## 4. Por qué importa (mecanismo, sin tecnicismo)

El depth le dice al modelo "qué píxeles son la MISMA cosa física que en el
frame anterior aunque se hayan movido". Con zeros=infinities-aways el modelo
no puede emparejar NADA temporalmente: trata cada frame como un escenario
nuevo y su acumulación temporal (la que quita ruido con el tiempo) queda
mutilada. El modelo sigue "funcionando" (responde, corrije ruido espacial)
pero pierde la mitad de su inteligencia. Es el mismo motivo por el que
flowReset cada 6 px borraba historia: sin depth no hay memoria coherente.

## 5. Plan de experimento (decisor, no paliativo)

Paso único con A/B triple offline PRIMERO (feeder, sin juego):
1. **Feeder con depth REAL**: quitar el hardcode `f[k]=1.0f` → enviar
   `cap_depth.raw` crudo (el POC lo capturó del juego real; es log-depth,
   escena 0..0.08). Gate: delta con contenido + rc=1 + SIN mono-channel.
2. **Consistencia host**: el proxy de zeros debe pasar a **1.0 (near)** si
   algún día vuelve a usarse como fallback — o mejor: el fallback pasa a
   ser "sin recurso depth" (las refs lo toleran: passthrough seguro).
3. Solo si (1) pasa: cadena real — capturar depth del juego en el hook
   evaluate (GetParamResource "Depth"), clone typed en el host (patrón fork
   CreateGuideClone), sellarlo por el anillo guide existente (ya hay 3 slots
   guideDepth transportando zeros — misma infra, otro contenido).
4. Validación in-game: A/B con user (calidad LEY 2) + capturas ×2 +
   cronómetro (el evaluate puede abaratarse: con depth bueno el modelo usa
   reprojection en vez de recomputar — hipótesis secundaria a medir).

## 6. Riesgos
- R1: dispatch death por contenido (lo que creyó R82f) → se verá EN EL
  FEEDER antes de tocar el juego; abortamos y queda documentado con la
  cadena moderna (ya no tiene los 3 bugs de lectura).
- R2: rc != 1 content-dependent → probar etiquetados {crudo+flag juego}
  primero (es lo que hacen TODAS las referencias); solo si falla, los
  inventos de re-etiquetado (y documentar por qué).
- R3: calidad PEOR con depth real (el modelo decide usarlo y el log-depth
  confunde SU reprojection ya sin culpa nuestra) → LEY 2: revert, pero con
  evidencia moderna, no con la medida contaminada de R82f.

---

## EJECUCIÓN FEEDER (21:1x) — veneno REFUTADO; calidad requiere MOVIMIENTO

- Implementado `SLI_FEEDER_DEPTH=real|one|zero` (+`SLI_FEEDER_DEPTH_INV`):
  'real' extrae el canal PAR de cap_depth.raw (verificado offline: R32G32
  a pitch de color, .x = depth log 0..0.063, ~21% sky-zeros, .y basura)
  a un plano R32_FLOAT tight a su propio pitch. Buffer bGd re-dimensionado
  a depthBytes (antes compartía el tamaño del color). Build 0 err/warn.
- Primera pasada con real: etiquetado invertido=0 por mi lectura apresurada
  de la geometría — CORREGIDO tras medir: sky=0, geometría cercana=MÁS
  grande (0..0.063) = near-is-large = INVERTIDO, y el create-flag del
  juego dice 1. real ahora navega con DepthInverted=1 (paridad total con
  lo que el juego declara). Rebuild + re-test.
- **A/B triple (one/zero/real, loops=1, misma captura): rc=1 en todos,
  delta 100% contenido, SIN franja mono-canal, SIN dispatch death, DELTA
  44-68 ms (mismo rango)**. El veredicto R82f queda formalmente refutado
  con la cadena moderna: el depth logarítmico REAL del juego fluye limpio.
- **Comparación píxel a píxel de los deltas dumpeados: BIT-IDÉNTICOS entre
  los tres modos** (|diff| ~1e-5 = redondeo). Interpretación honesta: la
  reproducción es la MISMA frame x5 (escena estática, cámara fija) — el
  depth solo pesa en la re-proyección temporal CON MOVIMIENTO; estático no
  hay nada que reproyectar. El feeder NO puede responder la pregunta de
  CALIDAD: necesita la cadena viva.
- CONCLUSIÓN del experimento: (1) nada se rompe con depth real — candidato
  seguro para la cadena viva; (2) la mejora de calidad (si existe) solo se
  manifestará en juego con cámara en movimiento; (3) el coste podría BAJAR
  (reprojection vs recompute) — a medir en vivo.
- SIGUIENTE (pendiente de go): host vivo — leer el depth del bloque
  evaluate del juego (GetParamResource "Depth" en OfferFrameToSession),
  clonar typed si typeless (patrón CreateGuideClone del fork, las DSV no
  son copiables), sellarlo por el anillo guideD existente (misma infra,
  otro contenido), A/B de calidad con user + cronómetro.
