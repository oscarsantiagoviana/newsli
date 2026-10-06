> Design document for the new-sli panel (from analysis/03, reviewed and
> adopted). The panel ships as Dear ImGui (Win32+DX11 backend, vendored MIT)
> with a SINGLE ControlSpec table — no parallel HWND arrays (the POC's
> TRAMPA PANEL: every array had to grow together with NPARAMS or memory
> corrupted silently).

# 03 — Panel de control (coproc_panel.cpp): anatomía + rediseño UX para new-sli

Fuente: `poc-sr-offload-host/engine/coproc_panel.cpp` (681 líneas, v2.6), `engine/coproc_ctl_common.h` (protocolo ctl v2.5, single-sourced) y `host/offload_host.cpp` (`CtlPollThread` L453–604, `HotkeyThread` L639–712 spawn Ctrl+F9, `CtlLoadIni` L748+). El objetivo del nuevo proyecto (`new-sli`) es publicación: **código e UI en inglés**, sin dependencias externas.

---

## 1. Anatomía actual (lo que hay)

**Win32 raw de un solo archivo.** `wWinMain` registra la clase `coproc_panel`, crea ventana 700×800, un `WC_TABCONTROL` con 3 pestañas (General / Neural Rendering / Captura) y TODOS los controles como hijos directos de la ventana principal; `ShowTab()` (L321) muestra/oculta por `ShowWindow` según la pestaña. No hay diálogos ni recursos: todo posicionamiento a mano (x,y hardcodeados). Single-instance con mutex `Local\coproc_panel_once` + `FindWindow` (L500–512).

**Tab arrays paralelos (NPARAMS=16).** La UI se materializa con 4 arrays paralelos indexados por el mismo `i`:
- `PDEF[NPARAMS]` (L77–94): nombre, `minPos/maxPos/defPos`, `bias/scale` (value = (pos+bias)/scale) y `field` (id de campo ctl).
- `HINTS[NPARAMS]` (L97–113): una frase en español por parámetro, mostrada en la hint bar al tocar el control (WM_HSCROLL / click en Override).
- `OVR_INI[NPARAMS]` / `VAL_INI[NPARAMS]` (L196–204): claves ini por parámetro.
- HWND arrays `g_ovr/g_track/g_readout/g_plabel` (L66).

**TRAMPA PANEL (documentada, hay que heredarla):** TODOS esos arrays deben crecer JUNTOS con `NPARAMS`. El comentario de L59–61 lo recuerda: `g_chkToggle` llegó a 6 con post-SR (R70c) y los sliders a 15 con Work scale (R71); escribir fuera de array corrompe HWNDs vecinos (UB). No hay ningún mecanismo (static_assert, template, tabla única) que obligue: es disciplina manual. Un rediseño DEBE colapsar esto en una sola tabla de especificación de controles.

**Semántica de dos clases de parámetros (R72, clave para el rediseño):**
- *Overrides del juego* (Sharpness, MV.Scale, Jitter, Pre-exposure, Exposure.Scale, MV.Offset, Frame time): checkbox "Override" + slider deshabilitado si no está marcado; desmarcado ⇒ se envía `-1` ⇒ manda el valor del juego.
- *Knobs POC siempre-valor* (NR Intensity..Skin, Work scale, Boost — `field >= F_NRI`): no son overrides del juego, son ajustes del propio pase NR; SIEMPRE envían su valor (semántica fork `value_or_default`); el checkbox Override es ignorado/no-op en la práctica (la fila vive siempre). `ApplyLive` (L234–255) y `WriteIni` (L206–232) implementan esa regla con el mismo predicado `field >= F_NRI`.

**Combos:** GPU de offload (enumera adaptadores NVIDIA por DXGI `EnumNvidiaGpus` L119–140, sin crear device D3D; filtra la GPU del juego vía `mirrorGameLuidLo/Hi`; "Automática" = índice −1) y NR Style (0 Por defecto / 1 Alternativo / 2 Extra).

**Checkboxes fijos (g_chkToggle[6], índices mágicos 0,2,3,4,5):** Offload (id 10), Capture (12), ForceReset (13), NR ON (14), NR post-SR (15). El índice 1 quedó libre. La numeración no es contigua: los IDs de comando están a saltos y se mapean en un switch.

**Persistencia ini.** `LoadUi` (L270–319) lee `coproc.ini` junto al exe del panel (ojo: el host lo lee de SU directorio, junto a `nvngx.dll` — si el panel vive en otra carpeta, el Save avisa con el probe de `nvngx.dll`, L402–421). Claves espejo de ApplyLive: sin Override ⇒ valor −1 (así el host no resucita un override muerto). `defs[]` hardcoded para sliders fríos. Solo se carga AL ARRANCAR el panel: el ini del host se aplica al armar el offload.

**Apply en vivo vía ctl.** `HostSend` (L142–162) escribe verb/field/fval/seq con `InterlockedExchange`, espera ACK (eco de seq) hasta 2 s por campo con fail-fast si el heartbeat se congela. `ApplyLive` envía 7 toggles/combos + 16 params (~23 sends = hasta ~48 s peor caso si el host murió; típico instantáneo). Timer 1 s + `CheckHeartbeat` (L444–460): ● VIVO / ○ MUERTO (hb < 2 s). El botón Apply con host muerto hace WriteIni y avisa sin modal (R69u).
**Host side:** `CtlPollThread` poll 25 ms, aplica el campo al switch (campos NR bump `nrParamSeq` ⇒ rebuild del modelo ~1 s; WorkScale/GPU ⇒ `rearmRequested`; NR_ON=0 apaga compose ya; NR_STAGE nunca resetea texturas en caliente), reescribe TODO el mirror y hace `ack=seq`. `HotkeyThread`: F7 offload/native, F8 reset, **Ctrl+F9 busca la ventana `coproc_panel` y si no existe la lanza con `CreateProcessW` desde `HostDllDirW()`** (L660–688) — el panel se lanza desde dentro del juego, sin ruta configurable.

## 2. Patrones UX rotos

1. **Hints solo al tocar, y en una sola línea.** La explicación existe (HINTS) pero es invisible hasta que el usuario toca el control; en una pestaña con 16 sliders + 5 toggles, el usuario no sabe qué es "MV.Offset.X" hasta que lo clickea. Violación directa del requisito "cada parámetro explicado en cristiano": la explicación debería estar siempre visible (tooltip + descripción estática) o el layout no debería necesitarla.
2. **Mezcla de overrides del juego y knobs POC en la misma lista.** El checkbox Override aparece en TODAS las filas, pero en los knobs NR (field ≥ F_NRI) es mentira: siempre mandan valor. El usuario ve "Override" desmarcado en "NR Intensity" y cree que "manda el juego" cuando no es así. El modo disabled (EnableRow) también difiere: knobs siempre habilitados, overrides grises hasta marcar. La diferencia REAL de semántica es invisible.
3. **Sin feedback de qué manda realmente.** El mirror ctl existe (mirrorSharp, mirrorNrInt…) y el host lo reescribe tras cada set, pero el panel JAMÁS lo lee para mostrar "el juego está mandando 0.35 / tú estás mandando 0.80". No hay indicador de fuente (game vs panel), ni diff, ni estado persistido-vs-aplicado. La única señal global es el heartbeat VIVO/MUERTO.
4. **Pestaña Neural Rendering = 16 sliders idénticos + toggles apilados.** Los params de SR (Sharpness/Jitter/MV/Exposure, que son overrides del JUEGO) viven en la misma pestaña que los knobs del modelo NR. Conceptualmente son dos dominios distintos (corrección de inputs del SR vs ajuste del modelo NR); el usuario no puede descubrirlo desde el layout. Además los 10 primeros sliders están OCULTOS en General/Captura (ShowTab oculta los 16 siempre que tab≠NR, L343–349).
5. **Sin estados disabled informativos.** NR Style y los knobs NR son editables con NR OFF (no hacen nada hasta NR ON); Capture y Reset tienen explicación solo al click. No hay controles deshabilitados con razón textual ("requires Neural Rendering ON"), ni combos placeholder para features futuras.
6. **Ventana 700×800, hint bar y botones posicionados a mano.** La revisión de L643–644 (fila 15 de sliders llegaba a y=557 y pisaba los hints → ventana de 700 a 760 de alto) es evidencia de fragilidad: cada control nuevo obliga a recalcular Ys. Win32 raw sin layout = deuda creciente.
7. **IDs mágicos y arrays paralelos como contrato oculto.** chkToggle[6] con índices 0/2/3/4/5, IDs 10–15 a saltos, sliders 40+/50+, combos 300/301. El switch de OnCommand crece linealmente; añadir un control = tocar 6 sitios (TRAMPA PANEL).

## 3. PROPUESTA DE REDISEÑO COMPLETO (new-sli)

Código e UI **en inglés**. Principios: (a) una SOLA tabla de definición de controles (struct ControlSpec) de la que se generan HWNDs, ini keys, hints, rangos y campos ctl — mata la TRAMPA PANEL; (b) descripción SIEMPRE visible por control (label de ayuda bajo el control o panel lateral de ayuda), no solo on-hover; (c) semántica game-override vs engine-knob explícita en la UI; (d) nada de texto libre: solo sliders acotados, checkboxes y combos; (e) Apply en vivo + Save persistente, con el estado real reflejado desde el mirror.

### Pestaña 1 — Upscaling
| Control | Tipo | Rango | Default | Qué hace / qué se envía / desviarse del default |
|---|---|---|---|---|
| Upscaler backend | Combo | DLSS / FSR (planned) / XeSS (planned) | DLSS | Qué hace: selecciona el backend de reconstrucción. Se envía: nada en vivo — se lee al armar (config). FSR/XeSS deshabilitados con etiqueta "(planned)" hasta que existan. Desviarse: hoy sin efecto (solo DLSS implementado). |
| Offload to secondary GPU | Checkbox | on/off | on | Qué hace: cada frame se envía a la GPU secundaria para el SR; desmarcado = DLSS nativa del juego (modo A/B). Se envía: `CTL_FIELD_OFFLOAD` 1/0 en vivo, efectivo frame siguiente. Desviarse: off = pierdes el offload (y el NR, que depende de él); mid-Apply es seguro. |
| Offload GPU | Combo | Auto + NVIDIA enumeradas (menos la del juego) | Auto | Qué hace: elige en qué GPU corre el pase offload. Se envía: `GPU_INDEX` (−1 auto). Desviarse: GPU con poca VRAM o driver distinto ⇒ fallos de armar/eco (~2 s de re-arm por cambio en vivo). |
| Sharpness | Slider + Override | 0.00–1.00 (pos 0–100) | 0.50 (OVR off) | Qué hace: nitidez tras la reconstrucción. Se envía: `SHARPNESS` o −1 (manda el juego; RDR2 manda 0.35). Desviarse: alto = halos/borde duro; bajo = imagen pastosa. |
| MV Scale X / Y | 2 sliders + OVR | 0.00–2.00 (0–200) | 1.00 | Qué hace: multiplicador de los vectores de movimiento (H y V). Se envía: `MVSCALE_X/Y` o −1. Desviarse: ≠1 = natación o ghosting (Y fue el bug R18 de la natación). |
| Jitter X / Y (px) | 2 sliders + OVR | −1.00–1.00 (−100–100) | 0.00 | Qué hace: offset subpíxel del muestreo temporal. Se envía: `JITTER_X/Y` o −1. Desviarse: desajustarlo = temblor fino en bordes (el juego alterna ±0.24 por frame). |
| Pre-exposure | Slider + OVR | 0.05–8.00 (5–800) | 1.00 | Qué hace: exposición HDR previa; DLSS divide el color por esto antes de reconstruir. Se envía: `PREEXPOSURE` o −1. Desviarse: ≠1 = colores lavados/oscurecidos en HDR. |
| Exposure Scale | Slider + OVR | 0.25–4.00 (25–400) | 1.00 | Qué hace: escala de exposición de salida. Se envía: `EXPOSCALE` o −1 (el juego NO la manda; solo tiene efecto con Override). Desviarse: quema altas luces o apaga la imagen. |
| MV Offset X / Y (px) | 2 sliders + OVR | −2.00–2.00 (−200–200) | 0.00 | Qué hace: offset de cámara entre frames para juegos que compensan el jitter sumándolo al MV. Se envía: `MVOFFSET_X/Y` o −1. Desviarse: valor erróneo = ghosting direccional. |
| Frame time (ms) | Slider + OVR | 0.0–40.0 (0–400, /10) | 16.7 | Qué hace: delta de tiempo por frame para la historia temporal. Se envía: `FTD` o −1. Desviarse: RDR2 manda 0.0; dar un valor real puede mejorar la historia temporal, o introducir artefactos si miente. |

### Pestaña 2 — Frame Generation (placeholder)
| Control | Tipo | Rango | Default | Nota |
|---|---|---|---|---|
| Frame Generation | Combo | Off / DLSS-G (planned) / FSR-FG (planned) | Off | Placeholder deshabilitado con "(planned)". Qué hace: reservado para interpolación de frames en el offload. Se envía: nada (aún). Desviarse: sin efecto hoy. |
| (grupo) FG pipeline status | Static | — | — | Texto explicativo: "Frame Generation requiere entrega in-list fija; se diseñará tras el refactor del compose." |

### Pestaña 3 — Neural Rendering
| Control | Tipo | Rango | Default | Qué hace / qué se envía / desviarse |
|---|---|---|---|---|
| Neural Rendering | Checkbox | on/off | off | Qué hace: ejecuta el modelo NR (DLSS-NR) en la GPU de offload antes del upscale. Se envía: `NR_ON` 1/0; ON crea la feature (~1 s el primer frame), OFF apaga el compose al instante (R70g). Desviarse: OFF = no hay corrección neural (comparación base). |
| NR Style | Combo | 0 Default / 1 Alternate / 2 Extra | 0 | Qué hace: variante del modelo NR. Se envía: `NR_STYLE` (bump nrParamSeq ⇒ rebuild ~1 s). Desviarse: estilos alternativos sin garantía de mejora; solo experimentación. |
| Intensity | Slider (knob) | 0.00–2.00 | 1.00 | Qué hace: fuerza global del neural renderer. Se envía: `NR_INT` SIEMPRE (knob, no override). Desviarse: alto = sobre-suavizado plástico; bajo = apenas corrección. Rebuild ~1 s. |
| Local structure | Slider (knob) | 0.00–2.00 | 1.00 | Qué hace: detalle/estructura local. Se envía: `NR_LS` siempre. Desviarse: alto = texturas "dibujadas"; bajo = detalle plano. |
| Local tone | Slider (knob) | 0.00–2.00 | 1.00 | Qué hace: tono y contraste local. Se envía: `NR_LT` siempre. Desviarse: alto = HDR chapado; bajo = imagen plana. |
| Skin structure | Slider (knob) | −1.00–2.00 | 1.00 | Qué hace: detalle en piel; −1 = sigue a Local structure. Se envía: `NR_SS` siempre. Desviarse: alto = piel con textura falsa. |
| Work scale | Slider (knob) | 0.25–2.00 | 1.00 | Qué hace: resolución a la que corre el modelo NR (fork DlssNrWorkingScale). Se envía: `NR_WORKSCALE` siempre; cambio ⇒ re-arm del engine (~2 s). Desviarse: <1 = más FPS en GPU1, delta algo más suave; >1 = más coste. |
| Boost (delta ×N) | Slider (knob) | 0–16 | 1.0 | Qué hace: multiplica la corrección NR al componer. Se envía: `NR_BOOST` siempre, SIN re-arm (aplica al frame siguiente). Desviarse: sube a 8–10 para VER el efecto del NR en A/B; déjalo en 1.0 para calidad (1.0 = fork exacto). |

### Pestaña 4 — Debug & Verification
| Control | Tipo | Rango | Default | Qué hace / qué se envía / desviarse |
|---|---|---|---|---|
| Capture frames | Checkbox | on/off | off | Qué hace: vuelca frames (color/depth/MV/out) para análisis offline. Se envía: `CAPTURE` 1/0. Desviarse: genera GBs de dumps en la carpeta de captura. |
| Reset history every frame | Checkbox | on/off | off | Qué hace: descarta la historia temporal de DLSS cada frame (sin acumulación). Se envía: `FORCERESET` 1/0. Desviarse: muestra el aporte puro del SR por frame; en uso normal, OFF. |
| **Delta tint (NEW)** | Checkbox | on/off | off | Qué hace: pinta la corrección NR como tinte rojo/azul en pantalla — rojo = el delta añade detalle, azul = lo quita — para verificar a simple vista que el delta llega y su dirección. Se envía: `CTL_FIELD_DELTA_TINT` (nuevo, campo 26) 1/0 en vivo, sin re-arm. Desviarse: es solo visualización (no cambia la corrección); deja OFF para medir calidad. |
| Host status / heartbeat | Static | — | — | ● Live / ○ No host. Muestra además "Saved but not applied" cuando hay cambios persistidos sin Apply. |
| Event log | Static | — | — | Últimas N acciones ctl (campo, valor, ACK ok/fail) — feedback de qué se envió de verdad. |

### Pestaña 5 — About
Versión del panel/protocolo (magic v2.5 → v3), build, enlace al repo, crédito, licencia. Sin controles.

**Cambios estructurales vs el panel actual:** desaparece el toggle **nrStage (pre/post-SR)** — el camino pre-SR se elimina del engine (muere); desaparece el **badge**; los combos de upscaler alternativo (FSR/XeSS) y FG aparecen deshabilitados con "(planned)". Los knobs NR pierden el checkbox Override (eran no-op): filas siempre activas con su valor real, y los overrides del juego muestran "Game: <value>" leído del mirror cuando están sin Override.

### Arquitectura técnica del panel (recomendación)

**Opción A — Win32 puro C++ (como hoy).** Ventajas: cero dependencias, un .exe de ~100 KB, compilable con cualquier MSVC/MinGW, sin runtime que distribuir, ctl ya funciona así. Inconvenientes: layout manual (la fragilidad y=557 demostrada), sin DPI scaling decente por defecto, verbosidad extrema (681 líneas para 3 pestañas), cada control nuevo = 6 puntos de edición.

**Opción B — Dear ImGui (backend Win32+DX11).** Ventajas: layout inmediato (sin Ys a mano, sin arrays paralelos: la tabla ControlSpec se recorre con un for), styles dark theme tipo OptiScaler gratuitos, tooltips/descriptions nativos, retarget futuro (mismo código UI corre sobre Vulkan/GL para una UI in-game OSD), iteración rapidísima. Inconvenientes: añade dependencia header-only + stub DX11 (la app debe crear un device D3D11 solo para la UI, ~150 líneas), binario ~x2, y hay que mantener el backend actualizado; para publicación implica vendorizar ImGui en el repo (aceptable: header-only, MIT).

**Recomendación: Opción B (Dear ImGui).** El rediseño exige descripciones siempre-visibles, combos disabled con "planned", feedback del mirror y un event log — todo lo cual es manual y frágil en Win32 raw y trivial en ImGui. La dependencia es header-only MIT vendorizada, se publica con el repo, y el coste (device DX11 + ~200 líneas de bootstrap) se paga una vez. Si la restricción "sin dependencias" fuera dura (solo 1 exe, sin vendor), la A sigue siendo válida pero entonces HAY QUE construir la tabla ControlSpec única y un mini-layout vertical automático (stacking) para no volver a la trampa de las Ys.

## 4. Protocolo ctl v2 → v3 (propuesto)

Problemas de v2.5: struct fijo 128 B sin sitio (campo 25 usado, queda 1), mirror incompleto (faltan NR_WORKSCALE/BOOST/STAGE y los knobs NR no se reflejan), sin versionado negociable (magic único = hard fail), sin descubrimiento (nombre fijo `Local\coproc_ctl_v1`), Apply granular de 1 campo por mensaje (lento con host caído).

**v3 propuesto:**
- **Versionado:** `magic` = `0xC070C0C5` + `version` u16 (empieza en 3) + `abi` u16. Host escribe magic+version; panel acepta si version ≥ la suya y lee los campos que conoce (forward-compat: campos nuevos al final, nunca reordenar). Magic distinto = protocolo incompatible ⇒ panel muestra "host protocol vN, panel supports v3+" y se vuelve read-only.
- **Descubrimiento:** el host crea `Local\newslt_ctl_v3` y además publica un evento `Local\newslt_ctl_ready` (Auto-reset) que el panel espera para no mapear a ciegas; el nombre lleva versión para coexistir con hosts viejos. El exe del panel se localiza vía `HostDllDirW()` como hoy (sin ruta configurable) o flag `--host-dir`.
- **Mensaje:** header fijo `{magic,version,abi,verb,field,fval,seq,ack,hb}` + arrays por índice de campo (tabla de campos compartida `ctl_fields_v3.h` con static_assert de offsets): verb 4 = **set-batch** (hasta 8 pares field/value por mensaje ⇒ Apply completo en 1–3 mensajes en vez de 23), verb 5 = **subscribe** (el host hace push del mirror al panel en cada cambio, incluidos los que hacen F7/F8, para que el panel refleje el estado real siempre). Campos nuevos: `CTL_FIELD_DELTA_TINT = 26` (verificación visual rojo/azul), 27–31 reservados (FG, FSR/XeSS). El mirror pasa a ser un array `mirror[NFIELDS]` indexado por campo (adiós mirrorXyz sueltos) + `mirrorGameLuidLo/Hi` + `mirrorFlags` (bit por "quién manda": game vs panel) que el panel muestra por control.
- **Migración:** mantener `coproc_ctl_common.h` como única fuente (single-source que ya evitó el copy-rot v2.1→v2.5), bump del static_assert a 256 B, y periodo de coexistencia: host v3 que también crea el mapping v1 legado si `COPROC_COMPAT_V1=1`.
