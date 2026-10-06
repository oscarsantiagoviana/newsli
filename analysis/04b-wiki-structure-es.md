# Temas Wiki - new-sli (propuesta de estructura completa)

Propósito: wiki pública en INGLÉS para D:/proyectos/new-sli, con el mismo nivel de detalle que la wiki actual de poc-dlss-standalone (16 ficheros). Contenido: qué es el proyecto (offload de DLSS NR a GPU secundaria alrededor del SR nativo), historia completa R1-R73c y documentación operativa. Reglas: inglés en código y wiki, sin credenciales, sin assets de NVIDIA redistribuidos, actualizada con cada revisión importante.

## Páginas propuestas (18 + README)

| Página (EN) | Descripción (1 línea) | Origen |
|---|---|---|
| README.md | Qué es new-sli, estado en una frase, índice | reescribir del README viejo |
| 01-vision-and-goals.md | Qué es, por qué existe, reglas de trabajo del usuario, hardware (2x RTX 3060, RDR2) | 01-vision-y-objetivo (traducir) |
| 02-architecture.md | Componentes (host proxy nvngx.dll, coproc_engine, panel), flujo por frame, bridge, failsafes jerárquicos | 02-arquitectura (traducir + ACTUALIZAR: camino delta, pre-SR eliminado por decisión R73c) |
| 03-weights-extraction.md | De nvngx_dlssnr.dll a dlssnr_logical.safetensors (649 tensores), contratos de E/S, corpus de capturas | 03-extraccion-pesos (traducir) |
| 04-nr-pipeline.md | El grafo de 71 bloques y la semántica vendor (E4M3 publish, softmax approx, swizzle, quadratic gate) | 04-pipeline-nr (traducir) |
| 05-historical-worklog.md | Worklog histórico cronológico R1-R73c (este documento, EN) | NUEVO |
| 06-tensorrt-engine.md | Export ONNX, cirugía (constantes broadcast, fp16 selectivo), fidelidad 42 dB, límites | 06-tensorrt (traducir; papel = referencia de destilación tras R71b) |
| 07-vendor-nr-runtime.md | Runtime vendor in-process: receta completa (core Init_Ext, cap block, depth 0.5 + DepthInverted, float slot, codec GPU nr_codec.hlsl, leak y recycle WS>5GB) | NUEVO (consolida R65-R68 de 15-referencia + runbook) |
| 08-bug-catalog.md | Catálogo síntoma -> causa raíz -> fix, ampliado con R69-R73 (delay-line, MipLevels, panel OOB, -1 knobs, RS 11 consts, hist[0]) | 08-catalogo-bugs (traducir + ampliar) |
| 09-benchmarks.md | Todas las mediciones en la 3060: vías NR, round-trips, fps, workScale, A/B en vivo | 09-benchmarks (traducir + añadir R71-R73) |
| 10-fidelity-validation.md | Cadena de verdad, bit-exactitud, PSNR, descomposición del gap (softmax vs fp16) | 10-validacion-fidelidad (traducir) |
| 11-upscalers.md | DLSS 310.8 nativo vs XeSS/FSR vía OptiScaler, por qué no cambiar de escalador | 11-escaladores (traducir) |
| 12-future-work.md | FG (fase 2), QAT/destilación, vía temporal, captura de la cadena de lanzamientos | 12-trabajo-futuro (revisar prioridades post-R73c) |
| 13-runbook.md | Arrancar, señales de vida del log, diagnóstico rápido, deploy, matar por PID, regresión offline | 13-runbook-operacion (traducir) |
| 14-glossary.md | Vocabulario: NR/SR/NGX, delay-line, delta, eco, workScale, boost, nrStage, delta-tint, badge | 14-glosario (traducir + términos nuevos R63-R73c) |
| 15-fork-reference.md | Referencia OptiScaler-DLSSNR-PreSR-Multipass: cómo funciona, qué corrige de nuestras conclusiones, qué adoptamos | 15-referencia-optiscaler-dlssnr (traducir) |
| 16-panel-design.md | Diseño del panel: regla de arrays conjuntos (NPARAMS), knobs siempre-valor vs override, slider boost 0..16, workScale, utf-8 | NUEVO (lecciones R21-R73 + revisión profunda db059bd) |
| 17-verification.md | Verificación visible: delta-tint reemplaza al badge; el post-SR no es presentado por RDR2; capturas solo CopyFromScreen + borderless | NUEVO (R70e + R73c) |
| 18-publishing-and-licensing.md | Readiness para repo público: qué se publica y qué no, licencias y atribución | NUEVO |

## Mapa viejo -> nuevo

- Directas (traducir): 01->01, 03->03, 04->04, 06->06, 08->08 (+ampliar), 09->09 (+ampliar), 10->10, 11->11, 12->12, 13->13, 14->14, 15->15.
- 02-arquitectura -> 02: traducir y ACTUALIZAR (quitar pre-SR/texNrColor como camino; queda el delta; engine sin sesión NGX).
- 05-historial-optimizacion -> 05: sustituido por el worklog histórico EN completo (R1-R73c, todas las eras, no solo R53-R73c).
- 07-integracion-engine -> se reparte: la saga R54-R63 queda en 05 (worklog) y lo operativo en 02/13; el backend vendor (R65-R68) pasa a la nueva 07.

## Qué hay que escribir de cero

1. 05-historical-worklog: el documento EN consolidado R1-R73c (ya producido; verter verbatim).
2. 07-vendor-nr-runtime: consolidar R65-R68 + R69 (guides): receta stub (exe nvngx.dll.exe, app 101616311, sdk 21, depth 0.5 + DepthInverted=1, BGRA8 in / RGBA16F out, float slot), codec GPU, descs válidas (MipLevels=1, StructureByteStride, ALLOW_UAV), leak ~10 MB/s y recycle por presupuesto WS.
3. 16-panel-design: el panel nació en R21-R27 y creció hasta romper (db059bd: 3 OOB + workScale 0 silencioso). Documentar el diseño correcto: arrays e inicializadores crecen con NPARAMS, semántica siempre-valor para knobs NR, boost 0..16 (uso: 8-10 para VER, 1.0 para calidad), workScale 0.25-2.0.
4. 17-verification: por qué el badge falla (1.0 no atraviesa el tonemap; 512 sí) y por qué el post-SR NO es verificable en RDR2 (no presentado). Estándar nuevo: delta-tint rojo/azul x32 HDR en el camino presentado + A/B por píxeles (118.5% vs 19.9% como referencia de calibración). Reglas de captura (PrintWindow/DWM inválido en fullscreen exclusivo).
5. 18-publishing-and-licensing: pesos extraídos de DLLs NVIDIA y engines TRT NO se redistribuyen (publicar solo las herramientas de extracción); runtime ShortFuse/cross-gen y DLLs NVIDIA fuera del repo; capturas de RDR2 no redistribuibles (usar capturas sintéticas/propias); atribución: fork OptiScaler-DLSSNR-PreSR-Multipass, MLX-DLSS, dlssg_for_sm86, Neural-coprocessor, DLSS5-NeuralScreen como referencias; repo 100% inglés, sin credenciales ni rutas personales.
6. Actualizaciones obligatorias de contenido traducido: 02 (arquitectura sin pre-SR), 09 (workScale 52.5/28/19.5 ms, A/B 21.7%/3.3, 26.6%/7.0, 64%/10.9, tinte 118.5%/19.9%, vendor 38-53 ms), 12 (la palanca de velocidad ya no es precisión: QAT/destilación; pre-SR fuera), 14 (delta, boost, workScale, nrStage, delta-tint, recycle).

## Criterios de detalle (mismo nivel que la wiki actual)

- Cada página con números medidos y fecha/round de procedencia; nada de claims sin medición.
- Los bugs referenciados desde cualquier página enlazan al catálogo (08) por id de round.
- Mantener el worklog (05) como fuente canonal de la historia: las demás páginas enlazan a rounds concretos en lugar de repetir la historia.
