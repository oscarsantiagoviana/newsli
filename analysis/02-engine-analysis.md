# Análisis ENGINE — poc-sr-offload-host → rewrite en new-sli

Fuente: `D:\proyectos\poc-sr-offload-host\engine\` (14 archivos leídos completos, 4.356 LOC totales). `coproc_panel.cpp` excluido (otro agente).

## 1. Arquitectura del engine (R64 NR-only)

**Proceso separado** (`coproc_engine.exe`, `wmain` en engine_main.cpp:144). El SR real corre en GPU0 vía el proxy del host; este proceso es solo el worker NR sobre GPU1.

**Arranque (engine_main.cpp)**
1. `InstallCoprocCrashHandler()` (crash_log.h: backtrace módulo+offset a `coproc_engine.log` ante 0xC0000005 en blobs NVIDIA).
2. `ParseEngineArgs`: solo exige `--w --h --cf --map` (R64 NR-only; quedan ~20 args legacy SR/replay sin uso).
3. `PickAdapter`: enumera adaptadores NVIDIA (VendorId 0x10DE) y crea device FL 12.0 por **LUID** (GPU1). Sin `--luid` en modo live **falla** a propósito — solo replay puede tomar "primer NVIDIA" (evita bind silencioso a la GPU del juego).
4. `SetupDevice`: DIRECT queue + allocator + command list + localFence + evento.
5. `OpenSharedTransport` → escribe `hs->ready = COPROC_READY_MAGIC` al handshake (**nunca** un fence: el ready no debe sangrar al produce fence del juego — lección R28).
6. `RunFrameLoop`.

**Transporte compartido (OpenSharedTransport, engine_loop.cpp:124)**
- File mapping nombrado de 8192 B: `CoprocFrameScalars` (0..256) | `CoprocHandshake` (@256, magic `0xC070C0C500000011`) | tuples (@384, legacy) | `CoprocTuning` (@2048, magic `0xC070C0C500000007`).
- El handshake trae los NT handles duplicados: `hic/hoc` (color in/out, **buffers placed cross-adapter** con `ALLOW_CROSS_ADAPTER` — este driver rechaza texturas compartidas placed con E_INVALIDARG), `hpf/hdf` (fences produce/done), y R69: `hgd[3]/hgm[3]` (**guías triple-buffer** depth+MV con fmt/pitch/dims crudos del juego y `depthInverted` del bit 1<<3 de los create-flags).
- Locales: `texInColor` (fmt del juego, flags NONE), `texOut` (UAV), `texGuideDepth/texGuideMV` (fmt crudo, ciclo SRV↔COPY_DEST), readback/upload CPU del viejo bridge + badge upload 8×8.
- `BppFor/PitchFor/FootprintFmtFor`: pitch alineado a 256; los footprints rechazan TYPELESS y (empírico) SINT/UINT → se declara el miembro FLOAT de la familia.

**Bucle de frames (RunFrameLoop, engine_loop.cpp:280)**
- Espera `produceFence >= frame+1` (poll 1 ms, timeout 60 s **sin avanzar** el contador — un timeout que avanzara desincronizaría para siempre la numeración). Poison: `GetCompletedValue()==UINT64_MAX` → salida limpia.
- Dims DRS: `scalars->renderW/H` distintas de args → bypass (guard R60).
- **Disciplina de segmentos R60**: cada tramo (`in`, `rb`, `vendor-frame`, `vendor-eco`, `nr-ul`, `out`) se cierra/suma/espera con nombre propio (close fallido nombra su segmento exacto; wait 120 s = sospecha de GPU hang).
- Vendor init (una vez): `c.vendor.Init(...)` con knobs del tuning; si falla → **bypass-eco** el resto de la sesión.
- Por frame (vendor armado): rebuild de knobs si `nrParamSeq` cambió (CREATE-time: release+recreate de la feature) → `ProcessFrame` en UNA sesión de cmd → `UpdateExposure` → `texOut→bufOut` → `engineResult=1`, `engineHostFrame = produce completado` (R70: el done fence lleva el contador del ENGINE que arranca ~3× atrás; el host necesita SU frame para el anillo de guías) → `Signal(doneFence, frame)`.
- Excepciones (std y no-std): `DumpGuidesNow` + `EchoToOut` (cmd+fence propios: texIn→texOut→bufOut; sin esto el host entrega bufOut sin actualizar = negro), recreación de cmd/alloc, `engineResult=0`, done signal. **El engine nunca muere por un frame roto.**

**Vendor NGX feature 18 NR in-process (nr_vendor.h, 1.212 líneas)**
- Receta stub R66: core `_nvngx.dll` del registro (`HKLM\...\nvlddmkm\NGXCore\NGXPath`) → `NVSDK_NGX_D3D12_Init_Ext(appId=101616311, cwd, dev, 0x15)` → `GetCapabilityParameters` (bloque del driver, no AllocateParameters) → `discover_float_slot` (probe del vtable slots 1..7 con round-trip Get) → forwarder `nvngx.dll_dlssnr.dll`: `dlssnr_call_create` (snippet Init_Ext 0x24480451 + create **feature 18** a w×h) y `dlssnr_call_evaluate_v2(cmd, feature, P, colorBGRA8, depth, mv, outRGBA16F, ...)`.
- El caller-gate del snippet mira el **módulo** que llama (path con "nvngx.dll"), no el nombre del exe.
- Tuning pre-create en el bloque P (el modelo lo lee UNA vez; lo que va solo a evaluate se ignora): `DLSSNR.{Enabled,Width,Height,Intensity,Style,Local*,Skin*,ScalingRatio,Reset,...}`, `PerfQualityValue=2`.
- `ProcessFrame` graba en una sesión: copy guías del anillo `[(N-1)%3]` → **encode** (texInColor HDR→BGRA8 sRGB vía CS) → **evaluate** (guías full-res, color/out a work dims, `mvScale*workScale`, reset en seq==0 o forceReset) → **decode** (psoDec frame completo o psoDelta delta puro según `nrStage==1`) → copy tiles→readback (exposure del frame siguiente).
- `UpdateExposure`: mediana CPU (`nth_element` sobre ~5k luminancias de tiles 8×8 en fp16) → `expoScale = 0.18/med`, **lag 1 frame** (estilo fork: la exposure cambia despacio, el lag es invisible y evita un round-trip).

**Codec GPU (nr_codec.hlsl, 246 líneas)**
- `CSMain` (encode): HDR lineal fp16 → Reinhard `d/(1+d)` clamp 1 → `pow(1/2.2)` → BGRA8. Escritura **por componente** (unorm float4 mapea .r→R por nombre; el codec CPU que escribía bytes metía doble swizzle R↔B = pieles azules, R68d). Coverage box exacto cuando workScale≠1 (no tap bilineal: la respuesta no movería con el sub-píxel).
- `CSDecode` (frame completo, pre-SR): `final = orig + boost*(InvCodec(model) − InvCodec(encode(orig)))` en dominio HDR; guard de saturación (enc≥0.985 → delta 0: donde el carrier BGRA8 saturó, la respuesta amplificada es ruido); modelo identidad ⇒ eco bit-exacto.
- `CSDecodeDelta` (post-SR, R70): delta en **dominio display [-1,1]** (`d = model_disp − encode(orig)`; la salida del SR es LDR R10G10B10A2 — sumar delta HDR-lineal ahí sería dimensionalmente incorrecto), resample bilineal work→render, alpha=2 como marca de "señal sólida" para el acumulador del host.
- `workScale` (R71, clamp [0.25, 2.0], workW/H mínimo 64): el modelo corre a render×scale; encode hace coverage box y decode integra. Mediciones de contexto: ~52.5 / 28 / 19.5 ms a scale 1.0 / 0.5 / 0.25 (CPU R67 era 200 ms/frame; el modelo solo ~28).

**Knobs ABI (CoprocTuning, offset 2048)**: `nrOn`, `nrStage` (0 pre-SR / 1 post-SR), `nrWorkScale`, `nrBoost` (0..16, R73b), `nrIntensity/LocalStructure/LocalTone/SkinStructure/Style` (CREATE-time → rebuild por `nrParamSeq`), `forceReset`, overrides (<0 = gana el juego). `CoprocFrameScalars`: frame, jitter, preExposure, sharpness, mvScale, reset, engineResult, renderW/H, engineHostFrame.

**Protocolo ctl (coproc_ctl_common.h)**: mapping `Local\coproc_ctl_v1` (crea el host), `CoprocCtlMsg` 128 B v2.5 (`static_assert` de layout — el copy-rot v2.1-vs-v2.5 mató coproc_ctl una vez), verb get/set/reset, fields 1..25 (`NR_STAGE=23`, `NR_WORKSCALE=24`, `NR_BOOST=25`), seq/ack espejo + heartbeat ~25 ms. El panel habla con el HOST; el host replica al tuning block; **el engine nunca abre el ctl map**.

## 2. Camino PRE-SR a eliminar en new-sli
- **Entrega de frame editado como Color del SR**: con `nrStage==0` el engine decodifica frame completo (`CSDecode`) en `texOut` → `bufOut`, y el host lo swapea como entrada del SR. Es el único camino de NR **visible históricamente** en RDR2. Disparadores concretos: `tuning->nrStage == 1` pasado como `deltaOut` a `ProcessFrame` (engine_loop.cpp:509) — en el rewrite el delta es la única salida.
- **Delay-line 1 frame pre-SR**: sellado N → Fire N / Collect N-1 → done(N-1) (cabecera R63/R64 y comentarios del bucle). El lag decae al host; documentar que el camino post-SR (nr_compose en el host) **nunca fue presentado por RDR2** (R73c): el NR visible SIEMPRE fue pre-SR.
- **Eco CPU del bridge** (engine_loop.cpp:529-541 + camino `nr-ul` con upload+badge): `memcpy(pocnrUlPtr, pocnrRbPtr, bytes)` + upload + badge — es el bypass que valida la arquitectura sin modelo; junto con `pocnrReadback/pocnrUpload/pocnrRbPtr/pocnrUlPtr/badgeUpload` y los segmentos `rb`/`nr-ul`. El fallback del nuevo engine es siempre eco GPU (texIn→texOut→bufOut) como ya hace `EchoToOut`.
- **Coherencia de sets / frames en vuelo**: el host ya no necesita mantener el set coherente del Color entregado al SR (ventaja documentada del delta: "el SR consume el color ORIGINAL del juego — sin delay, sin coherencia de set que mantener", nr_codec.hlsl:141-144).
- `nr_compose.hlsl` vive en el **host** (otro agente), pero el engine debe dejar de duplicar su semántica: alpha=2 "señal sólida" del CSDecodeDelta era el contrato con ese acumulador.

## 3. Código muerto / depuración (verificado, no asumido)
- **`nr_archspoof.h` (251) y `fh86.h` (198): MUERTOS.** Ningún `#include` vivo en ninguna TU (grep exhaustivo); `build.bat` solo compila `engine_main.cpp engine_loop.cpp`; engine_main.cpp:150-153 conserva un **comentario huérfano** del gate `COPROC_ARCHSPOOF=1` cuyo call-site fue eliminado. Eran el spoof de `NvAPI_GPU_GetArchInfo` (0x1B0 Blackwell) para que el runtime NGX permita feature 18 en sm_86 y el lanzamiento manual de cubins extraídos — el camino vendor actual no los necesita. Borrar ambos y el comentario.
- **`replay.cpp` (550): NO COMPILA.** Referencia `c.srEvalBlock/c.srHandle/c.srEvaluate/c.texInDepth/c.texInMotion/c.texInBias/c.texInExp` que ya no existen en `EngineCtx` (era la sesión SR del engine, retirada en R64) y está fuera del build. Herramienta standalone de la era SR; en new-sli, si se quiere replay, es un módulo nuevo contra la ABI delta.
- **`params_block.h` (222): muerto en el engine.** Incluido por engine_main.cpp y replay.cpp pero jamás instanciado (el vendor usa el capability block del driver con sus helpers `set_uint/set_res/set_flt`). Mantener solo si new-sli necesita un param block propio.
- **Badge 512 en ambos CS**: `CSDecode` pinta 512.0 HDR en [16,24)² (línea 242-243); `CSDecodeDelta` pinta d=512 con alpha=2 en [24,32)² (195-199). Además el badge CPU fp16 0x5F00 8×8 en (16,16) del camino eco (engine_loop.cpp:245-265, 600-609). **Decisión new-sli**: eliminar los tres; el flag estructural pasa a ser la delta-vista.
- **Dump one-shot frame 240** (`dumpFrame=240`): `InitDump` + `rbDumpIn/Model/Final/GuideD/GuideM` + `CoprocDumpD3DMessages`, `BppFor` tables duplicadas (engine_loop, replay), `nr_guide_depth.hlsl`+cso (la conversión SINT→FLOAT "murió" en R69v — el host entrega R32_FLOAT directo), `nr_bridge_cso.h` (ningún include), tuplas `COPROC_KEYS`/`CoprocTuple` (relay del bloque SR, sin consumidor NR), campos muertos de `EngineCtx` (`pocnrInFlight` R63, `outPitch`, ~20 args SR/bias/exposure de `EngineArgs`).
- Binarios/objetos versionados (`.obj/.exe/.lib/.exp/.log/eng_disasm.txt`) fuera del repo público.

## 4. Lo que se QUEDA (base del rewrite)
1. **nr_vendor.h** (recortado): Init/RebuildKnobs/ProcessFrame/UpdateExposure/EchoToOut + ABI del param block del driver (slot discovery, core por registro, forwarder). Sin dumps, sin stubs, sin psoDec.
2. **Codec con exposure**: `CSMain` encode BGRA8 + tonemap Reinhard/2.2, grid de tiles, mediana 0.18 con lag 1 frame, coverage box workScale.
3. **CSDecodeDelta**: delta display-domain [-1,1], guard de saturación, resample work→render. ÚNICA salida del engine nuevo.
4. **Knobs ABI** (`CoprocTuning`) y **ctl protocol** (`coproc_ctl_common.h` + herramienta ctl estilo `ctl_set_nrstage`): sin cambios de layout (static_assert).
5. **workScale** clamp [0.25,2.0] con mínimos 64px.
6. **boost 0..16** en el decode (R73b/R73c).
7. **Delta-vista como flag de verificación estructural**: `dTest==2` en `CSDecode` (tinte rojo=+/azul=− por magnitud, `×32` HDR para atravesar el SR, nr_codec.hlsl:230-237) sustituye al badge: si el tinte llega a pantalla, la cadena host→engine→delta→compose está viva. **Ojo**: el modo vive hoy en `CSDecode` (camino que se elimina) y su `dTest` se lee del ARCHIVO `coproc_compose_test.bin` junto al exe ('2' = vista), no del tuning ABI — en el rewrite hay que **portarlo a CSDecodeDelta y moverlo a un knob del tuning** (p. ej. reutilizar un campo test explícito).
8. Disciplina de segmentos con nombre + catch-all con eco GPU + crash handler + poison/timeout del bucle: patrones probados, se conservan.

## 5. Bugs / trampas documentadas (transferir al README de new-sli)
- **fp16 input crash**: RGBA16F como ENTRADA del modelo crashea el runtime con **0xC0000409** (stub R66); por eso texColor es BGRA8 sRGB. El output RGBA16F display-domain es válido.
- **MipLevels=0 → device removed**: SRV de textura con `MipLevels=0` es desc inválido y el driver remueve el device **en diferido** (R68b). Siempre `=1`.
- **UAV de RWStructuredBuffer**: `Format=R32_UINT` es view-invalid (remoción diferida); usar `DXGI_FORMAT_UNKNOWN` + `StructureByteStride=4` (sin stride también inválido).
- **Enums typeless**: footprints y copies rechazan TYPELESS y (empírico) SINT/UINT — declarar el FLOAT de la familia (`FootprintFmtFor`/`FpFmtFor`); la depth cruda se copia buf→tex con el fmt REAL del juego (bits idénticos).
- **Sagas del runtime**: evaluate puede dejar recursos internos transicionados → close E_INVALIDARG/0xBAD00002 (el caller debe poder tragar el close); `Init_Ext` puede dejar el device removido SIN fallar → check `GetDeviceRemovedReason()` tras el core init (engine zombie); tras el evaluate, **re-bind de descriptor heaps** (el runtime pisa los heaps del cmd).
- **Leak de memoria del runtime NR**: cada `RebuildKnobs` hace release+create de la feature 18 y el runtime no devuelve toda la memoria (crece con cada rebuild; los knobs son CREATE-time, mover un slider = rebuild); además el engine nunca llama `Shutdown()` al salir (lo limpia la muerte del proceso). Mitigar: debounce del rebuild por nrParamSeq y shutdown explícito.
- **Race estructural**: `engineResult`/`engineHostFrame` se escriben ANTES del done signal (el host los lee al observar done) — mantener el orden.
- **BppFor incompleto**: la tabla de engine_loop mide bien cf=10 (RGBA16F) pero R16G16 (33) cae al default 4 Bpp (real 8). Inofensivo hoy, trampa si cambia cf.
- **Texturas compartidas placed rechazadas** (E_INVALIDARG) en este driver → el transporte es buffers+footprint, ida y vuelta SIMÉTRICAS a dims render.
- **Listas simultáneas / decay**: texOut no-simultánea dejada en UAV por el evaluate NO decae a COMMON tras ExecuteCommandLists — la transición UAV↔COPY explícita es obligatoria (misma razón de los segmentos).

## 6. Propuesta de módulos para new-sli (C++, comentarios en inglés, público)

| Módulo | Contenido | LOC est. |
|---|---|---|
| `engine/abi.h` | scalars + handshake + tuning (sin tuples) + log + crash handler (fusión coproc_common/crash_log) | ~190 |
| `engine/main.cpp` | args mínimos (`--luid --w --h --cf --map`), PickAdapter por LUID, setup device, ready magic | ~140 |
| `engine/loop.cpp` | bucle produce/done por segmentos, única salida = delta, eco GPU unificado (un helper, no dos catches duplicados), sin badge CPU ni readback/upload | ~350 |
| `engine/nr_vendor.h` | init/create/evaluate/release + RebuildKnobs + UpdateExposure + EchoToOut (sin dumps/stubs/psoDec) | ~650 |
| `engine/codec/encode_delta.hlsl | CSMain + CSDecodeDelta + modo tinte portado (flag por cbuffer, no archivo) | ~160 |
| `engine/ctl_common.h` | CoprocCtlMsg v2.5 intacta | ~55 |
| `tools/ctl.cpp` | setter genérico field/fval (generaliza ctl_set_nrstage) | ~50 |
| **Total** | | **~1.600** vs 4.356 actuales (−63%) |

Reglas del rewrite: (a) el engine entrega SOLO delta display-domain; (b) sin badge — la delta-vista (dTest==2 portado, knob ABI) es la verificación estructural; (c) boost 0..16 se queda; (d) documentar en README que el post-SR compose (nr_compose, host) nunca fue presentado por RDR2 y que el NR visible histórico fue siempre pre-SR.
