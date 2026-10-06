# 42 — R92: el compose mataba el veredicto del modelo (re-anchor de R82j con las DOS ramas idénticas)

Fecha: 2026-10-04 (tarde). Síntoma user: "no aprecio cambio entre aplicar NR y
no aplicarlo incluso forzando en el panel todo al máximo; en la vista NR veo
detalles que luego no se pintan en el frame final". El mismo síntoma del cierre
del golpe 4 (worklog 40), ahora como sospecha principal y no veredicto.

## Causa raíz (algebraica, probada)

`nr_delta.hlsl` R82j (`8657cec`) portó el "luma re-anchor" del fork
NeuRotic (dlssnr.hlsl:928-963) COMPARANDO frame-vs-MODELO en ambas ramas:

```
ratio = (origLuma <= modelLuma) ? origLuma/modelLuma
                                : (modelLuma + max(0, origLuma-modelLuma))/modelLuma
upgraded = modelDisp * ratio
```

En el fork la primera rama compara `originalLuma` contra **proxyLuma** (la
luma del proxy, no la del modelo) y solo la de ARRIBA lleva el headroom; el
veredicto de luma del MODELO entra POR LA IMAGEN y se mide DESPUÉS en
`lumaRatio`. El port usa `modelLuma` en las dos ramas ⇒ con la forma
arriba: `ratio = origLuma/modelLuma` EXACTO, y `luma(upgraded) = modelLuma ×
origLuma/modelLuma = origLuma` también en la rama de abajo ⇒
`lumaRatio = (luma(upgraded)+k)/(origLuma+k) ≡ 1` SIEMPRE, en todo píxel,
para cualquier respuesta del modelo. Numería (proxy 0.5, modelo 0.3..0.7):
lumaRatio = 1.000000 en los cinco casos. aguante→floor→guard→chroma operan
sobre un ratio idénticamente 1: el frame final nunca vio el veredicto, con
detail 4 / boost 4 / bound 4 incluidos (todo colapsa a exactamente 1).
La vista NR pinta `modelDisp` CRUDO (payload opaco, sin gain): por eso
"la vista tiene detalles que el frame no recibe".

Por qué duró: el A/B del feeder se juzgaba con TINT (payload vista) y los
guards del compose daban medias plausibles; la rama rota es invisible salvo
que midas el GAIN de calidad contra la respuesta del modelo.

## Fix (estructural, fork-parity real)

`nr_delta.hlsl` (una sola pieza): el re-anchor de ambas ramas se elimina —
`upgraded = modelDisp` (con su empty-frame guard intacto). En NUESTRO
transporte `proxyLuma == origLuma` POR CONSTRUCCIÓN (el espacio del compose
ES el encode y el base crudo `o` monta la multiplicación), así que el
headroom del fork es idénticamente cero aquí y la paridad real es NO
re-anclar. El veredicto de luma del modelo vive ahora donde el fork lo mide:
en `lumaRatio` (aguante 1/512 + amplificación boost + bound per-pixel).
Identidad intacta: modelDisp==enc ⇒ upgraded==enc ⇒ gain≡1 bit-exacto.
Jitter: fuera de alcance (se repara en otra sesión).

## Validación

- Build limpio 44/44 (dxc recompiló nr_delta, paso [40/44]), ctest 3/3.
- Deploy de los 5 al gamedir (juego cerrado; md5 OK en los cinco; engine
  gamedir 14:24 == deploy 14:23).
- Feeder offline (capturas RDR2 1505×635, engine del GAMEDIR recién
  desplegado — sin trampa de binario viejo):
  - Brazo calidad: `delta mean +0.81038, nonzero 100%` en 15/15 frames —
    el gain de calidad POR FIN DIFIERE de 1 (pre-fix: ≡1.0 en luma por la
    prueba algebraica).
  - Brazo control NR_OFF: `mean +1.00000` exacto — identidad bit-exacta
    intacta (la red anti-runaway respira sobre 1, no sobre 0).
- Pendiente LIVE (user): A/B NR on/off en partida + split. El frame final
  debe mostrar por primera vez desde R82j lo que la vista ya enseñaba.

## Estado

- Sin commitear: R91b (nr_vendor.h, log.cpp, de otra sesión) + este fix
  (nr_delta.hlsl, build_r92.bat, worklog 42).
- Config del user en el ini: la "ideal" del golpe 4 (colour 0.25, ws 0.5);
  sus sliders a máximo fueron de la sesión de prueba, no persistieron si
  no dio Apply (R89f aplica+guarda en el mismo click).
