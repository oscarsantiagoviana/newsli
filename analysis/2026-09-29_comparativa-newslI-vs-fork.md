# Comparativa new-sli (R79/R80) vs fork OptiScaler-DLSSNR-PreSR-Multipass — pipeline por pipeline

Fecha: 2026-09-29 (noche). Fuentes: `2026-09-29_nuestras-pipeline-map.md` (lectura propia completa) + `2026-09-29_fork-pipeline-map.md` (análisis línea a línea delegado, verificado en revisión). Cada afirmación con archivo:línea de su mapa.

## A. Tabla maestra de fronteras

| Frontera | new-sli | Fork | Veredicto |
|---|---|---|---|
| Entrada al juego | dxgi.dll proxy (38 fwd) + load hook nvngx | Hook completo de OptiScaler (reemplaza el evaluate) | Diferente por diseño: nosotros SOMOS transparentes; el fork ES el upscaler |
| Punto de enganche NR | `EvaluateFeature_C` → seal; compose en `Present` (backbuffer) | `EvaluateBeforeUpscale` → Before(); `EvaluateAfterUpscale` → After() (mismo frame) | **Ambos same-frame** (R79 = paridad con DeferredSr en timing) |
| Input del modelo | color sellado pre-SR HDR lineal → encode Reinhard+gamma E=0.18/mediana (lag 1f) | Color→edited render-res → encode reversible (soft-knee/Neutwo/híbrido) con white point anclado a preExposure/exposición del juego, exposición viva in-shader (0 latencia) | **DIVERGENTE — el fork tiene sistema de exposición completo, nosotros una mediana aproximada** |
| Viaje del edit | delta display-domain RGBA16F [-1,1] → bufOut cross-adapter → **bilinear corner-to-corner** en present_add | residual comprimido signado 0.5±d/(1+\|d\|) RGBA16F → **escalado por una feature DLSS privada** (upscaler real, exposición unitaria) | **DIVERGENTE — el edit viaja escalado por DLSS en el fork, por bilinear en nosotros** |
| Composición | draw aditivo sobre el BACKBUFFER en Present (post-tonemap del juego) | ApplyResidual sobre el Output del SR (pre-tonemap/post-SR, dentro del pipeline HDR del juego) | **DIVERGENTE — la diferencia estructural más importante (ver §1)** |
| Guías | depth R32_FLOAT (clone) + MV crudo, triple buffer, `frame%3` (coherente con seal N) | depth/MV del juego al evaluate; región MV por extent de output (fix cmh1448) | Paridad funcional; ojo: nosotros no aplicamos el fix de región MV |
| Jitter | no consumido (muerto en nuestra cadena) | reenviado al DLSS privado (para escalar el residual) | Paridad de diseño: el modelo NR no ve jitter en NINGUNO |
| Acumulación temporal | NINGUNA (R79 la eliminó; hist solo en tint=2, solo-vista) | El resolve principal NO acumula por diseño ("the model re-decides"); ResidualAcrossRR OFF por defecto | **Paridad — nuestra directiva R79 coincide con el path por defecto del fork** |
| Reset/invalidación | reset del juego → evaluate reset; sin hist que invalidar | reset → todas las features + acumulador + DLSS privado | Paridad (más simple al no tener hist) |
| Skin | `kSkinStructure=-1`, `UseAutoMask=1` (knobs del modelo) — sin máscara host | máscara YCbCr SkinColourWeight + controles duales skin/entorno + clasifica el frame intacto | **FALTA el port (documentado, pendiente)** |
| Multipass | 1 pasada | 1..3 (30) features encadenadas base→A→B, historial por pass | Falta (fase posterior, planificado) |
| Sync GPU | fences produce/done cross-adapter + own compose queue + wait CPU 2s | timestamp queries por seam, generaciones jubiladas por marcador GPU, fences por slot en Late | Diferente problema: el nuestro es cross-proceso; disciplina del fork es referencia para F6 |

## B. Las diferencias que pueden explicar "problema en cómo alimentamos o entregamos"

Rankeadas por probabilidad de impacto visible, con el mecanismo causal:

### 1. Punto de composición: post-tonemap (nosotros) vs pre-tonemap (fork) — LA SOSPECHA PRINCIPAL

- **Nosotros**: `delta = enc(model(orig·E)) − enc(orig·E)` se calcula sobre el HDR lineal sellado pre-SR, se transporta a display-domain, y se SUMA al backbuffer FINAL (`present_add.hlsl:47-52`, ADD ONE/ONE). El backbuffer ya pasó por el SR del juego + tonemap + post del juego.
- **Fork**: el residual se compone sobre el Output del SR (`DlssNr_DeferredSr.inl:650-676`) y el TONEMAP DEL JUEGO procesa el frame ya compuesto — el edit NR sufre la misma curva de tonemap que el resto del frame.
- **Mecanismo de daño**: nuestro delta asume que `backbuffer ≈ enc(orig)` + constante local. Donde el tonemap del juego comprime (sombras, highlights), el delta queda **mis-escalado regionalmente**: demasiado fuerte en zonas comprimidas, débil en las expandidas. En movimiento (eye-adaptation de RDR2) el desajuste varía frame a frame → "adherencia inestable" a las formas, exactamente el síntoma que describes.
- **Nota histórica**: R73c intentó componer en el Output post-SR y RDR2 lo omitía (el juego copia el out DLSS pre-present). El fork lo resuelve porque su After corre DENTRO de la cadena de evaluate hookeada (el juego nunca ve el Output limpio). Para nosotros repetir eso exige interceptar el recurso de salida de forma más profunda — o aceptar el backbuffer y compensar.

### 2. Escalado del edit: bilinear (nosotros) vs DLSS privado (fork)

- El edit NR nace a render-res (1707×720) y llega a display (2560×1080) con UN tap bilinear (`present_add.hlsl:51`, sampler LINEAR). El fork escala el residual con una feature DLSS real (motion-aware). Un edit con detalle fino (grano estructurado, bordes) llega borroso y con ringing suave en nuestra cadena. Mitigable sin arquitectura nueva: al menos catmull-rom o 4 taps; idealmente el compose stage F6.

### 3. Exposición: mediana lag-1f (nosotros) vs white-point anclado + viva (fork)

- `expoScale = 0.18/mediana(frame anterior)` (nr_vendor.h:777-788) vs `clamp(preExposure/gameExposure·trim, 0.01, 4096)` + lectura in-shader sin latencia (`dlssnr.hlsl:271-283`). El `preExposure` que el juego nos entrega se IGNORA (grep: 0 consumidores).
- En transiciones de eye-adaptation, nuestra E va 1 frame detrás y el borde del guard de saturación (`enc≥0.985→d=0`, nr_delta.hlsl:84-85) se mueve → parpadeo en márgenes brillantes. El fork ancla y Lee en vivo.

### 4. Encode del portador: Reinhard+gamma con clamp (nosotros) vs reversible hue-preserving (fork)

- Nuestro `pow(min(d/(1+d),1),1/2.2)` (nr_encode.hlsl:89-92) recorta a 1.0 el HDR >1/E y comprime el hombro — el modelo ve menos rango del que el fork le da (Neutwo: [0,∞)→[0,1) sin punto de clip). Autoconsistente en el round-trip (R78 §5), pero con menos señal útil en highlights.

### 5. Skin/face: sin máscara host (port pendiente, ya documentado)

### 6. Lo que NO es divergencia (verificado paridad)

- Same-frame (directiva R79 = path por defecto del fork).
- Sin acumulación temporal en el path de calidad (el fork también la eliminó de su resolve principal).
- MV al modelo en dominio work por producto (fórmula literal).
- Jitter fuera del modelo NR en ambos.
- El par (delta N, guías N) sellado y GPU-ordenado.

## C. Plan propuesto (orden por impacto/coste)

1. **Medir el daño real del punto de composición** (experimento, no código): capturar frame sellado (pre-SR HDR) + backbuffer final del MISMO frame, ajustar la transferencia real del juego (SR+tonemap) y medir cuánto se desvía el delta aplicado del delta "correcto" (composición pre-tonemap sintética offline con los dumps). Si la desviación es grande en sombras/highlights → confirma §1.
2. **Compensación barata en el shader** (si §1 se confirma): modular el delta por la luminancia local del backbuffer (ratio luma-backbuffer/luma-enc(orig)) — aproximación al round-trip del tonemap sin tocar el pipeline del juego. El fork hace algo análogo en finished-colour (log-gain relativo, clamp 1/8..8, `dlssnr_finished_color.hlsl:40-75`).
3. **Escalado del edit**: sustituir el tap bilinear por catmull-rom en present_add (self-contained, sin ABI).
4. **Exposición**: consumir `preExposure` del juego como ancla (ya viaja en FrameScalars:42) + hold; mantener mediana como fallback.
5. **Skin port** (ya planificado).
6. Multipass (fase posterior).

## D. Bugs propios ya fixeados esta ronda (commit a2202d7)

- BUG-1 RS gráfica 1 SRV vs shader 2 texturas → PSO E_INVALIDARG para TODO formato (el "fmt 87" era un síntoma, no la causa). Fix: range count 2.
- BUG-2 UAV del merge CS en slot equivocado (OFFSET_APPEND → slot 1 null-SRV). Fix: offset explícito 2.
- BUG-3 spam de log → LogRate.

Pendiente de verificación live (user opera): relanzar RDR2 → composes suben de nuevo → tint=2 (structural) por panel → juzgar la vista.
