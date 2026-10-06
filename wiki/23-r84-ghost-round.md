# 23 — R84: the ghost round (bound-is-knob, flow reset, encode-exact decode)

Ronda: 2026-10-02 · Base `e2e813a` → `f950f6c` · 6 commits · ctest 3/3 ·
deploy 5/5 md5 (dos ciclos por ABI bump).

## Objetivo

Ejecutar el plan R84 (wiki 25): A1 bound per-pixel, A2 auto-reset de
historia, B1 sRGB encode/decode parity — contra el fantasma cuya causa
raíz cerró R83 (historia vieja del modelo cabalgando el gain).

Leyes fijadas por el user ANTES de tocar nada:
1. **Same-frame intocable** — nunca se pinta un frame sin su NR (descarta
   el patrón UNCANNY de "tope de espera + present del tardío": eso ES
   frameskip).
2. **Los fps se atacan haciendo el engine más rápido**, jamás saltando
   frames. Objetivo: calidad.

## Ejecutado

| Commit | Fix |
|---|---|
| `44df2d1` | **A1**: el bound per-pixel del gain ES el knob (ctl 36 → `nrGainBound` 1..8, def 2.0, paridad MaxRatio del fork). El caso especial global ×1.12 de R82k (parche global para pregunta per-pixel) eliminado con su `pk` huérfano; `NrHiGuard()` muerto fuera (no-legacy). ABI sin cambio de layout (swap float in-place). |
| `606ef4d` | **A2**: auto-reset de historia por flujo. El encode calcula el pico de \|MV\| por tile del guía MV del juego (SRV = vista, sin transición ni copia — lección 7.5) en un UAV in-band leído por el MISMO readback que la sonda de exposición (0 comandos GPU extra). El loop del engine latchea el percentil 85 y resetea el modelo cuando el desplazamiento supera `nrFlowReset`% de la altura. Tuning 104→112 B, ctl 38, CtlMsg 424→432 B, mirrors 39. Heap de descriptores RE-PLANIFICADO por contigüidad de tablas (el primer layout caía en la trampa R82e — cazado en papel; final: encode t=[0,1] u=[2,3,4], decode t=[5,6] u=[7,8,9]). |
| `014d2fd` | La fila en la tabla de ctl NO se dibuja sola: falta el `DrawControl` en panel_ui.cpp (REGLA ORO PANEL, coste de un rebuild — lección explícita en skill). |
| `17ec27d` | **A2.1** (primer A/B falló, resets disparando y fantasma vivo): flujo = eje MAYOR (X-only no veía paneos verticales); la croma del modelo (25%) pasaba SIN bound — la historia desplazada entraba por la puerta de la croma; mismo guard para ambas. |
| `290893e` | **B1 redefinido**: el encode YA era sRGB piecewise + white point + soft knee + peak norm desde R82f (la divergencia #2 del informe estaba STALE — la lectura completa lo pilló). La violación real era su espejo: el decode reconstruía "el encode que vio el modelo" como Reinhard+2.2 (curva DIFERENTE) → `upgraded/enc` era un error de curva por píxel incluso con modelo perfecto, y ese error multiplicaba la historia desplazada. El decode ahora reproduce el encode real EXACTO. |
| `f950f6c` | **A2.2**: default 15→6% calibrado in-game (workScale 0.25, log del engine: el paneo del user mueve 10-12%/frame; el 15% dejaba pasar medio paneo). Todas las superficies (ABI, host default+ini, panel). |

## El arco diagnóstico (evidencia, no teoría)

1. Fantasma sobrevivió A1+A2+A2.1 con los resets disparando (89 resets en
   el log, flujo 226-501 px vs umbral 162).
2. **Split (ctl 28=500)**: fantasma cortado LIMPIO en la costura; la mitad
   del juego crudo siempre sana → lo imprime NUESTRO compose (reconfirma
   R83 con la mejor captura de la ronda).
3. **Resets continuos (ctl 12 ×0.35 s)**: el fantasma conmutó a POR
   DELANTE en paneo dcha→izda → con historia fresca el desplazamiento
   restante muestra el WARP de MVs del propio runtime (firma doble-motion,
   la vía R82k.4).
4. **Auto-reset off**: vuelve a POR DETRÁS → la dirección CONMUTA con el
   estado de la historia. Mapa completo: historia acumulada = detrás
   (historia 1 frame tarde mal re-warpada a 8 fps); historia fresca =
   delante (warp del mismo frame); juego crudo = limpio.

## Dónde queda el fantasma (honesto)

Las fixes de la ronda quitaron el error de curva, acotaron la puerta de
la croma y dieron al reset un disparador que funciona — el fantasma es
MENOR pero la copia persiste. El vehículo restante apunta al warp interno
de MVs del runtime vendor a 8 fps (su re-proyección llega un frame tarde)
y/o a las convenciones de entrega de MVs nuestras (R82k.4 ya cazó un
ahead-ghost en esa vía). B2 quedó RE-SCOPED con el user: es la fix del
error estático de resolución (calidad a ws<1), NO de la copia desplazada.

Orden siguiente: auditoría de convenciones MV contra el fork
(patrones primero), luego B2, luego re-test a ws 0.5 (parte por la mitad
el desplazamiento por frame por la vía limpia).

## Instrumentos que cargaron la ronda

- **Split** (ctl 28): la costura atraviesa el fantasma — la captura
  decisiva.
- **Log de reset de flujo** ("history reset (flow X px > Y)"): probó que
  el trigger dispara y midió el desplazamiento real del paneo para la
  calibración.
- **Pulsos de force-reset** (ctl 12 en bucle): el test del conmutador de
  dirección.
- **workScale** como palanca limpia de fps (la ley del user).

## Estado al apagado

Todo commiteado y desplegado (md5 5/5), ABI Tuning 112 B / CtlMsg 432 B.
ini: split 0, pace 0, workScale 0.25, flowReset 6, gainBound 2.0.
Recuperación post-apagado: md5 verificado 5/5, panel + launcher
relanzados.
