# 08 — Plan de EJECUCIÓN detallado (F0→F5) con protocolo de calidad

Complementa a `06-master-plan.md` (arquitectura y decisiones D1–D9). Este documento define CÓMO se escribe el código: el protocolo de verificación multi-pasada, la matriz de contraste con los proyectos referencia, y el detalle paso a paso de cada fase con sus gates. Regla rectora del user: **ser detallista y muy cuidadoso con el código, comparándolo y analizándolo varias veces, consultando los proyectos referencia para contrastar**.

## 0. Decisiones adoptadas por defecto (reversibles — objeción del user en cualquier momento)

| # | Decisión | Default | Alternativa |
|---|---|---|---|
| A | Panel | **Dear ImGui** (Win32+DX11, vendored MIT) | Win32 puro |
| B | Build | **CMake** (MSVC + Windows SDK + dxc) | .bat |
| C | Orden validación | **Engine primero, offline con capturas reales** → luego host en vivo | directo a juego |
| D | Licencia | **MIT** (código 100% original, patrón re-implementado, sin copia GPL) | GPL-3.0 |
| E | Nombre público | **PENDIENTE** (candidatos: `crossload`, `duoscale`, `offscale`; working name `new-sli` hasta decisión — el rename es mecánico) | — |

## 1. Protocolo de calidad global (aplica a CADA módulo, sin excepciones)

### 1.1 Las cinco pasadas (orden fijo)

1. **P1 — Puente con trazabilidad**: portar/escribir el módulo desde el código POC citando en `TRACEABILITY.md` (fichero de trabajo, NO público) la correspondencia función-nueva → líneas POC de origen (`01`/`02` la traen). Toda divergencia intencional se anota con motivo.
2. **P2 — Contraste con referencia**: para cada mecanismo del módulo, leer la implementación de referencia (matriz §1.3) y diff de SEMÁNTICA (no de estilo): firma, orden de operaciones, estados de recursos, manejo de errores. Toda convención nuestra ausente en la referencia = bug latente (regla de oro R69v).
3. **P3 — Checklist D3D12**: pasar la lista §1.2 literalmente sobre el diff. Cada regla se verifica contra el código NUEVO (no contra lo que "iba a hacer").
4. **P4 — Revisión de ojos frescos**: un revisor independiente (subagente) lee el módulo completo SIN el contexto del puerto, buscando: código muerto, comentarios que no matchean el código (lección del comentario podrido de MakeSharedBuf), invariantes rotos (lock ordering, null-checks), y deudas de la POC arrastradas sin querer. Sus hallazgos se corrigen antes de continuar.
5. **P5 — Build + test**: compilar con warnings-as-errors (`/W4 /WX` + `/analyze`), ejecutar el harness offline del módulo y los gates de la fase. Solo entonces commit.

### 1.2 Checklist D3D12 (verificar en P3; fuente: análisis 01 §4 + 02 §5)

- [ ] Lista recién creada → `Close()` antes de `Reset()` (nace en recording).
- [ ] Reset por BLOQUE, no por frame; el consumidor (SR/backend) siempre recibe lista fresca.
- [ ] NUNCA barriers/copies sobre texturas del juego dentro de SU lista → lista propia sellada tras el submit del frame.
- [ ] Depth DSV no es copiable (spec): clon idéntico fmt+flags con transiciones neto-cero.
- [ ] Footprints/copies: TYPELESS y SINT/UINT prohibidos → miembro FLOAT de la familia; copy-target TYPELESS hermano del mismo bit-width.
- [ ] SRV con desc explícita: `MipLevels=1` SIEMPRE (0 = device removed diferido).
- [ ] UAV estructurado: `Format=UNKNOWN` + `StructureByteStride=N`. CS sin SamplerState en el RS.
- [ ] Al tocar consts del CB → actualizar `Num32BitValues` del root signature (trampa R73b).
- [ ] Cross-adapter: heap SHARED|SHARED_CROSS_ADAPTER align 64KB + placed buffer ALLOW_CROSS_ADAPTER + CreateSharedHandle NT; fences shared cross-adapter.
- [ ] Jamás bloquear el thread del juego; hooks de submit SIN el lock de sesión.
- [ ] Jamás `Reset()` de recursos posiblemente en vuelo (reconfig solo en transición Arming de la FSM).
- [ ] Estados UAV↔COPY explícitos tras ExecuteCommandLists (no decaen).
- [ ] Lectura de params NGX: 3-pasos typed→untyped; subrects y dims por-frame (nada hardcodeado).
- [ ] `GetDeviceRemovedReason()` tras init del core NGX (zombie detection); DRR diferido: atribuir con repro minimal, no por la llamada que falla.
- [ ] Swap de history por COPIA (jamás reescribir heap shader-visible en vuelo).
- [ ] SRV jamás apunta a la textura que el UAV del mismo dispatch escribe (hist ping-pong: leer [1], escribir [0]).
- [ : both sides de CopyTextureRegion: jamás footprint-placed en ambos lados; tamaño = pitch alineado × h, nunca w×bpp×h.
- [ ] Struct C++ multilinea: reescribir el bloque completo, no patch parcial (C2062).

### 1.3 Matriz de contraste mecanismo → referencia (P2)

| Mecanismo (new-sli) | Referencia a leer y contrastar | Qué verificar exactamente |
|---|---|---|
| Proxy nvngx: exports, Init cache, evaluate dispatch | `OptiScaler-upstream/OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp` | Set de exports completo, orden forward-vs-nuestro, manejo del `NVSDK_NGX_Parameter*`, cuándo passthrough vs backend, lectura de recursos/dims/subrects |
| Vector de carga dxgi + load-hook | OptiScaler `dllmain.cpp` (dispatch por nombre de DLL) + `renodx` (proxy minimal) + POC dxgi_proxy/load_hook | Carga perezosa en primer call (nunca hilo desde DllMain), redirect solo basename exacto `nvngx.dll`, anti-recursión |
| Observer de cola (vtable ECL) | POC nvngx_host QTrack (única fuente — referencias no hacen esto) + renodx hooks | Slot de vtable, orden llamado-después-del-submit, leak intencional documentado, fallback si firma cambia |
| Sellado de guías depth/MV | POC seal.cpp (reglas R69q/R69t) + Streamline `sl.interposer` (patrones de captura de recursos) | Clon DSV, transiciones neto-cero, lista propia tras la del frame, footprint R32F plano 0 |
| Transporte cross-adapter | POC transport.cpp; Neural-coprocessor (bridge multi-GPU) | Flags de heap/buffer, handles NT, fences compartidos, simetría de dims |
| Sesión FSM (arm/re-arm/teardown) | OptiScaler `State::changeBackend` + `FeatureProvider_Dx12::ChangeFeature` (cambio en caliente) | Qué recursos se recrean y cuáles sobreviven, orden de drenaje, cooldown |
| Codec encode/decode + exposure | Fork `DlssNr_Dx12.cpp` (encode/resolve wrap) + POC nr_codec.hlsl | Tonemap/white-point por componente, coverage box workScale, mediana con lag 1 frame, guard de saturación 0.985 |
| Acumulador del delta (D9) | **Fork `dlssnr_residual.hlsl` v2 (leído completo en 07)** | Reproyección MV + blend 0.08 + invalidación (off-screen/|mv|≥2 → hist=0), ping-pong por copia |
| Evaluate del vendor NR (feature 18) | Fork `DlssNr_Dx12.cpp` + POC nr_vendor.h + knowledge `vendor-nr-recipe` | Cap-block del driver, float-slot discovery, guías full-res/color work dims, re-bind de heaps tras evaluate, tragar close inválido |
| Compose post-SR mínimo | Fork `DlssNr_DeferredSr.inl` (p->Set + Dispatch IN-LIST) | Punto de enganche tras el evaluate real, arrival del output por juego (config, no flag-file) |
| Panel ImGui | OptiScaler menu (estructura por features) + análisis 03 | Tab ControlSpec única, explicación siempre visible, planned placeholders, mirror con fuente game/panel |
| ctl v3 | POC coproc_ctl_common.h v2.5 (single-source, static_assert) | Versionado, set-batch, subscribe, coexistencia |
| Backends futuros (F5) | OptiScaler `upscalers/FSR2Feature_Dx12.cpp`, `proxies/XeSS_Proxy.h` | Mapeo NVSDK params → FfxFsr2/XeSS structs, GetProcAddress, creación en device secundario |

### 1.4 Entornos de verificación (tres niveles, en orden)

1. **Harness offline (sin juego)**: `tools/replay_feeder.exe` — lee capturas reales del corpus (`poc-dlss-standalone/captures/cap_*.raw`, fp16 pitch-aligned) y las inyecta al engine por la ABI compartida, señalizando produce. Validable en CI/loop, sin riesgo.
2. **RDR2 en vivo** (offload real): gates de contadores/diag; verificación VISUAL limitada (R73c: el compose no se presenta) → counters + dumps + A/B píxel.
3. **Cities Skylines II** (juego presentador, fork ya validado allí en R65): para verificación VISUAL del delta-tint y del compose (la que RDR2 no puede dar).

### 1.5 Metodología A/B en vivo (lecciones R74b incorporadas)

- Cadena verificada VIVA antes de capturar: leer diag (`done/last/engRes/nrOn`) del mapping, no fiarse del ini ni del panel (estado runtime ≠ persistido).
- Orden de cambios ctl: stage/config que disparan re-arm PRIMERO → esperar `first frame computed` → nrOn/knobs → verificar diag → medir.
- Escena válida: gameplay vivo con cámara quieta; NUNCA menú pausa (grano animado envenena) ni foto-mode (0 evaluates). 2+ capturas por brazo, reportar varianza.
- El restore de fin de ronda SOLO toca lo que uno mismo cambió (jamás pisar Applies del user).

### 1.6 Reglas de código público

- Inglés en comentarios y mensajes; comentarios explican el POR QUÉ (la regla D3D12 o el bug que previene), no el qué.
- Sin rutas personales, sin nombres de máquina, sin binarios en git (`.gitignore` ya cubre), sin credenciales.
- Sin `using namespace` en headers; RAII (ComPtr WRL con la regla `operator&` documentada); un estilo consistente (formato: 4 espacios, braces Allman — el de la POC).
- Todo afirmación "esto está muerto" verificada con include-graph + TUs del build (regla del análisis 02), no por intuición.

## 2. Fases en detalle

### F0 — Esqueleto y build (1 sesión)

**Alcance**: repo layout final (src/shared|host|engine|panel/tools, shaders/, external/, wiki/), `CMakeLists.txt` con 5 targets (`nvngx` DLL, `dxgi` DLL, `sli_engine` EXE, `sli_panel` EXE, `sli_ctl` CLI) + `tools/replay_feeder` (F1), vendoring de imgui (docking branch, MIT) y detours (MIT) con sus LICENSE, dxc integration para shaders (custom command → cso → header), `.gitignore`, LICENSE (MIT), README stub, wiki skeleton (18 páginas de 04b con índices e "Estado: borrador pendiente").

**Pasadas**: P1 n/a (código nuevo), P2 contrastar con CMake de Streamline SDK (estructura de targets DLL proxy), P5 build verde de los 5 targets con stubs vacíos + `cl /W4 /WX /analyze` limpio.

**Gate F0**: `cmake --build` verde desde clone limpio; los 5 binarios se generan; dxc compila un CS de humo a header; wiki index navegable.

### F1 — Capa shared + ABI + ctl v3 (1 sesión)

**Alcance y orden**:
1. `shared/abi.h` — desde POC coproc_common.h (21–130): scalars, handshake (guías triple), tuning (SIN nrStage — muerto con el pre-SR; CON nrBoost + `nrTint` nuevo campo knob + `nrTest` si aplica), magics/offsets, `static_assert` de layout y tamaño. Single-source: host y engine incluyen EL MISMO header.
2. `shared/ctl_common.h` v3 — desde coproc_ctl_common.h + diseño 03 §4: header {magic, version, abi, verb, field, fval, seq, ack, hb} + mirror array + `CTL_FIELD_DELTA_TINT=26`, verbs set-batch(4)/subscribe(5), static_assert 256B.
3. `shared/log.h` — desde host_log.h + rotación por tamaño (10 MB × 3) + crash handler con MiniDumpWriteDump (port de engine/crash_log.h + minidump del host, deuda 01 §3.9).

**Contraste (P2)**: la ABI es diseño propio (ninguna referencia la tiene); el ctl v3 se contrasta contra la v2.5 (evolución, no invención — coexistencia versionada).

**Gate F1**: harness de layout (exe de test que valida offsets de cada campo contra un blob dorado generado de la POC — compatibilidad de interpretación); unit test del log (rotación); crash handler probado con un AV deliberado en un exe de prueba (genera minidump).

### F2 — Engine delta-only (2–3 sesiones) — CORAZÓN, máxima pasada

**Alcance y orden**:
1. `engine/main.cpp` — desde POC engine_main: args mínimos (`--luid --w --h --cf --map`), PickAdapter por LUID (sin --luid = fallo deliberado en vivo), ready magic al handshake (nunca fence).
2. `engine/nr_vendor.h` — desde POC nr_vendor.h (1.212 L → ~650): Init/RebuildKnobs (con DEBOUNCE por nrParamSeq — cada rebuild fuga, deuda 02 §5)/ProcessFrame/UpdateExposure + receta stub R66 completa (core del registro, Init_Ext appId 101616311 SDK 0x15, cap-block del driver, discover_float_slot, forwarder). SIN dumps, SIN stubs A/B, SIN psoDec frame-completo, SIN badge.
3. `shaders/nr_encode.hlsl` — CSMain encode: port 1:1 del codec (tonemap Reinhard + 1/2.2 por componente, coverage box workScale, tiles de exposición).
4. `shaders/nr_delta.hlsl` — CSDecodeDelta: delta display-domain [-1,1], guard saturación, resample work→render + **delta-tint portado con flag por cbuffer (NO flag-file — defecto conocido)** + boost 0..16.
5. `engine/loop.cpp` — desde POC engine_loop: bucle produce/done por segmentos con nombre, eco GPU UNIFICADO (un helper EchoToOut, no dos catches), salida ÚNICA = delta, timeout sin avanzar contador, poison limpio.
6. `tools/replay_feeder.exe` — NUEVO: abre la ABI por nombre, inyecta cap_*.raw del corpus, señaliza produce, lee done, dumpea delta de salida.

**Contraste (P2)**: evaluate vendor vs fork `DlssNr_Dx12.cpp` (orden de sesiones, re-bind de heaps); codec vs fork encode/resolve; reglas de formato vs knowledge `vendor-nr-recipe` (BGRA8 in / RGBA16F out OBLIGATORIO, fp16 input = fastfail).

**Validación offline (gate F2)**:
- Eco identidad: sin vendor armado, delta de salida = 0 bit-exacto y el bucle vive 10k frames sin crecer WS (medir WorkingSet pendiente/10 min — leak conocido).
- Con vendor: delta con contenido (stats: media/std/% señal), dump visualizable + **delta-tint verificado en el dump** (rojo/azul según signo).
- Inyección perturbada (captura nocturna): delta visible ∝ 1/luma (regla R70d se sostiene en el harness).
- `cl /analyze` limpio; P4 ojos frescos OBLIGATORIO antes del gate.

### F3 — Host núcleo (3–4 sesiones) — el código de más riesgo

**Alcance y orden (dependencias primero)**:
1. **Design-doc FSM primero** (media sesión, se revisa CONTIGO antes de codificar): estados Idle→Arming→Live→Draining→Teardown→Cooldown, única cola de reconfig (gpuIndex/workScale/DRS/guías tardías dejan de ser 4 vías sueltas — análisis 01 §3.3), recursos se recrean SOLO en Arming, drenaje de listas en vuelo antes de liberar.
2. `host/transport.cpp` — desde POC 133–222 + 800–894: format utils (BppFor/PitchFor/BytesFor/FootprintFmt/Typeless — CONSERVAR íntegro), SharedBuf cross-adapter, fences, mapping `Local\sli_frame_<pid>`. AUDIT del comentario podrido MakeSharedBuf (01 §3.4): verificar en la POC con un log de VRAM si el heap vive en DEFAULT o CUSTOM, y documentar el comportamiento REAL.
3. `host/seal.cpp` — desde POC 1165–1256 + 1429–1594: sello color in-list en el seam del evaluate, clon DSV neto-cero, guías MV en lista propia tras el submit, SIN anillos lag pre-SR (muerto).
4. `host/session.cpp` — FSM: arm/spawn (SUSPENDED + dup handles + cmdline mínima)/teardown (poison + handshake de cierre explícito — mejora de la deuda 01 §3.2)/re-arm/recycle con budget configurable.
5. `host/compose.cpp` — reescritura mínima del post-SR (280–300 + 1258–1427 + 1914–2093): clean→dispatch→composed→output, CB con boost+tint, SIN badge SIN damero, acumulador D9 con reproyección MV (port de nr_compose.hlsl + fork residual v2), arrival del output por CONFIG.
6. `host/ngx_proxy.cpp` — desde POC nvngx_host: exports (def), core loader (LdrLoadDll + registro + glob DriverStore), DoInit cache anti-retry, TryOffload (3-pasos, subrects), EvaluateFeature → TryOffload → forward → AfterRealEvaluate(compose).
7. `host/queue_observer.cpp` — QTrack: vtable ECL slot 10, NoteEvaluateList, OnListSubmitted (produce GPU-ordered), SIN ctx.cs en el hilo de submit.
8. `host/load_vector.cpp` — dxgi forward + detours load-hook.

**Contraste (P2)**: ver matriz §1.3 por fichero; además P4 ojos frescos DOS pasadas (es el módulo donde la POC acumuló todos los device-lost).

**Gate F3 (en vivo, RDR2, sesión dedicada)**:
1. Arm limpio desde menú → `engine live` + counters fluyendo (produce/done/composes).
2. **A/B de inocuidad**: offload OFF vs `COPROC_DISABLE=1` → diff de píxel 0.00% (el SR nativo queda INTACTO con nuestro hook presente).
3. Cadena delta viva: diag counters (done≈last, engRes=1) + dump del delta con contenido en el host.
4. Re-arm en vivo (workScale + gpuIndex + DRS): sin device lost, sin frame negro, cooldown observado, FSM vuelve a Live.
5. Soak 30 min: recycle por budget funciona, WS del engine acotado, sin zombie engines (contar procesos).
6. Teardown limpio al cerrar el juego: shutdown explícito del vendor (deuda: la POC nunca llamaba Shutdown).

### F4 — Panel v3 (1–2 sesiones)

**Alcance**: `panel/panel.cpp` ImGui + backend Win32/DX11; tabla ControlSpec ÚNICA (spec → HWND/ini/hint/rango/campo ctl — mata la TRAMPA PANEL); 5 tabs del diseño 03 §3 con cada control y su explicación en cristiano (EN); combos "(planned)" deshabilitados para FG/upscalers; event log de ctl; mirror con badge game-vs-panel; hotkeys F7/F8/Ctrl+F9 desde el host.

**Contraste (P2)**: estructura de menú de OptiScaler (agrupación por feature); `references/win32-gui-tools.md` para layout (aunque ImGui elimina casi todo el layout manual).

**Gate F4**: cada control hace algo medible en vivo (log del host lo confirma); explicación visible sin interacción; Apply de todo el estado ≤3 mensajes (set-batch); Save/Load ini idempotente; sin arrays paralelos (grep: una sola tabla).

### F5 — Publish pass + verificación visual (1–2 sesiones)

**Alcance**:
1. Auditoría de comentarios EN (pasada P4 completa del árbol: comentario≠código = bug).
2. Auditoría de procedencia: grep de fragmentos OptiScaler (GPL) — solo patrón re-implementado, cita como inspiración en README/ACKNOWLEDGEMENTS.
3. README público: qué es, cómo funciona, restricción R73c (present-path por juego), instalación (user aporta DLLs vendor), disclaimer no-afiliación, licencias terceros.
4. Wiki completa EN: traducir 01–15 según mapa de 04b + escribir frescas 16 (panel design), 17 (verification: delta-tint, capturas borderless), 18 (publishing & licensing); verter `04-worklog-en.md` como 05-historical-worklog.
5. Verificación visual EN CS2 (juego presentador): delta-tint visible en pantalla + compose presentado → capturas para la wiki.
6. Fresh-clone test: clonar el repo a temp, build, deploy a un dir de juego limpio, arrancar — cero dependencias de mi máquina.

**Gate F5**: revisión conjunta user+agent de TODO el árbol; wiki completa; demo visual CS2 documentado.

### F6 — Backends alternativos (post-publish, sesiones separadas)

`IBackendSR` + FSR2 (ffx pública, GetProcAddress, contexto en GPU secundaria) → XeSS → FG (capa swapchain — módulo propio, riesgo anti-cheat documentado). Cada backend con su gate A/B DLSS↔backend en vivo. Patrón de `upscalers/FSR2Feature_Dx12.cpp` como referencia, código original (GPL safe).

## 3. Riesgo principal y mitigación por fase

| Riesgo | Fase | Mitigación |
|---|---|---|
| Device lost por reconfig en caliente | F3 | FSM con drenaje; recursos solo en Arming; soak gate |
| Leak vendor en rebuild de knobs | F2 | Debounce por seq; recycle por budget; medir WS en harness 10 min |
| vtable ECL sin fallback | F3 | Check de firma en debug + log claro; documentado como known-limit |
| Grano del delta (D9) | F2 | Acumulador MV-reproyectado portado del fork y verificado en harness (dump antes/después) |
| Rotura SR nativo | F3 | A/B inocuidad obligatorio (diff 0.00%) antes de cualquier feature |
| Copy GPL inadvertido | F5 | Auditoría de procedencia + P4 |
| Nombre público pendiente | F0–F5 | Working name `new-sli`; rename mecánico al decidir (E) |

## 4. Cadencia de trabajo

- Sesión = 1 fase o sub-fase completa con sus gates; commit por módulo con mensaje descriptivo; `TRACEABILITY.md` actualizado en cada P1.
- Wiki EN se actualiza al cerrar cada fase (no al final): la página de arquitectura al cerrar F3, panel al cerrar F4, etc. — misma disciplina que la wiki POC.
- Si un gate falla 2 veces: parar, escribir el síntoma en el worklog de la fase, diagnosis con harness offline antes de reintentar (nada de hotfix en vivo encadenados — regla de las 3 sesiones de device-lost de la POC).
