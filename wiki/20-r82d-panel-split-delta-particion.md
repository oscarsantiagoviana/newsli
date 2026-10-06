# 20 — R82d: panel (split fix, delta view, partición)

Ronda 2026-09-30 → commits `8cb0ea3` (split+tint) y `254caaa` (partición).

## Split A/B: por qué "se deshabilitaba de inmediato"
El checkbox del split era **lector pasivo**: escribía un `on` local y jamás
`g_st.value[i]`; el mirror del host (aún 0) revertía el tick en el poll
siguiente de 500 ms. **Todo control debe ser writer de primera clase del
estado** (`g_st.value[i]` al tick). El seam se recuerda mientras está off
(`static int seamPos`, R82d).
Compón con: checkbox on/off + slider de posición (1-1000 permille, nunca
hay que arrastrar a 0 para apagar).

## Delta view (tint) — qué ve cada modo
- **tint=1 Raw**: la edición del frame pintada por signo (rojo +, azul −).
  Es 90 % grano de inferencia; solo sirve para verificar que la cadena vive.
- **tint=2 Structural**: el **engine** mantiene un EMA del gain en un UAV
  persistente (`gStructEma`, slot 7, escrito por el MISMO hilo del CS de
  decode — race-free) y pinta `gain-1` con el grano promediado: queda lo
  pegado a las formas = lo que NR aporta de verdad. Estable, shape-following.
- El draw del gate usa **PSO aditivo cuando tint≠0** (un multiply aplastaría
  el pintado); multiply solo para tint=0 (calidad).
- EnsureStruct + struct_accum.hlsl + el heap de 3 descriptors del host:
  ELIMINADOS (POC sin código muerto). La vista estructural vive íntegra en
  el engine (`nr_delta.hlsl`, 223 L).

## Nota "NR" del panel: inferencia ≠ aplicación
El modelo evalúa sobre el frame de render **pre-upscale** (input del DLSS,
con guías), pero la corrección se compone **post-upscale y post-tonemap**
en el Present (R73c: pre-tonemap imposible en RDR2). La nota antigua
("before the upscale") describía el diseño muerto del POC.

## Partición del panel (1575 L → 5 TUs)
`panel_internal.h` (tabla única + tipos + contratos) / `panel_state.cpp`
(snapshots value/applied/saved + ring de eventos + ini) / `panel_host.cpp`
(ctl: open/poll/apply + EnumGpus) / `panel_ui.cpp` (rows+help+tabs+header)
/ `panel.cpp` (bootstrap Win32/DX11 + loop). Mecánico, sin cambios de
lógica; la tabla con sus static_asserts sigue siendo EL contrato.

### Trampas de la partición (cazadas por el build)
- `using namespace sli` y `#include "shared/log.h"` van en el header
  interno (el panel.cpp viejo los traía él solo); sin ellos: C2065
  CTL_FIELD_* y C2039 sli::Log.
- `kLogCap` en UN sitio (header); redeclarado en el .cpp no compila.
- `<cstdlib>` para `_wtoi/_wtof`.
- snprintf de un FORMATO (para ImGui) necesita `%%` escapado; un `%.2f`
  suelto sin varargs es UB silencioso.
- `kNumControls` evaluando 0 dentro de un struct = C2229 cascada; mirar el
  PRIMER error del log, no el tail.
- El smoke del panel requiere que el del user esté cerrado (mutex de
  instancia única); deploy con copyfile (copy2 preserva mtime = deploys
  "rancios").

### Trampa RS (cazada en live): NumDescriptorRanges < array
Al añadir un recurso (u2 struct EMA) se actualizó el array `uav[3]` y el
heap, pero NO `NumDescriptorRanges` (quedó 2): la RS miente al runtime,
`CreateComputePipelineState` del decode falla y el fail-soft lo tapa como
"init FAILED - NR falls back to zero-delta" — el juego sigue funcionando
con delta CERO y solo el log del engine del gamedir lo delata. **Regla:
recurso nuevo = array + heap + views + RANGE COUNT, los cuatro juntos.**
Verificación barata sin juego: replay_feeder (CONTENT 5/5, res=1).

### Trampa live #2 (fix2): getter de tint de la era anterior
`tuningTint()` colapsaba `nrTint 2 -> 0` (resto R82c): el host elegía PSO
ADITIVO para tint=2 pero el CS pintaba el GAIN (~1.0) → el gate SUMABA
gain ≈ 1 por píxel → **pantalla en blanco**. Regla: los DOS modos de
pintura (1 raw, 2 structural) deben llegar al decode CS; valor
desconocido degrada a raw, NUNCA a gain, porque el PSO lo elige el host
por `tint != 0`. Verificado por feeder con `SLI_FEEDER_TINT=1/2` (media
+2.5 = pintura; gain sería ~0.9). El engine zombi se mata y el exe se
copia en el hueco del respawn: el host relanza desde el binario nuevo
SIN cerrar el juego.

## R82e — el probe que destapo la cadena (feeder como instrumento)
El "gain estructurado" de R82-R82d era un ARTEFACTO: la salida del modelo
que llega al decode es SINTETICA (rampa lineal sobre input casi negro;
verificado con probe in-shader: slots alternos OUT/HDR del mismo pixel).
Toda la normalizacion por mediana fue NO-OP desde R82c — el descriptor
u1 apuntaba al slot 11 de un heap de 8 (offset RELATIVO A LA TABLA, no al
heap; fail-soft silencioso). Fixes: u1=+1, dNormRGB float3 por canal
(el ancla luma-only dejaba pasar el sesgo azul B 0.85), tiles uint2 f16
RGB, RS 12 consts (el count corto leia ceros -> gain x2), chromaTaper
0.08, grid de tiles (w+7)/8 (el >>3 desalineaba el readback una columna
por fila), gate antifabricacion fk=0.06 (sintesis neutralizada donde no
hay senal). Resultado: gain med 1.000/1.000/1.000 EXACTO, A/B visual
continuo sin seam (vision), oscuras naturales sin patron.
PROBE (tecnica): el stash in-shader (ultimos 32 slots del buffer de tiles
+ marcador DEADBEEF) es el canal de diagnostico que funciona — los
CopyTextureRegion extra en/despues del evaluate del runtime ROMPEN el
launch (NvAPI -1) o el close (0x80070057). El feeder + este stash = kit
de verificacion offline sin tocar el frame.
Trampa HLSL: uint4 o = Tex.Load() TRUNCA display [0,1] a 0 — float4.

## Estado tras la ronda
`nrTint` es BINARIO: 0 = calidad (gain multiplicativo), 1 = **NR output
view** — la pantalla muestra la SALIDA PURA del modelo (post mediana 3×3)
con un draw OPAQUE (Src=ONE, Dest=ZERO: el payload REEMPLAZA el frame, sin
mezclas ni dominios raros). Con el split activo, la mitad NR se recorta
con SCISSOR en el host (la vista llega entera al seam; el juego sin tocar
a la derecha). Las pinturas por signo (raw + EMA estructural, u2, ×32/k6)
están ELIMINADAS del árbol — el tint rojo/azul resultó inútil: un sesgo
global −2% satura la pantalla entera de azul y las sutilezas no se juzgan
a ojo. La calibración de calidad (tiles de luma) sigue escribiéndose en
modo vista (un solo store con payload ternario, sin returns tempranos).
Feeder: gain mean 0.89 / 100% px vs view mean 0.17 / 47% px.

## Estado tras la ronda
Smoke panel PASS (vivo 3 s, sin crash); deploy gamedir md5 `1cb6f825f3`.
Engine R82d y nvngx.dll desplegados (ronda anterior). PENDIENTE live:
split checkbox + tint 1/2 con juego real; boost restaurado a 1.0;
HoldFrame; F6 backends.
