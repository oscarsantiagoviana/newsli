# R82 — Estabilización del crash live + calidad AMDNR + verificación offline

Fecha: 2026-09-29/30 (noche). Commit `942d66e`. Deploy verificado (hashes OK).

## Contexto

El user reporta "última modificación dio error y el juego se paró". La sesión 22:38
murió: 1 evaluate → compose #1 → device removed 0x887A0001 → crash del juego en
su módulo anti-tamper → cascada (engine poison, dumps).

## Diagnóstico (evidencia, no conjeturas)

1. **Dump del juego (173 MB)**: AV write dentro del módulo anti-tamper, mismo
   offset que los WER previos de 21:19 y 21:22 (misma firma). El anti-tamper muriendo
   con el proceso, no el asesino.
2. **Cronología fina del host log**: produce 1 a las 21.607 → EnsureStruct falla
   0x887A0005 a las 25.107 (el device YA estaba muerto; los creates devuelven
   device-removed al instante). El Present del juego quedó **bloqueado 3.5 s**
   esperando el primer delta (arranque frío del engine: vendor init 2.3 s +
   evaluate 0.8 s). Sin TDR del driver (Event Log limpio) → device-removed a nivel
   app: RDR2/EMP no tolera un Present multi-segundo.
3. **9 dumps del engine, TODOS idénticos** (D3D12Core.dll+0x1D9DC, stack en
   nvngx_dlss.dll): thread residual del runtime NGX que AV tras `main()` limpio
   (rc=0) durante el teardown del CRT. Colateral, no causa.
4. El draw NUNCA se ejecutó de verdad (el fence "completó" por la remoción), así
   que el blend multiplicativo R81 nunca fue el asesino — la sesión murió ANTES.

## Fixes (todos estructurales, no opcionales)

- **Cold-start gate** (`PresentGateInner`): hasta que el engine NO entrega su
  primer delta, present nativo sin bloquear. El contrato same-frame estricto se
  recupera desde el frame 2 (warm). Elimina la clase entera de "Present
  bloqueado en arranque".
- **Engine hard-stop**: `TerminateProcess(GetCurrentProcess(), rc)` tras el exit
  limpio — imposible el AV post-exit del thread nvngx (57 dumps de historia).
- **Telemetría**: `GetDeviceRemovedReason` con nombre legible (DEVICE_HUNG vs
  DRIVER_INTERNAL vs DEVICE_REMOVED).
- **Blend alpha**: en ambos modos el alpha del backbuffer pasa intacto (el
  multiply con SRC_COLOR en alpha lo habría puesto a 0).
- **Gain floor knee absoluto** (nr_delta.hlsl): floor por-píxel 2%-del-pico →
  dividía por ~4e-4 en oscuros y amplificaba ruido del modelo. Ahora: pk < 0.02
  → gain NEUTRO. **Verificado por números** (ver abajo).
- **MaxRatio 2.0 + luma/croma split** (patrón AMDNR `DlssNrMaxRatio` +
  TransferStrength/ColourStrength): clamp [1/2, 2] ("a detail pass has no
  business restyling a light source") + croma del edit al 25%.
- **preExposure ancla**: `UpdateExposure(gamePreExposure)` — el preExposure del
  evaluate del juego manda; mediana de tiles como fallback.
- **Panel**: filas placebo jitterX/Y y mvScaleX/Y eliminadas (documentadas como
  no-ops desde R78).
- **ini envenenado**: nrTint=2 (estado de debug de la sesión de vistas) → 0.

## Verificación offline (replay_feeder, captures reales nr_noisy)

G2 CONTENT 5/5, G3 STABILITY 5 frames. Análisis del gain transportado
(pitch 1728, no el reshape ingenuo — trampa documentada):

```
f0-f4: lum mediana 0.92-0.96 | p99 ≤ 1.016 | 0 px <0.5 | 0 px >2 | neutral 33-40%
```

Antes del floor fix: media 0.78-0.81 (oscurecía un 20%), p1=0.000 (píxeles
negros por división ruidosa). Después: acotado y sin outliers.

## Pendiente / conocido

- **Sesgo azul del modelo** (-15% B sistemático en los captures NOC): el
  taper de croma al 25% lo deja en ~4% efectivo; con boost=1 casi invisible.
  Investigar si es matriz de primarios del runtime vendor (P3).
- **Verificación live**: bloqueada autónomamente — el menú del juego ignora
  input sintético (el anti-tamper lo filtra). El juego
  quedó lanzado en el menú principal esperando Enter humano.
- EnsureStruct sigue lazily-fail-soft (tint=2 funcional pero su RS falla si el
  device murió; con el cold-start ya no corre en ventana crítica).
