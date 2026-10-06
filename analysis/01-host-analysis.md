# Informe de análisis HOST — poc-sr-offload-host → new-sli

Fuente: `D:\proyectos\poc-sr-offload-host\host` (leído completo). offload_host.cpp=2755L, nvngx_host.cpp=734L, dxgi_proxy.cpp=194L, load_hook.cpp=162L, host_log.h=83L, offload_host.h=72L, nr_compose_cso.h=433L, nr_readdepth_cso.h=463L (muerto, ver §2).

## 1. Mapa de componentes (con líneas exactas)

### 1.1 offload_host.cpp — el núcleo (2755L)
| Bloque | Líneas | Función |
|---|---|---|
| Contrato ABI (espejo de engine/coproc_common.h) | 21–130 | `CoprocFrameScalars` (22–35, incluye `engineHostFrame` R70:34), `CoprocHandshake` (37–55, guías triple `hgd/hgm[3]`), magics/offsets (57–62), `CoprocTuning` (64–91, con `nrStage`:87, `nrWorkScale`:89, `nrBoost`:90), tabla `COPROC_KEYS` (95–128, SOLO la usa FillTuples muerto) |
| Utilidades de formato D3D12 | 133–222 | `BppFor` (133–150), `PitchFor` (152–155, align 256), `BytesFor` (157–162), `FootprintFmt` (168–186, SINT/UINT→FLOAT de familia), `TypelessGuideFmt` (195–206), `TypedGuideFmt` (208–222) — **CONSERVAR íntegro** |
| Tipos auxiliares | 224–300 | `EnvOn` (224–227, muerto), `SharedBuf` (229–245, heap+buf+handle NT), `StepFence` (247–275, muerto), `ComposeGpu` (280–300, post-SR) |
| Estado global `Ctx` | 302–422 | Monolito: config, dispositivo/dims/LUIDs, SharedBufs de transporte (321–325), fences compartidos (326–328), proceso engine (333–339), triple pending async (341–360), ctl (362–371), estado NR pre-SR (373–399: texNrColor, anillos lag×8, lagScalars), lista seal propia (405–418) |
| ctl: include single-source | 427–428 | `../engine/coproc_ctl_common.h` + static_assert 128B |
| ctl: listener protocolo panel | 432–604 | `TuningDefaults` (432–451), `CtlPollThread` (453–604: verb set/get/reset, espejo de estado, heartbeat 25ms, casos nrStage 505–525 / workScale 526–535 / boost 536–544 / gpuIndex 545–557) |
| ctl: ciclo de vida + hotkeys + ini | 606–798 | `CtlStart` (606–634), `HotkeyThread` (639–712: F7 A/B, F8 reset, Ctrl+F9 panel), `CtlStop` (714–743), `CtlLoadIni` (748–798) |
| **MakeSharedBuf cross-adapter** | 800–847 | CreateHeap SHARED\|SHARED_CROSS_ADAPTER align 64KB + placed buffer ROW_MAJOR flag ALLOW_CROSS_ADAPTER + CreateSharedHandle NT |
| Selección de GPU1 | 849–885 | `PickSecondAdapterLuid`: enumera NVIDIA ≠ adaptador del juego, ordinal configurable |
| Fences compartidos | 887–894 | `MakeSharedFence` SHARED\|SHARED_CROSS_ADAPTER; `MakeStepFence` 896–902 muerto |
| **Spawn engine (ABI handshake)** | 906–986 | CreateProcess SUSPENDED oculto + cmdline --luid/--w/--h/--cf/--map, `dup()` DuplicateHandle de handles NT (inColor, guideD/M[3], out, produce, done) al handshake (952–981), ResumeThread. hib/hie retirados=0 (978–979) |
| Dump forense R73c | 992–1079 | `DumpTexDeltaDump`: readback texDelta/texHist a .rgba16f — TEMPORAL, eliminar |
| **Teardown / TeardownEngine** | 1084–1162 | Engine-only (conserva panel) vs completo; poison produceFence=UINT64_MAX (1088–1089); mata todo estado de entrega (R70f:1144–1153) |
| **Sellado guías depth/MV** | 1165–1256, 1491–1594 | `ArmDepthClone` (1180–1200: clon IDÉNTICO fmt crudo+flag DSV), `RecordDepthCapture` (1202–1256: en lista del juego DW→COPY_SOURCE→CopyResource→DW neto-cero + plano0 R32F→shared buf); guías MV en **lista propia** sealCmd (1505–1594, R69q: nunca barriers en lista del juego); R69t: depth DSV NO copiable → solo MV (1542–1546) |
| **InitCompose post-SR** | 1258–1427 | Texturas clean/composed/delta/hist×2/mv (1276–1322), RS 3 params 4 SRV+2 UAV (1323–1358), PSO CSO, heap 7 slots, SRV MipLevels=1 (1384), t2=hist[1] fix R73c (1403–1407) |
| **RecordSealFrame (sello)** | 1429–1766 | Color in-list en el seam del evaluate (1441–1473, subrect box), depth clone (1480), re-arm guías tardías (1491–1499), seal list MV (1505–1594), **anillos lag pre-SR** (1600–1671, ELIMINAR), copia delta K→texDelta+MV K→texMv (1673–1747), Close/rebuild (1748–1762) |
| **DeliverPendingInList (pre-SR)** | 1775–1810 | Copia bufOut→texNrColor en la lista del juego — CAMINO PRE-SR, ELIMINAR |
| Muertos varios | 1812–1908 | `WriteGpuBytesToDisk` (1812–1864), `FillTuples` (1870–1908) |
| **AfterRealEvaluate (compose post-SR)** | 1914–2093 | En la MISMA lista tras el evaluate real: output→clean, dispatch CS (CB 11 consts con cTest/cBoost 1990–2021), composed→output, swap hist (2065–2089). **R73c: RDR2 nunca lo presenta** |
| API pública | 2099–2132 | `GetParamRes` (2099–2110), `Configure` (2113–2126, CtlStart desde menú R64c), `SetAppDataPath` (2128–2132) |
| **DlssEvaluateSync (orquestador)** | 2134–2690 | Re-arm en caliente (2147–2152), offloadOn (2154–2160), DRS re-arm (2166–2178), **arm** (2180–2415: LUID, MakeSharedBuf inColor/out, mapping coproc_frame_PID 2223–2238, tuning+ini 2240–2255, texNrColor 2257–2277 [ELIM], anillos 2287–2334 [ELIM], InitCompose 2340–2341, guías triple 2346–2395, SpawnEngine 2403)), READY handshake (2417–2437), entrega async (2462–2583: poison 2470–2490, **recycle WS>5GB** 2503–2515, delta diag 2517–2527 [ELIM], rama post-SR 2530–2542, rama pre-SR 2543–2581 [ELIM]), escalares al mapping (2585–2600), sello+pending triple (2602–2614), **sustitución params pre-SR** (2621–2687 [ELIM]), return false=forward nativo SIEMPRE (2689) |
| **OnListSubmitted (produce)** | 2698–2745 | Desde HookedECL tras submit real: entrega sealCmd detrás de la lista del frame (2718–2722), Signal produce GPU-ordered (2741), dump R73c (2724–2739 [ELIM]) |
| Shutdown | 2747–2752 | Teardown completo bajo ctx.cs |

### 1.2 nvngx_host.cpp — proxy NGX transparente (734L)
- **Carga del core real**: `CoprocLdrLoadExW` LdrLoadDll directo (49–66), `CorePath` registro HKLM `NGXCore\FullPath` + fallback glob DriverStore (79–110), `Core` (114–131), `Forward` (139–146).
- **Observer de cola (QTrack)** (157–267): patch de vtable ID3D12CommandQueue slot 10 = ExecuteCommandLists (210–239, probe queue leak intencional 236), `HookedECL` (175–206) — llama `Offload::OnListSubmitted` DESPUÉS del submit real; `NoteEvaluateList` anillo de 8 listas (241–248); `SealQueue` (250–257).
- **Enablement** (278–303): ficheros presentes + `COPROC_DISABLE` → `Offload::Configure`.
- **TryOffload** (395–505): lectura 3-pasos typed→untyped de recursos (GetParamResource 307–322), dims reales Width/Height + Render.Subrect (455–474), subrects base de cada input (476–483), escalares jitter/mv/preExp/sharpness/reset (485–502). `DumpParamBlock` referencia de superficie NGX (331–393).
- **DoInit anti retry-storm** (513–537): forward UNA vez + cache (el driver reintentó 1.2M veces con fallos crudos).
- **Exports** (543–734): EvaluateFeature (612–627) y _C (631–649, la que usa RDR2) — TryOffload → si rechaza forward + **AfterRealEvaluate** (624/645); Shutdown (651–655); forwarders D3D12; stub CUDA/VULKAN/OTA (721–730). Export table en nvngx.def.

### 1.3 dxgi_proxy.cpp (194L) + load_hook.cpp (162L) + detours
- dxgi_proxy: forward de 5 exports reales + 13 de seis slots ABI-safe (71–192); `EnsureHost` (41–60) carga perezosa de nvngx.dll en el primer call dxgi (thread del juego; el hilo helper de la primera versión mataba el proceso) + `InstallNgxLoadHook`.
- load_hook (usa `host\detours\detours.lib`, precompilado): detour de LoadLibraryExW/W en kernel32 Y kernelbase (134–162); redirige SOLO basename exacto `nvngx.dll` (82–94), NUNCA `_nvngx.dll` (lo carga el core internamente); `OurCaller` anti-recursión (71–79); logging de loads ngx como evidencia.
- Vector de carga: slot dxgi.dll (probado por OptiScaler/ReShade en RDR2); version.dll fast-fail 0xC0000409 (comentario 12–14).

### 1.4 host_log.h (83L)
`HostLogLine` append-only con timestamp ms (10–43), `HostLog` printf-safe (49–56), `HOST_LOG_RATE` macro por call-site (61–68), `HostDllDirW` (71–83). Sin rotación; abre/cierra fichero por línea.

### 1.5 Crash/minidump
NO hay minidump ni `SetUnhandledExceptionFilter` en el host. Solo engine/crash_log.h (logger de excepción con módulo+offset y backtrace resuelto). Oportunidad para new-sli: crash handler + MiniDumpWriteDump en el host.

## 2. Código muerto/obsoleto a eliminar

### 2.1 Camino pre-SR de NR (decisión: eliminar por completo)
- `DeliverPendingInList` **1775–1810**; entrega pre-SR en DlssEvaluateSync **2543–2581**; sustitución de params Color/Jitter/MV/Reset/Depth/MV-ring **2621–2687**.
- `texNrColor`: decl 375, creación 2257–2277, entrega 1780–1808, reset 1150.
- Anillos lag: `texNrDepthRing/texNrMvRing[8]` decl 387–388, creación 2287–2334, sellado 1600–1671, consumo 2647–2662. `lagScalars/lagValidFrom/lagDepthFrame/lagMvFrame` 393–397, 2680–2686, reset 1132–1133.
- Gate `postSrMode()` 1168 y todas sus ramas; `nrStage` en tuning 87, ctl 505–525, ini 784; herramienta engine/ctl_set_nrstage.*.
- `nrActive` 398 (muere con el pre-SR).

### 2.2 Compose post-SR tal cual está — HALLAZGO R73c (decisión arquitectónica)
El compose escribe en la textura `output` del DLSS (AfterRealEvaluate 2051–2057), pero **RDR2 copia/procesa esa textura ANTES del present ⇒ el compose jamás llega a pantalla**. Evidencia: badge 512 y damero cTest==1 tampoco se veían (la prueba reina que motivó DumpTexDeltaDump y el delta diag). Los efectos NR visibles históricamente venían del camino pre-SR (texNrColor→Color del SR), ya eliminado. **Consecuencia de diseño para new-sli**: el punto de enganche correcto para NR post-SR NO es el out del DLSS sino la textura que el juego realmente presenta (present-chain/swapchain o post-process posterior); en RDR2 ese punto no existe vía NGX → el nuevo proyecto mantiene el compose post-SR como vía conceptual/demo para juegos que sí presentan el out, documentando la asimetría, con el flag de tinte rojo/azul como verificación visual. Eliminar: `InitCompose` 1258–1427, `AfterRealEvaluate` 1914–2093, `ComposeGpu` 280–300, llamadas en nvngx_host 621–625/643–647, sello delta 1673–1747, decl 66–68 en offload_host.h. (Reescribir versión mínima con tinte, §5.)

### 2.3 Debug temporal R73c
- `DumpTexDeltaDump` **992–1079** + `dumped/deltaDumpPending` 298–299, 1742, disparo en OnListSubmitted 2724–2739.
- Badge 512 + damero: `composeTest` 2000–2011 (modo '1'; '2'=delta-vista ES la base del nuevo flag de tinte), `cTest` en ComposeCb 1994/2019, patrón dentro del CSO nr_compose_cso.h.
- Diags: 'delta diag' 2517–2527, 'compose diag' 2023–2028, 'lag diag' 2666–2676, contadores 1165–1167, `lateDeliveries` 2516.
- Flag-files de iteración: `coproc_sealguide.on` 1531–1546, `coproc_compose_arrival.bin` 1930–1953 (el arrival configurable merece sobrevivir como config, no como fichero suelto).

### 2.4 Muerto puro (nunca referenciado)
`EnvOn` 224–227 · `StepFence`+`MakeStepFence` 247–275/896–902 · `WriteGpuBytesToDisk` 1812–1864 · `FillTuples`+`COPROC_KEYS` 95–128/1870–1908 (muerto desde R64) · `inBias/inExp` 322–325, reset 1110–1111, hib/hie=0 978–979 · `depthClone` 331 (nunca creada) · `pendingOut/pendingOutBaseX/Y` 351–353 · `capturedTime/timeIdx/firstFrameTick` 337–339 · **nr_readdepth_cso.h + nr_readdepth.hlsl** (no incluidos por ningún .cpp del host; camino rev5 sustituido por copia pura rev6) · `psapi.h` include línea 10 — OJO: solo lo usa el recycle WS (2506); si se elimina psapi.h se elimina el recycle. Recomendación: conservar el recycle (mitiga el leak real de ~10MB/s) y mantener psapi.h ordenado; si la orden es eliminarlo, migrar el recycle a `GetProcessMemoryInfo` vía kernel32/`NtQuerySystemInformation` o asumir la deuda.

## 3. Deudas técnicas y bugs conocidos
1. **Leak del runtime NR en el engine ~10 MB/s** (comentario 2498–2502; device-lost cada ~650 s). NO arreglado: mitigado por recycle WS>5GB cada 300 frames (2503–2515) con cooldown 500 ms. En new-sli: budget configurable + telemetría.
2. **Fence poison**: TeardownEngine señala produceFence=UINT64_MAX (1088–1089); el evaluate detecta done==UINT64_MAX y distingue device-lost del juego vs lado engine (2470–2490). Frágil: el centinela coincide con el valor de TDR real del fence compartido; sin handshake de cierre explícito con el engine.
3. **Re-arm en caliente con estados parciales**: hasta 4 vías (gpuIndex 545–557, workScale 526–535, DRS 2166–2178, guías tardías 1491–1499) que mezclan Teardown/TeardownEngine + retryAt ad-hoc. R70d documentó que Reset() de texturas en caliente referenciadas por listas en vuelo = device lost (510–514); hoy se sobrevive con flags, pero es el mayor riesgo estructural.
4. **Comentario podrido en MakeSharedBuf** (802–807): describe el fix R69k "Heap CUSTOM en system memory (pool L0)… 44 MB fuera de VRAM" pero el código usa `HEAP_TYPE_DEFAULT` (807) — los bufs viven en VRAM de GPU0. O el fix se revirtió o el comentario miente; auditar antes de copiar.
5. **Doble mutex ctx.cs/pendingCs** (355–358, 462–465, 2700–2703): correcto (el hook de submit no puede tomar ctx.cs) pero frágil de mantener; el `Ctx` monolito global (302–420) mezcla responsabilidades.
6. **vtable patch ECL slot 10** (nvngx_host 224–236): asume layout de vtable de ID3D12CommandQueue; probe queue leak intencional. Sin fallback si el driver cambia.
7. **Tuning relay sin lock** (2162–2164): memcpy de generación mixta documentado como tolerable (462–465) — válido pero sutil.
8. **Log sin rotación** (host_log.h 30–43): open/close por línea; a 1.2M inits habría sido letal — hoy con HOST_LOG_RATE se aguanta, pero un proyecto público necesita rotación por tamaño.
9. **Sin crash handler en el host** (solo engine tiene crash_log.h): un AV dentro de nvngx.dll tumba el juego sin evidencia.

## 4. Qué MERECE conservarse tal cual (reglas duramente ganadas)
1. **In-list delivery/seal pattern**: sellar DENTRO de la lista del evaluate (punto GPU exacto donde DLSS lee; el sellado en call-time leía MV ya cleared = natación v1; 1429–1436) y produce GPU-ordered tras el submit (OnListSubmitted 2698–2745 + HookedECL 200–206). La entrega del resultado también in-list en el seam del output (1768–1774) — patrón a conservar aunque cambie el payload.
2. **Lista nueva → Close antes de Reset** (1520–1524): una lista recién creada está en recording; Reset exige cerrada.
3. **Sellado de guías en lista PROPIA, detrás de la del frame** (R69q, 1482–1524): grabar barriers sobre texturas del juego dentro de su propia lista corrompía el device a minutos (ERR_GFX_D3D_DEFERRED_MEM / EMP.dll AV, 5 sesiones).
4. **Depth DSV no es copiable** (R69t, 1500–1504): CopyTextureRegion prohíbe src ALLOW_DEPTH_STENCIL; solución clon idéntico fmt+flags (ArmDepthClone) con transiciones neto-cero DW→COPY_SOURCE→CopyResource→DW y plano 0 R32F a footprint (1202–1256).
5. **Footprint/TYPELESS**: copias rechazan SINT/UINT → mapear al FLOAT de la familia (FootprintFmt 168–186); hermano TYPELESS del mismo bit-width para copy-targets (TypelessGuideFmt 195–206); output TYPELESS no admite vistas → texturas propias con fmt vista de familia + intercambio por CopyTextureRegion subresource, NUNCA heredar flags (1276–1291).
6. **SRV explícita MipLevels=1** (1384): 0 en desc explícita = device removed silencioso. **CS con UAV sin sampler** (RS compose 1323–1358).
7. **Cross-adapter heaps**: CreateHeap SHARED|SHARED_CROSS_ADAPTER (align 64KB) + placed buffer ALLOW_CROSS_ADAPTER + CreateSharedHandle; fences shared cross-adapter (800–894).
8. **Never block game thread** (R69j 2462–2469): delay-line tolerante; si no hay resultado → forward nativo y entrega en evaluate posterior. Hooks en submit-thread SIN ctx.cs (R69i 2700–2703).
9. **Proxy NGX transparente**: Init forward UNA vez + cache (anti retry-storm 1.2M, 509–537); core cargado por LdrLoadDll directo; load-hook solo nombre exacto nvngx.dll, jamás _nvngx.dll; fallback nativo permanente (return false 2689) — el juego nunca se queda sin frame.
10. **Vector dxgi.dll** con carga perezosa en el primer call (nunca hilo desde DllMain); def de exports + stubs familia CUDA/VULKAN/OTA.
11. ABI single-source con static_assert (include 427–428) y lectura 3-pasos typed→untyped de params (307–322).

## 5. Estimación de líneas por módulo propuesto (new-sli, C++ puro)
| Módulo | Origen principal | Líneas est. |
|---|---|---|
| `ngx_proxy.cpp` (exports, core loader, DoInit cache) | nvngx_host 49–146, 507–734 | ~430 |
| `queue_observer.cpp` (QTrack ECL vtable) | nvngx_host 154–267 | ~120 |
| `load_vector.cpp` (dxgi fwd + detours load-hook) | dxgi_proxy + load_hook | ~350 |
| `transport.cpp` (SharedBuf cross-adapter, fences, mapping, format utils Bpp/Pitch/Footprint/Typeless) | 133–222, 229–245, 800–894, 2223–2238 | ~330 |
| `abi.h` single-source (handshake/scalars/tuning/ctl) | 21–130 + coproc_ctl_common.h | ~210 |
| `session.cpp` (arm/spawn/teardown/re-arm/recycle/poison) | 849–986, 1084–1162, 2113–2415, 2462–2515 | ~520 |
| `seal.cpp` (sello color in-list, depth clone, guía MV, seal list) | 1165–1256, 1429–1594 | ~380 |
| `ctl.cpp` (panel protocol, hotkeys, ini) | 432–798 | ~300 |
| `delta_tint.cpp` (NR post-SR conceptual: compose mínimo, tinte rojo/azul, boost 0..16, sin badge/damero) | reescritura de 1258–1427+1914–2093 | ~280 (+CSO ~330) |
| `log.cpp` (logging + rotación + crash handler/minidump host) | host_log.h + nuevo | ~140 |
| **Total host** | | **≈2.900–3.060** (vs 4.896 actuales incl. .h; −35/40% al purgar pre-SR/debug) |

Notas de rewrite: (a) `Ctx` monolito → session/transport/seal separados con propiedad clara; (b) re-arm unificado en una sola máquina de estados (arm→ready→live→teardown→cooldown); (c) comentarios en inglés, orientado a publicación; (d) documentar R73c en el README como restricción por juego (present-path asimetría).
