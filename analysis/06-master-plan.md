# 06 — Plan maestro de refactorización: poc-sr-offload-host → new-sli

Estado: PROPUESTA para revisión conjunta (no se ha escrito código). Fuentes: 01–05 de este directorio (análisis línea a línea completos).

## 0. Punto de partida (los números)

| | Actual (2 proyectos) | new-sli propuesto |
|---|---|---|
| Host | 4.896 LOC (offload_host 2.755 + nvngx_host 734 + dxgi 194 + load_hook 162 + headers) | ~2.900–3.060 LOC en 10 módulos |
| Engine | 4.356 LOC (14 ficheros, ~40% muerto: archspoof, fh86, replay roto, params_block, badge, dumps) | ~1.600 LOC en 7 módulos (−63%) |
| Panel | 681 LOC Win32 raw (arrays paralelos, TRAMPA PANEL) | ~900–1.100 LOC (ImGui, tabla ControlSpec única) |
| Wiki | 16 ficheros ES (2 proyectos) | 18 ficheros EN + worklog histórico |
| Total código | ~10.100 LOC vivos + ~2.700 muertos | ~6.500–7.500 LOC, 0 muertos, comentado EN |

## 1. Principios (fijados por el user, no negociables)

1. **C++ puro** en todo lo que se publica (host DLL, engine EXE, panel EXE, tools CLI). Los scripts Python de análisis quedan fuera del repo público (dev-only) o se reescriben como tools C++ no críticos.
2. **Código y wiki en inglés**, orientado a publicación. Comentarios explicando el POR QUÉ de cada regla D3D12 (están todas en 01 §4 y 02 §5).
3. **Sin pre-SR NR**: el engine entrega SOLO delta display-domain. El frame que consume el SR es SIEMPRE el original del juego.
4. **Sin badge/marca**: la verificación visual es el **delta-tint** (rojo=+/azul=−) como flag de verificación, portado a knob ABI (hoy es un archivo `coproc_compose_test.bin` — defecto conocido).
5. **Sin código muerto**: regla no-legacy mantenida por diseño (POC heredada).
6. **Verdad arquitectónica R73c documentada**: el compose post-SR NO se presenta en RDR2 (el juego copia el out del DLSS antes del present). El NR visible en RDR2 no existirá tras el refactor — verificación en RDR2 = contadores + dumps + tint en juegos que SÍ presentan el out (p.ej. Cities Skylines II). Esto va en README y wiki 17.
7. **Extensibilidad planteada, no implementada**: `IBackendSR` (FSR2/XeSS después), placeholder de Frame Generation (fase 2, requiere capa swapchain — ver 05 §2).

## 2. Arquitectura propuesta

```
new-sli/
├── README.md                     # qué es, cómo funciona, restricciones por juego (R73c), disclaimer
├── LICENSE                       # (decisión pendiente — ver §6.D)
├── CMakeLists.txt                # build público y portable (MSVC + Windows SDK + dxc)
├── src/
│   ├── shared/                   # single-source ABI: abi.h (scalars/handshake/tuning),
│   │                             #   ctl_common.h v3, log.h (rotación), crash.h (minidump host+engine)
│   ├── host/                     # = nvngx.dll (proxy NGX) + dxgi.dll (vector de carga)
│   │   ├── ngx_proxy.cpp         # exports NGX, core loader (LdrLoadDll+registro), DoInit cache anti-retry
│   │   ├── queue_observer.cpp    # QTrack: vtable ECL patch, NoteEvaluateList, OnListSubmitted
│   │   ├── load_vector.cpp       # dxgi forward + detours load-hook (solo basename nvngx.dll)
│   │   ├── transport.cpp         # SharedBuf cross-adapter, fences, mapping, format utils
│   │   ├── session.cpp           # arm/spawn/teardown/re-arm/recycle — MÁQUINA DE ESTADOS única
│   │   ├── seal.cpp              # sello color in-list, depth clone DSV, guías MV en lista propia
│   │   ├── compose.cpp           # compose post-SR mínimo: delta + boost + delta-tint (sin badge/damero)
│   │   └── ctl.cpp               # protocolo panel v3, hotkeys, ini
│   ├── engine/                   # = sli_engine.exe (worker GPU1, NR-only, salida = delta)
│   │   ├── main.cpp              # args mínimos, PickAdapter por LUID, ready magic
│   │   ├── loop.cpp              # bucle produce/done por segmentos, eco GPU unificado
│   │   └── nr_vendor.h           # runtime NGX feature 18 in-process (recortado, sin dumps/stubs)
│   ├── panel/                    # = sli_panel.exe (ImGui Win32+DX11, vendorizado)
│   │   └── panel.cpp             # tabla ControlSpec ÚNICA → tabs/controles/hints/ini (mata TRAMPA PANEL)
│   └── tools/
│       └── ctl.cpp               # CLI setter genérico field/value (generaliza ctl_set_nrstage)
├── shaders/
│   ├── nr_encode.hlsl            # CSMain encode (BGRA8, Reinhard/2.2, coverage box workScale)
│   ├── nr_delta.hlsl             # CSDecodeDelta + delta-tint portado (flag por cbuffer)
│   └── nr_compose.hlsl           # compose acumulador ping-pong hist + tint (sin badge)
├── external/                     # imgui/ (MIT), detours (MIT) — vendored con sus LICENSE
└── wiki/                         # 18 páginas EN (ver 04b) — worklog histórico = 05-historical-worklog.md
```

### Decisiones de diseño críticas

- **D1 — El engine solo produce delta.** `CSDecodeDelta` ([-1,1] display-domain, guard de saturación, resample work→render) es la ÚNICA salida. Muere `CSDecode` frame-completo, la delay-line pre-SR, texNrColor, anillos lag×8, coherent sets (~600 LOC host + camino engine). El eco de fallback es GPU (`EchoToOut`→bufOut delta=0), un solo helper.
- **D2 — Delta-tint estructural.** Portado a `CSDecodeDelta` y al compose; se activa por **knob ABI/ctl** (campo 26) en vivo, sin archivo. Ganancia ×32 HDR para atravesar el post-process del juego. Es EL mecanismo de verificación (reemplaza badge y damero).
- **D3 — Máquina de estados de sesión.** Hoy hay 4 vías de re-arm parcial (gpuIndex, workScale, DRS, guías tardías) = el mayor riesgo estructural (R70d: reset en caliente = device lost). new-sli: una FSM `Idle→Arming→Live→Draining→Teardown→Cooldown` con una única cola de peticiones de re-config; los recursos se recrean SOLO en transición Arming.
- **D4 — IBackendSR (plantado, no implementado).** `struct IBackendSR { Init(device, params); Evaluate(cmdList, params); Shutdown(); }` + factory por enum + fallback = passthrough DLSS nativo (que ya existe). FSR2/XeSS son fase posterior (patrón OptiScaler en 05 §4: DLLs públicas del usuario + GetProcAddress, contexto creado en el device de GPU1 — el offload multi-GPU es valor propio, OptiScaler no lo tiene).
- **D5 — Panel.** Dear ImGui (Win32+DX11, MIT, vendored) sobre Win32 raw: layout inmediato mata la TRAMPA PANEL (tabla ControlSpec única), descripciones SIEMPRE visibles, combos "(planned)" para FSR/XeSS/FG, event log, mirror con "quién manda" (game vs panel). Diseño completo de tabs/controles/explicaciones: ver 03 §3.
- **D6 — ctl v3.** Versionado (magic+version+abi), descubrimiento (`Local\sli_ctl_v3` + evento ready), verbs: get/set/reset/**set-batch**/**subscribe** (push del mirror), mirror array por campo + flags de fuente. Detalle: 03 §4.
- **D7 — Robustez de publicación.** Crash handler + minidump en el HOST (hoy solo engine), log con rotación por tamaño, budget de recycle configurable (leak vendor ~10 MB/s documentado), audit del comentario podrido de MakeSharedBuf (01 §3.4) antes de copiar, `GetDeviceRemovedReason()` tras init core.
- **D8 — Se conserva TAL CUAL** (reglas duramente ganadas, 01 §4 + 02 §5): in-list seal + produce GPU-ordered, lista nueva→Close antes de Reset, guías en lista propia (R69q), clon DSV neto-cero (R69t), FootprintFmt/TYPELESS, MipLevels=1, UAV sin sampler, cross-adapter heaps 64KB, never-block + fallback nativo, ABI single-source con static_assert, lectura 3-pasos de params, disciplina de segmentos, catch-all con eco, poison fence (con handshake de cierre explícito añadido — deuda 01 §3.2).
- **D9 — GRANO (ver 07-grain-analysis.md)**: el acumulador temporal fork-exact (reproyección MV + blend 0.08 + invalidación) va DONDE el delta se aplica, en el camino presentado — en la POC el camino visible (pre-SR) aplicaba el delta CRUDO (+0.02 a boost 1, +0.51 a boost 6, escala lineal con boost) mientras el acumulador vivía solo en el camino muerto post-SR. Boost = solo A/B (amplifica señal y ruido por igual). El delta-tint lee el hist acumulado. Con D1 (delta-only) el grano propio desaparece de RDR2 por construcción y el NR futuro nace con acumulador.

## 3. Qué se ELIMINA (inventario completo, recuperable de git)

**Host**: camino pre-SR entero (DeliverPendingInList 1775–1810, entrega 2543–2581, sustitución params 2621–2687, texNrColor, anillos lag, nrStage, nrActive) · badge 512 + damero cTest==1 · DumpTexDeltaDump + diags 'delta/compose/lag diag' · flag-files de iteración (sealguide.on, compose_arrival.bin → pasa a config) · EnvOn, StepFence, WriteGpuBytesToDisk, FillTuples+COPROC_KEYS, inBias/inExp, depthClone, pendingOut, nr_readdepth_cso.h · psapi.h desordenado (recycle se queda, include arriba).
**Engine**: nr_archspoof.h, fh86.h (muertos verificados, 0 includes) · replay.cpp (no compila) · params_block.h · badge 512 (3 sitios) · dumps frame 240 · stubs A/B · camino eco CPU + badge upload · args legacy SR (~20) · tuplas · nr_guide_depth + nr_bridge_cso · psoDec frame-completo.
**Panel**: nrStage toggle, checkbox Override en knobs NR (mentiroso), arrays paralelos (sustituidos por ControlSpec).
**Tooling Python** de análisis: fuera del repo público.

## 4. Fases de ejecución (cada una con criterio de salida verificable)

| Fase | Contenido | Criterio de salida |
|---|---|---|
| **F0 — Esqueleto** | Repo + CMake + LICENSE + wiki skeleton (18 páginas EN, índices) + vendored imgui/detours | Build vacío verde; `git init` público |
| **F1 — shared + engine** | abi.h, ctl v3, log/crash; engine delta-only (main/loop/nr_vendor recortado); shaders encode/delta(+tint) | Engine compila; validación offline: alimentar capturas reales y verificar delta + tint en dump |
| **F2 — host núcleo** | ngx_proxy, load_vector, queue_observer, transport, session (FSM), seal, compose mínimo | RDR2 en vivo: arm → engine Live → composes contando → delta-vía viva (counters + dump); SR nativo intocado (A/B bit-exacto con offload off) |
| **F3 — panel v3** | panel ImGui con ControlSpec + explicaciones por control; hotkeys; ini | A/B en vivo de cada control desde panel; explicaciones visibles; sin arrays paralelos |
| **F4 — publish pass** | Comentarios EN completos, README (con R73c y disclaimer no-afiliación), wiki completa (traducir 01–15 + nuevas 16–18), purga de rutas personales/binarios, licencias de terceros | Revisión completa; repo listo para público |
| **F5 — backends (post-publicación)** | FSR2 vía ffx pública → XeSS → FG (capa swapchain, fase propia) | A/B DLSS↔FSR2 en juego |

## 5. Riesgos y mitigaciones

1. **RDR2 sin NR visible tras el refactor** (consecuencia de eliminar pre-SR): mitigación = verificación por counters/dumps en RDR2 + delta-tint en juego presentador (CSI2) para lo visual. Es una decisión ya tomada; se documenta, no se negocia en F1–F4.
2. **Leak vendor ~10 MB/s** sigue: recycle con budget configurable + debounce de rebuild de knobs (cada rebuild crea+fuga; los sliders CREATE-time disparan rebuild).
3. **vtable ECL slot 10** sin fallback: añadir verificación de firma del vtable en debug + documentar.
4. **GPL por copiar OptiScaler**: NO se copia código suyo (patrón re-implementado); mantener auditoría de procedencia en F4.
5. **Nombre "SLI"** es marca registrada de NVIDIA para un público gamer — recomiendo alias público neutro en F4 (decisión del user).

## 6. Decisiones que te pido (para arrancar F0)

- **A. Panel**: ImGui (recomendado, 03 §3) vs Win32 puro.
- **B. Build**: CMake (recomendado, estándar público) vs .bat simple.
- **C. Alcance F1**: ¿engine primero validado offline con capturas (recomendado), o directo a F2 en juego?
- **D. Licencia**: MIT (recomendado si no se copia código OptiScaler — nuestro código es original) vs GPL-3.0.
- **E. Nombre público** del repo (new-sli es interno; SLI es marca NVIDIA).
