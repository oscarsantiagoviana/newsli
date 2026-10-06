# Mapa detallado de nuestra pipeline NR (new-sli R79/R80) — línea a línea, con cada interfaz y tipo

Fecha: 2026-09-29 (noche). Árbol: `D:\proyectos\new-sli` @ working tree (post-6f86bab + cambios R80 sin commit).
Complementa: `analysis/2026-09-29_audit-cadena-tiempo-captura-compose.md` (auditoría R78, cadena pre-R79 — lo que allí sigue vigente se marca [R78-OK]).

## 0. Vista de bloques (flujo de un frame N)

```
RDR2 (D3D12, GPU0)
 │ 1. CreateDXGIFactory* → dxgi.dll (nuestro proxy)
 │ 2. CreateSwapChainForHwnd → FactoryProxy → SwapChainProxy (Present/Present1 hookeados)
 │ 3. driver preloads ngx core → load_hook redirige "nvngx.dll" → nuestro host
 │ 4. NVSDK_NGX_D3D12_Init_Ext → OnInit → Configure(1) → FSM arranca; QTrack vtable hook
 │ 5. CreateFeature(SuperSampling) → guardamos handle
 │ 6. EvaluateFeature_C(cmdList, handle, params) ── OfferFrameToSession
 │       │ normaliza EvalArgs (subrects, mvScale, jitter, reset, flags)
 │       │ host::Evaluate → EvaluateInner: scalars→mapping, ++frame,
 │       │ RecordSealFrame (color/depth en la LISTA del juego; MV+ring en sealCmd)
 │       │ pending{frame, list} ← {N, cmdList}
 │       └ SIEMPRE cae al evaluate nativo (el juego no pierde nada)
 │ 7. ExecuteCommandLists (vtable slot 10 hookeado) → OnListSubmitted
 │       │ push sealCmd detrás de la lista del frame EN LA MISMA COLA
 │       └ queue->Signal(produceFence, N)   [GPU-ordenado: sellado real]
 │ 8. sli_engine.exe (GPU1) esperaba produce ≥ N:
 │       seal bufInColor→texInColor → ProcessFrame:
 │         copia guías (frame%3) → encode HDR→BGRA8(+tiles) → fEval (modelo NR
 │         vendor, via forwarder) → decode-delta (display domain [-1,1]) →
 │         texOut→bufOut → engineHostFrame=N → Signal(doneFence, frame)
 │ 9. Present(N) (game thread) → SwapChainProxy::Present → sli_PresentGate → PresentGateInner
 │       espera done con engineHostFrame==N (evento 250ms re-check, sin límite)
 │       → ComposeStageDraw: bufOut→texDelta (copy), [struct merge si tint=2],
 │         fullscreen triangle ADITIVO sobre backbuffer (PSO por fmt, RTV cache),
 │         en la cola del juego (fallback: propia) → fence wait 2s → real Present
 └ frame N en pantalla = frame N del juego + delta N  (same-frame, directiva R79)
```

## 1. Frontera juego → dxgi.dll (load vector)

| Interfaz | Tipo/firma | Implementación | Notas |
|---|---|---|---|
| `fwd_CreateDXGIFactory/1/2` | `long WINAPI(const void* riid, void** factory)` | dxgi_proxy.cpp:598-620 | `EnsureHost()` primero (reclama nombre nvngx + instala load hook + resuelve gate), luego `WrapFactory` |
| `fwd_DXGIDeclareAdapterRemovalSupport`, `fwd_DXGIGetDebugInterface1` | idem | :622-634 | |
| resto (ApplyCompat…, PIX…, D3D10 layering) | six-slot pass-through `const void*×6` | :640-722 | ABI-safe x64 hasta 6 args |
| `EnsureHost` | — | :79-100 | una vez, en el primer call dxgi del juego (hilo del juego, pre-device) |
| `Real()` | `LoadLibraryW(System32\dxgi.dll)` | :56-64 | |

- **dxgi.dll SIEMPRE 38 forwarders** (regla R75). def: `src/host/dxgi.def`.
- Load hook: `InstallNgxLoadHook(self, host)` — load_hook.cpp (162 L), patrón OptiScaler: el LoadLibrary del DriverStore “nvngx.dll” cae en nuestro módulo; `_nvngx.dll` NO se reclama (el core lo carga interno y debe recibir el real).

## 2. Frontera dxgi → swapchain/factory proxies (COM)

| Interfaz | Base que posee | Alias | Present hook | Archivo:línea |
|---|---|---|---|---|
| `FactoryProxy : IDXGIFactory7` | `IDXGIFactory7* real` (creación) | — | creadores → `sc_proxy::Wrap` | dxgi_proxy.cpp:360-472 |
| `SwapChainProxy : IDXGISwapChain4` | `IDXGISwapChain1* base` (creación) | `IDXGISwapChain4* wide` (QI+Release, no-owning) | `Present`→`g_gate(base,…)`; `Present1`→`g_gate1` | :127-255, :293-309 |
| `Wrap()` | toma la ref de creación; QI falla → pasa sin envolver | | | :315-348 |
| Registro | `g_proxies[8]` + SRWLOCK, contador `g_live` | | | :260-291 |

- `QueryInterface` sirve el proxy para IUnknown/IDXGIObject/…/IDXGISwapChain4 (identidad COM); IID exótico → objeto real (:133-151).
- `Release()==0` → `delete this` (proxy muere con el objeto) (:153-160).
- `ResizeBuffers/ResizeBuffers1` se re-envían sin avisar al gate → el RTV cache lo absorbe por resource-pointer (ver §8). **Nota de riesgo**: tras un resize el formato puede cambiar sin que nadie lo invalide explícito — hoy lo salva el cache miss por puntero.

## 3. Frontera juego/driver → nvngx.dll (NGX proxy)

| Export | Firma normalizada | Comportamiento | Líneas |
|---|---|---|---|
| `NVSDK_NGX_D3D12_Init_Ext` | `(u64 app, wchar*, ID3D12Device*, NVSDK_NGX_Version, FeatureCommonInfo*)` | `DoInit` una sola vez, cachea resultado (evitó la tormenta de 1.2M retries) | nvngx_host.cpp:505-512, 474-501 |
| `…_Init`, `…_Init_ProjectID`, `…_Init_with_ProjectID` | variantes → normalizan a Init_Ext ABI (version ANTES de fcInfo) | :514-567 | |
| `…_CreateFeature` | `(ID3D12GraphicsCommandList*, Feature, Parameter*, Handle**)` | guarda `g_ssHandle` si SuperSampling | :569-584 |
| `…_EvaluateFeature` / `_EvaluateFeature_C` | `(cmdList, handle, params, callback)` | **ÚNICA intercepción**: `OfferFrameToSession` ANTES del forward; luego SIEMPRE evaluate nativo | :586-613 |
| `…_Shutdown` | | drena sesión (`host::Shutdown`) + forward | :631-635 |
| `…_ReleaseFeature`, GetCapabilityParameters, Get/Allocate/DestroyParameters, GetScratchBufferSize, GetFeatureRequirements, UpdateFeature | plain forwarders | :642-706 | |
| CUDA/VULKAN/D3D11/OTA family | `Host_Unsupported_Family` stub | log una vez, FAIL_FeatureNotSupported | :712-722 |
| `sli_PresentGate` / `sli_PresentGate1` | `long WINAPI(IDXGISwapChain*, UINT, UINT[, const void*])` | trampolines C que el dxgi resuelve por GetProcAddress | :618-629 |

- Core load: `CoreLoadDirect` vía `LdrLoadDll` nt-direct (bypass del load hook, sin recursión) :45-64; ruta por registry `NGXCore\FullPath` con fallback glob DriverStore :69-101.
- `OnInit` :306-355: log/crash-handler junto a NUESTRA dll; enablement = `sli_engine.exe` presente && sin `SLI_DISABLE`; `QTrack::Install(dev)`.

### 3.1 OfferFrameToSession (normalización de tipos) :360-465

- Recursos por `GetParamResource` (patrón typed→altKey→untyped) :288-304: `Color`/`DLSSD.Color`, `Depth`, `MotionVectors`, `Output`.
- Dims: desc del recurso → si `Width/Height` params existen → sobreescriben → si `DLSS_Render_Subrect_Dimensions_*` existen y ≤ dims → sobreescriben (:398-427).
- Subrect bases: `DLSS_Input_Color_Subrect_Base_X/Y`, `…_Input_Depth_…`, `DLSS_Input_MV_SubrectBase_X/Y` (:429-440).
- Escalares: `MV_Scale_X/Y` (default 1), `Jitter_Offset_X/Y`, `DLSS_Pre_Exposure` (≤0→1), `Sharpness`, `Reset`, `DLSS_Feature_Create_Flags` (:442-462).
- `QTrack::NoteEvaluateList(cmdList)` + `a.gameQueue = QTrack::SealQueue()` (:387-389).

### 3.2 QTrack (observer de cola) :153-273

- Hook de vtable slot 10 (`ExecuteCommandLists`) de `ID3D12CommandQueue` (clase compartida): `VirtualProtect` sobre la vtable con una probe queue; probe nunca se libera (la vtable vive) :224-250.
- `HookedECL`: trackea última cola DIRECT + reconoce las lists de evaluate (`g_evalLists[8]` rolling) → tras el real submit llama `host::OnListSubmitted(list, q)` por lista (:189-220). Shield try/catch.

## 4. Frontera proxy→sesión: `EvalArgs` (offload_session.h:36-64)

```
ID3D12Device* gameDev; ComPtr<ID3D12CommandQueue> gameQueue; ID3D12GraphicsCommandList* inCmdList;
NVSDK_NGX_Parameter* params; ID3D12Resource* srcColor/srcDepth/srcMotion/dstOutput;
unsigned inW,inH,outW,outH; colorBaseX/Y, depthBaseX/Y, motionBaseX/Y;
float mvScaleX/Y, jitterX/Y, preExposure, sharpness; bool gameReset; unsigned createFlags;
```
API: `Configure(int)`, `SetAppDataPath(wchar*)`, `Evaluate(EvalArgs)`, `PresentGate[1]`, `OnListSubmitted(void*, ID3D12CommandQueue*)`, `Shutdown()` (h:67-100).

## 5. Frontera host→engine: transporte cross-adapter + ABI

### 5.1 Recursos compartidos (MakeSharedBuf, offload_session.cpp:186-232)
- Heap `D3D12_HEAP_TYPE_DEFAULT` + `FLAG_SHARED | SHARED_CROSS_ADAPTER`, 64K-aligned; placed **BUFFER** ROW_MAJOR `ALLOW_CROSS_ADAPTER` (las placed TEXTURES las rechaza el driver); `CreateSharedHandle` NT (GENERIC_ALL).
- Buffers: `inColor` y `out` = `PitchFor(inW,bpp)*inH` (simétrico render) :1629-1635.
- Guías triple-buffer `guideD[3]/guideM[3]`: depth como **R32_FLOAT** (plano 0 del clone), MV en fmt crudo del juego (:1655-1679).
- Fences compartidas `produceFence/doneFence` (`SHARED|SHARED_CROSS_ADAPTER`) :234-242.

### 5.2 Mapping nombrado `Local\sli_frame_<pid>` (8192 B) — abi.h:151-164
| Offset | Struct | Tamaño | Contenido |
|---|---|---|---|
| 0 | `FrameScalars` | 56 (assert) | frame, jitterX/Y, preExposure, sharpness, mvScaleX/Y, reset, engineResult, renderW/H, **engineHostFrame** (u64: host-frame que el engine procesó) |
| 256 | `Handshake` | 144 (assert; ready@136) | magic, hic/hoc/hpf/hdf, hgd[3]/hgm[3], guideDepthFmt/MvFmt/Pitch/Pitch, guideW/H, depthInverted(=bit3 create-flags), version=ABI_VERSION 3, ready=READY_MAGIC |
| 2048 | `Tuning` | 112 (assert; nrTint@104, nrTestSplit@108) | offloadOn, capture, forceReset, overrides -1=game-wins, nr* knobs, nrOn, nrParamSeq, gpuIndex, nrStyle, nrWorkScale, nrBoost, nrTint (0/1/2), nrTestSplit (permille 0-1000) |

- Tuning es SIEMPRE-VALUE para knobs NR; floats <0 = "gana el juego".
- **El host re-escribe TODO el bloque Tuning en cada EvaluateInner** (memcpy a TUNING_OFFSET, :2162-2164) — el panel habla al host, el host es autoritativo.

### 5.3 Spawn (SpawnEngine :1481-1567)
- `sli_engine.exe --luid lo hi --w W --h H --cf fmt --map Local\sli_frame_<pid>`, `CREATE_SUSPENDED`, cwd = game dir; DuplicateHandle de TODOS los NT handles al hijo (aborta spawn si falla); handshake→ResumeThread.
- READY = sondeo de `hs->ready == READY_MAGIC` con timeout 30s (los pesos del modelo tardan segundos) :1744-1777.

## 6. Engine (GPU1) — loop.cpp / nr_vendor.h

### 6.1 Transporte abierto (OpenSharedTransport loop.cpp:122-268)
- OpenFileMapping→scalars; handshake version check; `OpenSharedBuf` = OpenSharedHandle(heap)+CreatePlacedResource CROSS_ADAPTER (:42-68).
- `texInColor/texOut` locales render-res fmt `cf`; `bufZero` UPLOAD con delta-0 bit-exacto (rgb=0,a=1 0x3C00 patrón :208-213) — todo path desarmado entrega CERO.
- Guías: 6 buffers + texturas locales en fmt RAW del juego (depth llega como R32_FLOAT convertido por el host).
- PitchFor = `(w*bpp+255)&~255` :37-40. FootprintFmtFor/Footprint: copias declaran el miembro FLOAT de la familia (:91-120).

### 6.2 RunFrameLoop (loop.cpp:298-633) — por frame
1. Espera `produce ≥ frame+1` (60s timeout sin avanzar contador; poison UINT64_MAX = exit) :366-389.
2. DRS guard: `renderW/H != args` → zero-delta (:398-407).
3. Knob rebuild: `nrParamSeq` cambió → `RebuildKnobs` (CREATE-time) :438-452.
4. Seal in: `bufInColor→texInColor` footprint rw×rh (:457-480).
5. `ProcessFrame` (UNA sesión GPU): guías buf→tex (frame%3) → encode → evaluate → decode-delta → tiles readback (:482-496). **Guía usada: `frame % 3`** — coherente con el sello del host N (produce N se señala tras el seal que escribe guide[N%3]; orden de cola lo garantiza) [R78 fix #1].
6. Delta out: `texOut→bufOut` UAV→COPY_SOURCE→copy→UAV (:514-542).
7. Fail-safe: `!computed → CopyBufferRegion(bufZero→bufOut)` (:546-556).
8. `engineResult=1; engineHostFrame = produce->GetCompletedValue(); Signal(doneFence, frame)` (:558-564) — **el tag del delivery es el frame del HOST**.
- Shields: todo frame envuelto en try/catch (std/int/…) → zero-delta + recrear cmd y seguir; el engine NUNCA muere por un frame roto (:569-631).

### 6.3 NrVendor (nr_vendor.h)
- **Armed path** (Init :437-623): registry NGXCore → `LoadLibraryEx(_nvngx.dll)` → `Init_Ext(app 101616311, cwd, dev, 0x15, null)` → `GetCapabilityParameters` → `discover_float_slot` (probe vtable 1..7 con round-trip Get :156-168) → forwarder `nvngx.dll_dlssnr.dll` → `dlssnr_call_create(18 args)`.
- Texturas modelo: `texColor` BGRA8 sRGB work-res (RGBA16F INPUT CRASHEA 0xC0000409 — validado sm_86), `texOutModel` RGBA16F work-res :524-529. `workW = clamp(render*workScale, 64..render)` :452-459.
- Codec heap 6 slots: 0 SRV texIn(engine RGBA16F), 1 UAV BGRA8, 2 UAV tiles, 3 SRV texOutModel, 4 SRV texIn, 5 UAV texOut(engine) :365-429. UAV bufTiles con StructureByteStride=4 y Format=UNKNOWN (RWStructuredBuffer) :409-417.
- `ProcessFrame` :642-773:
  - guías: footprint RAW fmt + pitch; SRV→COPY_DEST→copy→SRV (:657-678).
  - **encode** (psoEnc, rs 3 params: 7 consts + SRV t0 + UAV u0/u1): dispatch WORK grid; exposure = `expoScale` del frame anterior; box-average exacto src full→work cuando workScale≠1 (:682-699; shader nr_encode.hlsl:39-96).
  - **evaluate** `fEval(cmd, feature, P, texColor, texGuideDepth, texGuideMV, texOutModel, workW, workH, guideW, guideH, guideW, guideH, 0,0,0,0, depthInverted, reset, kIntensity, kStyle, kLocalStructure, kLocalTone, kSkinStructure, 1, mvScaleX*workScale, mvScaleY*workScale)` (:713-721) — MV a dominio work por producto (fork-exact [R78-OK]).
  - **decode-delta** (psoDelta): t0=modelDisp(work), t1=origHDR(render), u0=delta(render); `d = modelDisp − enc(orig)`; guard saturación `enc≥0.985→d=0`; clamp ±1; boost (min 1); tint==1 → pintado por signo amp 20·boost, alpha=2 (:738-756; shader nr_delta.hlsl:42-105). Bilinear corner-to-corner work→render :49-73.
  - tiles→readback; texInColor→COMMON.
- `UpdateExposure` :777-788: mediana CPU (nth_element ~5k tiles) → `expoScale = 0.18/med` si med>1e-4 (hold implícito). Lag 1 frame por construcción. **tile-grid ahora a WORK dims** (tilesX=(workW+7)/8, :347-348) [R78 fix #2].
- `RebuildKnobs` :803-843: release+create con knobs nuevos, `seq=0` (reset primera evaluate).

### 6.4 Forwarder (nvngx.dll_dlssnr.dll, externo)
- ABI C: `dlssnr_call_create(snip, cwd, dev, cmd, P, w, h, preset, intensity, style, localStruct, localTone, skinStruct, autoMask, uiCorrection)`; `dlssnr_call_evaluate_v2(28 args)`; `dlssnr_call_release(feature)`; `dlssnr_call_set_float_slot(int)`.
- Snippet path: `nvngx_dlssnr.dll` (165 MB, user-provided, NUNCA al git).

## 7. Present gate (host, game thread) — offload_session.cpp:2234-2647

- `PresentGateInner` :2458-2570: fast-path si degraded/off/not-LIVE/TEST-flag/N==0/yá compuesto; FrameGuard; **wait loop**: re-check estado+generación cada vuelta; done==UINT64_MAX→FencePoison+degradan; `done>gateLastDone` → mira `scalars->engineHostFrame`: `==N` → break (delta N en bufOut); `>N` → native presente (anomalía, no mezclar); espera evento `done+1` + handle proceso (250ms); engine muerto→degradar; watchdog 10s→FailStreak+degradar.
- `ComposeStageDraw` :2277-2455:
  1. bbIndex = `GetCurrentBackBufferIndex` (SC3) o 0 (legacy); `GetBuffer`; desc → fmt/W/H.
  2. `RtViewFor` cache 4 slots por resource-pointer (resize-safe; overflow = drop set) :1174-1203. `PsoFor` cache 4 PSOs por formato :1206-1223.
  3. **rtv/pso miss = skip frame + retry (NO degradación permanente)** :2300-2308 [R80 fix].
  4. Stage delta: `out.buf (COMMON→COPY_SOURCE) → texDelta (PSR→COPY_DEST)` footprint RGBA16F pitch 8·W :2330-2362.
  5. (1b) merge estructural SOLO tint==2 y `EnsureStruct` (lazy, fail-soft) :2364-2392.
  6. Draw: bb PRESENT→RT; viewport/scissor W×H; OMSetRenderTargets; heap; GateCb {cBoost, cTint(0/1/2), cTestSplit(≤1000), cOutW=W}; tabla t0; DrawInstanced(3) fullscreen triangle; RT→PRESENT :2394-2430.
  7. Close→Execute en **la cola del juego** (`gameQueueForFlush`, fallback propia + log una vez) :2432-2438; fence propia + **wait CPU 2s** (timeout = degradar) :2439-2448.
- Shields PresentGate/1 try/catch → GateDegrade + ForwardPresent (:2609-2647).

## 8. Compose GPU (InitCompose :923-1041 + shaders)

- `texDelta`: RGBA16F render-res SRV-only (PSR inicial).
- RS gráfica: `[0] 4×32bit consts b0 | [1] tabla SRV t0+t1 (¿2 SRVs? ver BUG-1) | static sampler s0 LINEAR/CLAMP` :946-975.
- Heap shader-visible **3 slots**: [0] SRV delta, [1] null-SRV RGBA16F (t1 por defecto; EnsureStruct escribe el real), [2] UAV struct :978-1011.
- Cola/alloc/cmd/fence propios (DIRECT) :1014-1030.
- `present_add.hlsl` (87 L): VS fullscreen por SV_VertexID; PS: `v = delta.rgb*boost`; `cTint==1` → raw passthrough; `cTint==2` → pinta `gStruct.Sample` por signo amp 24·boost ×32; split: `uv.x ≥ cTestSplit/1000 → v=0` + divisor 2px 0.25; isfinite guard; alpha out 0 (ADD-0 preserva alpha del backbuffer).
- `struct_accum.hlsl` (45 L): CS 8×8; `gStruct = cReset ? delta : lerp(gStruct, delta, 0.10)`; finite guards; prime por flag (DEFAULT-heap garbage se reemplaza).
- MakeComposePso :1135-1170: blend ADITIVO ONE/ONE ADD rgb+alpha, write ALL, sin depth/stencil, topology TRIANGLE, RTVFormats[0]=fmt del backbuffer, SampleDesc.Count=1.

## 9. FSM sesión (una sesión, un drain)

- Estados Idle/Arming/Live/Draining/Cooldown + Reason {GpuIndex, WorkScale, Drs, LateGuides, Recycle, EngineDead, FencePoison, FailStreak, ReadyTimeout, Transport, Shutdown} :128-163.
- `DoArm` :1578-1781: snapshot Arm→transport→guidas→depthClone(idéntico desc DSV)→mvRing[8]→InitCompose→doneEvt→SpawnEngine→READY wait→Live. Fallo = ReleaseTransport + cooldown 2s.
- `DoDrain` :1841-1928: Draining→esperar done≤produced (2s)→poison produce→esperar/matar por HANDLE→game thread fuera (g_frameBusy 3s)→flush cola del juego con fence propia→ReleaseTransport.
- `PollHealth` :1933-1984: reconfig pendiente, engine muerto, fence poison (TDR), **recycle WS>5GB** (leak ~10MB/s del runtime vendor).
- Evaluate entry :2076-2232: arm-snapshot si Idle; FrameGuard; offload A/B; DRS guard; gameQueueForFlush; tuning→mapping; health (done/UINT64_MAX/proceso); scalars del frame N; RecordSealFrame; pending{N, list}.
- `OnListSubmitted` :2572-2607 (game SUBMIT thread, sin session mutex): si list==pending → push sealCmd en la MISMA cola → Signal(produce, N) GPU-ordenado.

## 10. HALLAZGOS DE ESTE ANÁLISIS (divergencias/bugs propios)

- **BUG-1 (CRÍTICO — el "fmt 87" en bucle)**: `present_add.hlsl` declara `t0` Y `t1` (:22-23), pero la RS gráfica declara UN range de **1** SRV (:947-949) → `CreateGraphicsPipelineState` E_INVALIDARG para TODO formato (no solo 87; 87 = `B8G8R8A8_UNORM`, el fmt del swapchain del juego). Consecuencia: 0 composes, log spameado. Fix pendiente: count 1→2.
- **BUG-2 (latente, tint=2)**: CS `struct_accum` tabla = heap-start con ranges OFFSET_APPEND → SRV→slot0 (delta) pero UAV caería en **slot 1** (el null-SRV) en vez del slot 2 diseñado (:1089-1093 + heap layout :979). Fix: UAV range con offset explícito 2.
- **BUG-3 (menor, logging)**: "rtv/pso miss" y "compose pso failed" se loguean por frame sin rate-limit (spam 10 L/s) — la versión desplegada aún loguea el par completo.
- **Deuda documentada [R78]**: forceReset no resetea NADA del lado host (ya no hay hist que resetear — moot tras R79); sliders panel jitter/mvScale placebo; `preExposure` del juego ignorado (grep: nadie lo consume en engine; la E propia se ancla a mediana).
- **Riesgos de diseño a vigilar**:
  - El gate compone en la cola DEL JUEGO con wait CPU 2s dentro de Present — si esa cola se atasca por el propio juego (no por nosotros) degradamos permanente sin culpa nuestra; el watchdog de 10s cubre el caso engine.
  - `ComposeStageDraw` asume backbuffer 1 sample (SampleDesc.Count=1) — un swapchain MSAA rompería el PSO (fail-soft por frame tras BUG-3 fix, pero eterno).
  - La vista tint=2 NO pasa por split (early return en PS) — verificación solo.
  - `engineHostFrame` se lee del mapping escalares sin barrera de memoria: el engine lo escribe ANTES de Signal(done) y el host lee DESPUÉS del wait — orden correcto por fence (x86 TSO).

## 11. Tipos en fronteras (resumen una línea)

- Game↔dxgi: COM IDXGIFactory7/IDXGISwapChain4 (proxies), HWND, DXGI_SWAP_CHAIN_DESC1, DXGI_PRESENT_PARAMETERS.
- Game↔nvngx: NVSDK_NGX_* (Result, Parameter, Handle, Feature, Version, FeatureCommonInfo, ProgressCallback[_C]), ID3D12Device/CommandList.
- dxgi↔nvngx: `sli_PresentGate(IDXGISwapChain*,UINT,UINT)`, `sli_PresentGate1(…,const void*)` por GetProcAddress.
- Host↔Engine: NT handles (heaps/fences) via DuplicateHandle; mapping `Local\sli_frame_<pid>`: FrameScalars(56)+Handshake(144)+Tuning(112); fences compartidos produce/done (u64 counters); ZERO-delta = patrón 8B {0×7,0x3C}.
- Engine↔vendor: P-block vtable (slots: SetULL@0, SetUInt@3, SetFloat@n, GetFloat@n+8), forwarder C ABI (create 18 / evaluate_v2 28 / release / set_float_slot).
- GPU: DXGI formats — color sellado = fmt crudo del juego (RGBA16F en RDR2), modelo BGRA8 sRGB work-res, delta RGBA16F display-domain [-1,1], backbuffer B8G8R8A8_UNORM (87), depth guía R32_FLOAT, MV fmt crudo (R32G32 SINT familia→R32G32_FLOAT en footprint).
