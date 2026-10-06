# 08 — Catálogo de bugs (síntoma → causa raíz → fix)

Catálogo completo de los bugs de la integración. Formato: qué se VEÍA, qué ERA,
y la lección. Ordenados por tema.

## Bridge / protocolo

### B01 — "identity con coste" (pocfe heredado)
- **Síntoma**: frames devueltos idénticos pero con latencia de proceso.
- **Causa**: el runner procesa pero el resultado no se recoge (seq/resultSeq
  desalineados) o el runner no vio el evento.
- **Lección**: el handshake seq/resultSeq es la única verdad; los eventos
  auto-reset retienen como mucho un wake.

### B02 — ~15 runners zombie (RAM varios GB)
- **Síntoma**: tras un crash del engine, lista de procesos llena de pythons.
- **Causa (2)**: el host pasaba `--nr <fwd>` (literal con espacio, por eso rg no
  lo veía) que el engine R54 ya no parseaba → 'bad args' → bucle muerte/re-arma
  → `SpawnNrRunner` en cada re-arm.
- **Fix**: bloque `--nr` fuera del host + guard `NrRunnerAlive()` (mapping
  `pocnr_bridge2` + magic PCFE vivos → no spawn).
- **Lección**: matar SIEMPRE por PID; revisar procesos tras cada sim/bench
  (los zombies inflan timings posteriores).

## D3D12 / command list

### B03 — pantalla negra: throw 6 en cada frame (R54 deploy)
- **Síntoma**: juego arrancado, HUD vivo, frame 3D negro. Log: 977+ throws.
- **Causa**: doble Reset del cmd list — el segmento 2 abre grabación con su
  Reset y el bloque pocnr hacía OTRO Reset con la lista grabando → E_FAIL.
- **Fix R56**: readback graba en la lista YA abierta; Reset solo tras SubmitSeg.

### B04 — `E_INVALIDARG` en Close del segmento inputs (R57)
- **Síntoma**: `seg inputs close failed 0x80070057` en cada frame.
- **Causa**: overread de 88 bytes — `pocnrBytes = pitch×h = 2.488.320` pero el
  buffer fuente mide `pitch×(h−1)+w×bpp = 2.488.232`. `CopyBufferRegion` leía
  más de lo que hay.
- **Fix**: copy textura→buffer (footprint con dims reales). El sim NO podía
  cazarlo: no tiene buffers D3D12 reales.

### B05 — `E_INVALIDARG` del copy con footprints en ambos lados (R59)
- **Causa**: `CopyTextureRegion` con `PLACED_FOOTPRINT` en AMBOS extremos =
  buffer→buffer, forma inválida (un lado debe ser textura).
- **Lección**: citar el "patrón pocfe probado" de memoria sin releer — el patrón
  real era buffer→textura. Releer SIEMPRE antes de citar.

### B06 — SR evaluate `0xBAD00002` + engine zombi (R54/R56)
- **Síntoma**: con el bloque NR activo, el snippet SR rechazaba el frame; el
  engine quedaba colgado; el juego corría por fallback nativo.
- **Causa**: tras `SubmitSeg("pocnr-rb")` la lista quedaba CERRADA; en frames
  identity no había Reset → el snippet SR recibía lista cerrada → la rechazaba
  (0xBAD00002 = caller gate) → nuestro Close() sobre lista cerrada → E_FAIL
  0x80004005 → throw → device envenenado → zombi.
- **Fix R60 (regla estructural)**: **Reset fresco SIEMPRE antes del socket/evaluate,
  venga el camino que venga**. Cada bloque su Reset+submit.

### B07 — `pocnr-rb close failed 0x80004005` cada frame (R63 deploy)
- **Síntoma**: juego corre sin marca NR (fallback nativo).
- **Causa**: en la reordenación R63, el bloque readback+fire del FINAL del frame
  grababa sin Reset (la lista quedó cerrada tras el submit de inputs). La regla
  B06 se me escapó en el bloque nuevo.
- **Fix R63b**: Reset antes de grabar el readback final.
- **Lección**: la regla "Reset antes de grabar" es por BLOQUE, no por frame.

## Tuning / spawn

### B08 — `nrWant` nunca true (0 frames pocnr)
- **Síntoma**: stack vivo, bridge open, runner cargado — pero 0 frames procesados.
- **Causa**: `TuningDefaults()` (nrOn=0) en cada re-arm + iniApplied static →
  el re-arm DRS machacaba el nrOn=1 del ini.
- **Fix R55**: defaults SOLO en la primera armadura del proceso.

### B09 — anti-stall mataba el boot del runner
- **Causa**: el runner tarda ~10 s en cargar pesos; 10 frames de identity
  (timeout de 3000 ms agotado con runner muerto-anterior) contaban como muerte.
- **Fix**: gate `RunnerAlive()` — mientras bootea, NR espera gratis; el streak
  solo cuenta timeouts con runner VIVO.

## Runner / torch

### B10 — "NR todo blanco"
- **Causa**: contrato roto — el runner mandaba u8 en vez de float32 [0,1].
  El NR espera display-referred [0,1]; el frame del juego es lineal HDR fp16.
- **Fix**: display transform + inversa exacta alrededor del NR.

### B11 — CUDA Graphs roto por H2D pageable
- **Causa**: el tensor de swizzle se copiaba H2D POR LLAMADA desde memoria
  pageable → la captura del grafo reventaba.
- **Fix**: cache del tensor por device (`_SWIZZLE_INDEX_CACHE`).

### B12 — fidelidad GPU residente: 1 cast perdido
- **Síntoma**: features 3e-5 de diff → head 1.69 (el transformer amplifica).
- **Causa**: `scaled_color` hace 3 casts half (`half(c)`, `-0.5`, `×0.125`);
  la vía GPU se comió el primero. 1 ULP de f16 subnormal amplificado.
- **Fix**: los 3 casts exactos; validación 0.0.

### B13 — crash 0xC0000005 en trt_run
- **Causa**: `c_char_p(host_bytes)` liberado antes del memcpy asíncrono +
  firma de la API cuda-python distinta a la ctypes cruda.
- **Fix**: `create_string_buffer` retenido, memcpy síncrono.

## Herramientas / entorno

### B14 — wmic CSV UTF-16 vacío
- **Fix**: `wmic /format:list` con decode utf-16-le, o PowerShell `Get-Process`.

### B15 — parcheo incremental corrompe ficheros
- **Síntoma**: replay.cpp con contenido duplicado; funciones desalineadas;
  SetupCore borrada por error.
- **Causa**: find/replace con strings frágiles + CRLF (ficheros \r\n, strings
  \n) + indentación + comillas escapadas del read_file JSON.
- **Fix sistemático**: `git checkout --` + re-aplicar por anclas exactas de HEAD
  / cirugía por líneas con verificación tras cada paso (orden de bloques,
  balance de llaves, compile).

### B16 — protobuf "Failed to serialize proto" (ONNX)
- **Causa**: initializers >2 GB embebidos (Constant de tamaño de frame).
- **Fix**: cirugía ONNX (ver [06-tensorrt-engine.md](06-tensorrt-engine.md)) — escalares +
  external data manual.

### B17 — venv sin pip
- **Fix**: `uv pip install --python <venv>\Scripts\python.exe`.

## R69 — guías reales

- **WRL ComPtr `operator&` = `ReleaseAndGetAddressOf()`**: construir arrays
  con `&comPtr` libera el recurso. Usar `.Get()` SIEMPRE que se quiera un
  puntero crudo. Este bug produjo "guías a cero" durante toda la ronda.
- **Grabar en la lista del juego después del evaluate** (barriers sobre sus
  texturas) corrompe el device a minutos: el runtime NGX nativo graba sus
  propias transitions ahí. Síntoma: ERR_GFX_D3D_DEFERRED_MEM / crash del anti-tamper /
  nvwgf2umx.dll AV. Regla: nuestras grabaciones van en cmd list PROPIA
  sometida tras la del frame (hook de ECL).
- **CopyTextureRegion prohíbe src ALLOW_DEPTH_STENCIL**: la depth DSV del
  juego no se puede copiar a buffer. La MV (ALLOW_RENDER_TARGET) sí.
  Alternativas: pasar el resource directo (fork), compute shader read-only
  (pendiente), o stub.
- **done-wait con mutex tomada = deadlock potencial**: si el señalizador
  (hook de submit) necesita la misma mutex, el juego entero se congela.
  Nunca bloquear el thread de evaluate esperando GPU; delay-line.
- **Close() de cmd list recién creada**: `Reset()` exige lista CERRADA; una
  lista nueva nace en recording → cerrarla vacía al crearla antes del ciclo
  Reset/graba/Close.
- **Filtro de logs por string de hora cruza sesiones/días**: parsear el
  timestamp numérico o cortar por markers de sesión ("Offload: enabled").

## R69u — delay-line que nunca entrega

- **Condición de entrega vs token que avanza**: exigir `done >= pendingFrame`
  cuando `pendingFrame` se reescribe cada frame y el productor es más lento
  (50 ms vs 16 ms) = condición imposible para siempre: el resultado se
  descarta SIEMPRE y el consumidor nunca se activa (`nrActive` jamás on).
  Síntoma engañoso: todo "funciona" (ENHANCED, sin throws) pero el usuario
  no ve NI marca NI cambio en el A/B. Fix: comparar contra el último done
  ENTREGADO (`lastDeliveredDone`), no contra el último solicitado.
- **"Procesa" ≠ "Entrega"**: un pipeline puede procesar perfecto y ser
  invisible si la vuelta al consumidor nunca corre. Verificar la entrega
  con una marca visual deliberada (badge) en cada integración.
- **UX panel**: estados esperados (host aún no armado) no llevan MessageBox
  modal — status bar / persistencia silenciosa.

## R69v — CS sobre DSV y las hipótesis en cadena

- **SRV planar fmt21 sobre una DSV en DEPTH_WRITE = device removed al
  ejecutar**; con barrier correcto el dispatch PASA pero **lee ceros** en
  el driver 616.64 — un CS "legal" puede ser funcionalmente inútil. Solo
  un repro con contenido verificado (clear 0.42 → readback) distingue
  "legal" de "funciona".
- **CopyResource a un clon IDÉNTICO (fmt crudo + mismo flag DSV) sobre la
  DSV del juego: PASS con datos** — la "prohibición spec" de R69t era un
  artefacto del footprint planar completo (Close E_FAIL), no de copiar la
  DSV. Copiar el plano 0 a footprint R32_FLOAT después: PASS.
- **El patrón que cierra el círculo**: clonar en el seam del evaluate con
  barriers neto-cero (lo que llega al runtime nativo debe encontrar el
  estado tal cual lo dejó el juego) + extraer del CLON (recurso propio,
  estados conocidos), nunca del original en vuelo.
- **Metodología (regla de oro del user)**: 3 hipótesis y 5 crashes
  evitables por no hacer la comparativa referencias-vs-código ANTES de
  teorizar. Y el repro standalone mínimo (bisect por primitiva con DRR
  tras cada paso) responde en minutos.


---

# R70-R74 additions (new-sli era, English)

## Compose / present path

| Symptom | Root cause | Lesson |
|---|---|---|
| Post-SR compose runs (52.8k dispatches) but NOTHING appears on screen | RDR2 copies the DLSS output texture before present — the composed texture is never presented (R73c) | A compose path must be verified with a pattern that WRITES WITHOUT READING (checkerboard); if invisible while composes flow, the composed texture is not the presented one. Per-game truth. |
| Badge visible in engine dumps but not on screen | Dump forensics prove the ENGINE chain only; screen forensics prove the last leg | Verify the LAST link explicitly (screen capture in borderless via CopyFromScreen). |
| "Applied boost but see nothing" (×2 rounds) | (a) compose CB grew 10→11 consts but the root signature still declared 10 — the new const read garbage with NO error; (b) panel slider calibration (pos+bias)/scale capped at 1.6 not 16 | Growing a cbuffer ⇒ update RootSignature Num32BitValues in the same edit. Calibrate slider math before blaming the shader. |
| NR toggle "does nothing" after redeploys | ctl nrStage change fires a host re-arm that RESETS tuning defaults, wiping nrOn (R55 reborn in R74b) | Order ctl changes: stage/config that re-arm FIRST → wait for `first frame computed` → set nrOn → verify via diag before measuring. |

## A/B methodology (the poisoned-measurement family)

| Symptom | Root cause | Lesson |
|---|---|---|
| A/B showed 26.6% diff "proving" boost — invalid | The PAUSE MENU overlays animated film grain; the diff measured the grain, not the effect | A/B only in live gameplay with a still camera; never in pause menus or photo mode (photo mode = 0 evaluates). |
| Day-scene A/B showed nothing; night A/B clearly visible | Delta visibility ∝ 1/luma | Validate in the luminance regime where the user sees the problem (R70d: "a day A/B does not validate night"). |
| Dark-scene grain (~present) decomposed as "our noise" | 90% was the game's own animated film grain + DLSS at crushed res; our raw-applied delta adds the rest, scaling linearly with boost (R74b) | Decompose with a native control arm before attributing; boost is an A/B knob, not quality. |

## Stability

| Symptom | Root cause | Lesson |
|---|---|---|
| Black frozen screen with live processes (ESC dead) | Engine poisoned mid-session; re-arm revived it but the pre-SR delivery kept serving a dead frame | Teardown must invalidate ALL delivery state of BOTH paths (R70f/R70i); the new-sli FSM makes teardown a single transition. |
| Device lost ~650 s into NR sessions | Vendor runtime leaks ~10 MB/s working set | Recycle by budget (configurable), debounce knob rebuilds (each rebuild re-creates the feature and leaks more). |
| SRV reading the texture the same dispatch writes as UAV | History ping-pong bound slot(2) to hist[0] (the UAV target) since R70h — undefined read, driver returned 0 | Never point an SRV at the same texture a UAV writes in the same dispatch; read [1], write [0], swap by copy. |
