# Catálogo de algoritmos OptiScaler (leído del código) y plan de backends para new-sli

Fuentes inspeccionadas (código real, no docs): `D:\proyectos\OptiScaler-upstream` (commit 6ec6681, v0.7.8, repo github.com/optiscaler/OptiScaler), `D:\proyectos\Streamline-2.14.1-sdk`, `D:\proyectos\renodx`. Todo lo listado existe en el código; lo que no aparece no se lista (p.ej. **no hay NIS ni CAS como backend** en OptiScaler; NIS existe solo como plugin `sl.nis` en Streamline).

## 0. Arquitectura observada (el patrón a copiar)
- Se adjunta como `winhttp.dll` / `dxgi.dll` / `d3d12.dll` (`dllmain.cpp` despacha por nombre de fichero) **o directamente como `nvngx.dll`/`_nvngx.dll`** exportando la API NGX completa: `inputs/NVNGX_DLSS_Dx12.cpp` implementa `NVSDK_NGX_D3D12_CreateFeature` y `NVSDK_NGX_D3D12_EvaluateFeature`.
- El evaluate despacha: si DLSS nativo activo → passthrough por `NVNGXProxy` al nvngx real (`inputs/NVNGX_DLSS_Dx12.cpp:1140`); si no → `State::currentFeature->Evaluate()` (backend activo). Cambio de backend en caliente: `FeatureProvider_Dx12::ChangeFeature` + `State::changeBackend`.
- Cada backend = clase `IFeature{_Dx12,_Vk,_Dx11}` con `InitInternal/EvaluateInternal` (mapea `NVSDK_NGX_Parameter*` → struct del SDK); factory = `switch (enum Upscaler)` en `upscalers/FeatureProvider_Dx12.cpp`. Fallback universal: FSR 2.1.2.
- Los backends no-NVIDIA se cargan de dos formas: DLL pública + `GetProcAddress` (`proxies/XeSS_Proxy.h`, `proxies/FfxApi_Proxy.h`) o import-libs AMD enlazadas (`ffx_fsr2_api_x64.lib`, `ffx_fsr3upscaler_x64.lib`, etc. en `OptiScaler.vcxproj`).
- Añadidos propios: RCAS/MAS sharpening y Output Scaling (shaders HLSL en `shaders/`) aplicables a cualquier backend.

## 1. Backends SR (dir `upscalers/`, enum `Upscaler` en `OptiTypes.h`)

| Algoritmo (enum) | Vendor / requisito | APIs | DLL / runtime (entrada) | Madurez en OptiScaler |
|---|---|---|---|---|
| DLSS (`dlss`) | NVIDIA Turing+ (`dlssCapable` = arch >= TU100, GTX16xx vía spoofing, `IdentifyGpu.cpp:307`) | DX12, Vulkan, DX11 | `nvngx.dll`/`_nvngx.dll` → `nvngx_dlss.dll`; `NVNGXProxy::D3D12_CreateFeature` con `NVSDK_NGX_Feature_SuperSampling`. Soporta presets override (DLSS 3.7+) | Maduro |
| DLSSD (`dlssd`) = DLSS-D / Ray Reconstruction | Solo NVIDIA | DX12, Vulkan, DX11 | `nvngx_dlssd.dll`, `NVSDK_NGX_Feature_RayReconstruction` (`DLSSDFeature_Dx12.cpp:52`) | Maduro |
| FSR 2.1.2 (`fsr21`/`fsr21_12`, dir `fsr2_212`) | AMD SDK, corre en cualquier GPU | DX12, Vulkan, DX11 (+on12, VkOnDx12) | `ffx_fsr2_api_x64.dll` + `ffx_fsr2_api_dx12_x64.dll` (import-libs linkadas); init `ffxFsr2GetInterfaceDX12`/`ffxFsr2CreateContext`, eval `ffxFsr2Dispatch` | Muy maduro (es el fallback) |
| FSR 2.2.1 (`fsr22`, dir `fsr2`) | ídem | ídem | ídem | Maduro |
| FSR 3.1 (`fsr31`, dir `fsr31`) | AMD ffx-api | **DX11 nativo** (solo `FSR31Feature_Dx11.cpp`; usa `ffx_upscale.h` de ffx-api) | módulos ffx-api | Maduro |
| FFX (`ffx`/`ffx_12`, dir `ffx`) = FSR 2.3/3.1/4.x vía FidelityFX SDK v2 (ffx-api) | AMD SDK, cualquier GPU (FSR4 solo RDNA4) | DX12, Vulkan, DX11On12, VkOnDx12 | `amd_fidelityfx_loader_dx12.dll`\|`amd_fidelityfx_dx12.dll` + `amd_fidelityfx_upscaler_dx12.dll` (VK: `amd_fidelityfx_vk.dll`); entrada `ffxCreateContext` vía `FfxApiProxy::D3D12_CreateContext` (`FFXFeature_Dx12.cpp:671`) | Maduro (backend FSR actual) |
| FSR 4 (dir `fsr4/`) | **Solo AMD RDNA4** (FP8/INT8, `FSR4Support`), forzable por config | DX12 (sobre vía FFX) | **No es backend separado**: `FSR4Upgrade` hookea `amdxc64.dll` (interfaz external-provider del driver) y sirve el modelo ML de `amdxcffx64.dll` cuando el contexto FFX upscaling lo pide | Experimental/automático |
| XeSS (`xess`/`xess_12`, dir `xess`) | Intel SDK; corre en cualquier GPU vía DP4a (XMX solo Arc; spoofing de VendorId para que el juego active XMX) | DX12, Vulkan, DX11 (`libxess_dx11.dll`) | `libxess.dll`; entrada `xessD3D12CreateContext/BuildPipelines/Init/Execute`, `xessSelectNetworkModel` (todo por GetProcAddress, `XeSS_Proxy.h:263+`). XeSS 2.1 (changelog) | Maduro |
| RCAS+MAS, Output Scaling (FSR1/Bicubic/CatmullRom/Lanczos/Kaiser) | propios (sin vendor) | DX12/DX11/Vk | shaders HLSL propios (`shaders/rcas`, `shaders/output_scaling/fsr1`) | Maduro (no son upscalers: post-pass) |

## 2. Frame Generation (dir `framegen/`, enums `FGInput`/`FGOutput`/`FGNvngxReplacement` en `State.h`)

| Opción | Qué hace | Requisitos | Riesgo anti-cheat |
|---|---|---|---|
| OptiFG (`FGInput::Upscaler`) | FG propio dentro del evaluate del upscaler (`inputs/FG/Upscaler_Inputs_Dx12.cpp`); swapchain capturada + hudfix + resource-tracking (Config `FG*`) | cualquier GPU; necesita hudfix para UI | Medio-alto: hooks de swapchain/Present y copias HUD; experimental (changelog) |
| DLSS-G vía Streamline (`FGInput::DLSSG`) | DLSS-G real: `sl.interposer.dll` + `sl.dlss_g.dll` (`StreamlineProxy`, `DLSSG_Dx12.cpp`) | NVIDIA RTX 40+ para FG real; SL en el juego o instancia propia | Bajo (driver-level), pero requiere path SL |
| DLSS-G vía Nvngx (`FGInput::NvngxFG`) — expone la API NGX `DLSSG_NVSDK_NGX_D3D12_*` para que el juego crea que es DLSS-G. Submodos (`FGNvngxReplacement`): **None** = DLSSG real (RTX 40+); **Nukems** = `dlssg_to_fsr3_amd_is_better.dll` (mod dlssg-to-fsr3 de Nukem9); **Arturs** = `dlss-enabler-headless.dll` (DLSS Enabler, FSR3 MFG); **FFX** = FSR 3/4-FG nativo ffx-api (`amd_fidelityfx_framegeneration_dx12.dll`) con swapchain SL; **Combo** = FFX+Enabler según nº de frames falsos | según modo (FSR3: cualquier GPU) | Medio: sustituye la DLL que el juego espera y gestiona swapchain |
| FSR 3.1 FG (`FGInput::FSRFG`) y FSR 3.0 FG (`FSRFG30`) | FG ffx-api con reemplazo de swapchain (`wrapped_swapchain`, `hooks/FG_Hooks.cpp`, `FSRFG_Dx12.cpp`) | cualquier GPU, DX12 | Medio: reemplazo de swapchain + Present hook |
| XeFG (`FGInput::XeFG`/`FGOutput::XeFG`) | FG de Intel XeSS 2.x: `libxess_fg.dll` (`XeFG_Proxy::InitXeFG`, API `xefg_swapchain`), con XeLL (latencia) integrado (`low_latency/`) | cualquier GPU (público desde XeSS 2.x), DX12 | Medio: swapchain propia |
| Latencia asociada: Reflex (nvapi64), Anti-Lag 2 (SDK AMD), LatencyFlex (LFX), XeLL (`main.dll`) | input/latencia, no FG | según vendor | bajo |

Nota para RDR2: no integra FG nativo; cualquier FG exige el layer de swapchain descrito (fase 2 de new-sli).

## 3. NR / RR landscape
- **DLSS-D / Ray Reconstruction** = OptiScaler `dlssd` (NGX `NVSDK_NGX_Feature_RayReconstruction`, `nvngx_dlssd.dll`; Streamline `kFeatureDLSS_RR=1001`, plugin `sl.dlss_d`). Reemplaza upscaler **y** denoisers en juegos con RT/path-tracing. Solo NVIDIA (mismo gate `dlssCapable`). Permite preset override.
- **DLSS_NR**: solo existe como feature ID en Streamline 2.14 (`kFeatureDLSS_NR=1004`; el NRD clásico está `NRD_INVALID`/removed). **OptiScaler no implementa DLSS_NR** y no hay binario `nvngx_*nr*` en `bin/x64`. No listarlo como backend disponible.
- **Quién usa RR**: solo títulos que integran NGX RR o SL (`sl.dlss_d`/DLSS_RR). En el hook de OptiScaler el FeatureID RayReconstruction se enruta al backend DLSSD o cae al fallback FSR 2.1.2 (`inputs/NVNGX_DLSS_Dx12.cpp:770` solo acepta SuperSampling y RayReconstruction).
- No existe denoiser no-NVIDIA cableado como backend equivalente a RR: el módulo denoiser de ffx-api (`amd_fidelityfx_denoiser_dx12.dll`) se carga en `FfxApiProxy` pero no es un backend NR autónomo.

## 4. Integración para new-sli (C++ puro, sin Python, manteniendo el proxy nvngx)
Ya interceptáis `CreateFeature/EvaluateFeature` → el patrón OptiScaler encaja 1:1: interfaz propia `IBackendSR {Init(device,params); Evaluate(cmdList,params); Shutdown()}` + factory por enum + fallback FSR2 + cambio en caliente.
1. **DLSS nativo**: ya lo tenéis (passthrough al nvngx real). Cero trabajo.
2. **FSR2 (2.1.2/2.2.1)** — primer backend no-NVIDIA recomendado: headers+DLLs públicas AMD; init `ffxFsr2GetScratchMemorySizeDX12`/`ffxFsr2GetInterfaceDX12`/`ffxFsr2CreateContext`, eval `ffxFsr2Dispatch` mapeando `NVSDK_NGX_Parameter*`→`FfxFsr2DispatchDescription` (patrón exacto en `FSR2Feature_Dx12.cpp`).
3. **FSR 3.1/4 vía ffx-api** (FidelityFX SDK v2): `amd_fidelityfx_loader_dx12.dll`/`amd_fidelityfx_dx12.dll` + `amd_fidelityfx_upscaler_dx12.dll`; misma cubierta sirve para FG en fase 2 (`amd_fidelityfx_framegeneration_dx12.dll`).
4. **XeSS**: `libxess.dll` por GetProcAddress (`xessD3D12CreateContext/Init/Execute`); corre en las 2x RTX 3060 por DP4a. XeSS 2.x da acceso futuro a XeFG.
5. **DLSS-D (RR)**: passthrough de `Feature_RayReconstruction` — en vuestro offtload podéis crear el contexto NGX en el device de la GPU secundaria (ya hacéis SR+NR allí).
- **Clave multi-GPU**: OptiScaler NO hace offtload — todo corre en el device del juego. Vosotros debéis crear el contexto del backend en el `ID3D12Device` secundario y copiar recursos cross-adapter (shared heaps); ese es vuestro valor añadido, no código copiable de OptiScaler.
- **FG (fase 2)**: requiere capa de swapchain (Present hook + hudfix tipo `hudfix/` + pacing). Planearlo como módulo separado; nada de FG es alcanzable solo desde el evaluate del upscaler salvo OptiFG (que también usa swapchain).
- No necesarios para new-sli: spoofing DXGI/VK (ya sois proxy NGX), menú ImGui, DX11/Vk (vuestro target es DX12).

## 5. Licencias
- **OptiScaler: LICENSE = GPL-3.0** (premisa 'MIT' corregida). Copiar sus ficheros obliga a GPL-3.0 para new-sli público; reimplementar el patrón (interfaces/factory propias) es seguro.
- **AMD FidelityFX SDK** (FSR2/FSR3/ffx-api, AntiLag2 headers): MIT (changelog 'Added FidelityFX license'; submodule `external/FidelityFX-SDK*` vacío en este checkout — verificar fichero al sincronizar).
- **Intel XeSS/XeFG/XeLL**: MIT (submodule `external/xess` vacío aquí — verificar).
- **NVIDIA NGX/DLSS SDK y binarios** (`nvngx_dlss.dll`, `nvngx_dlssd.dll`, `nvngx_dlssg.dll`, plugins SL): 'NVIDIA RTX SDKs LICENSE' (`bin/x64/nvngx_dlss.license.txt`): distribución solo en formato objeto integrado en una aplicación y sujeta a sus requisitos; no redistribución suelta. Headers NGX (`external/nvngx_dlss_sdk`) bajo esos mismos términos de SDK.
- **Streamline SDK** fuente: MIT-style permission notice (`license.txt`, © NVIDIA 2023); **renodx**: MIT (Carlos Lopez). LatencyFlex: MIT.

## 6. Riesgos de publicación (new-sli público)
- **NGX ToS**: interponer `nvngx.dll` y re-enrutar a backends no-NVIDIA es el modelo OptiScaler (distribuye código, nunca DLLs vendor). El **bypass de la firma de DLSS 3.7+ (método de Artur)** es zona legalmente sensible: no replicarlo; passthrough del nvngx real basta.
- **No redistribuir** `nvngx*.dll`, `sl.*.dll`, `amd_*.dll`, `libxess*.dll` en releases: que el usuario las aporte (del juego/driver/SDK), como hace OptiScaler.
- **GPL**: si importáis código de OptiScaler, new-sli hereda GPL-3.0 (ya es público — decidid explícitamente). Alternativa limpia: reimplementar el patrón y citar como inspiración.
- **FG y anti-cheat**: swapchain hooks + frames interpolados pueden disparar AC en online; mantener FG opt-in, documentado para SP (RDR2 offline OK).
- **Marcas** ('DLSS', 'FSR', 'XeSS'): incluir disclaimer de no afiliación (renodx lo hace explícito).
- **Spoofing de VendorId**: innecesario para vosotros (ya proxy NGX); evitadlo reduce fricción legal/técnica.
