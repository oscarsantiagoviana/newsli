# R95 — Extirpación PRE-SR + auditoría integral + preparación GitHub

User: "eliminamos completamente el modo PRE-SR, aprovecha para auditar el codigo
linea a linea y fichero a fichero y dejarlo perfectamente preparado para
publicarlo en github… codigo, comentarios, documentacion, wiki y worklog".

Tag de seguridad: pre-r95-presr-extirpation.

## Decisión arquitectónica (de la conversación)
POST-SR es el único modo: cumple LEY 1 al 100% (cada frame presentado lleva
SU delta, gate en Present sin límite). PRE-SR real (golpe 5) tiene lag-1
estructural cross-GPU → incapaz de cumplir LEY 1 → MUERE. Pre-SR legacy
(transporte a render res) muere con él: una sola línea de procesamiento.

## Fase A — Extirpación (manual, yo)
A1. offload_session.cpp/h: rama pre-SR real de EvaluateInner (seal→stage→
    compose→swap), PreSrEnsureObjects/PreSrStageGain/PreSrRecordCompose/
    PreSrSwapColorIfArmed, miembros (texNr/texGain/gainStage*/rsPreSr/
    psoPreSr/heapPreSr*/preSrSwap*/preSrDelivered/preSrStagedDone +
    viewer: viewerPid/viewerOfPid/viewerDeadLatch/viewerMap/ViewerUpdate),
    jitter escalares condicionales, s.inW/inH pre-SR, CtlLoadIni
    nrPreSr/nrColorBack, caso ctl 30 + mirror, PresentGate rama pre-SR.
A2. Borrar: src/host/nr_viewer.cpp, shaders/nr_compose_presr.hlsl.
    CMakeLists: target sli_viewer + embed shader fuera.
A3. nvngx_host.cpp: llamadas PreSrSwapColorIfArmed (ambos exports) + decl.
A4. abi.h: nrColorBack@108 fuera; Tuning re-packed (host+engine se
    despliegan juntos; sin retrocompat en POC). static_asserts al día.
A5. ctl_common.h: CTL_FIELD_NR_PRESR (30) = HOLE documentado (append-only).
A6. Engine: payload colorBack (loop.cpp/nr_vendor.h/nr_delta.hlsl) fuera;
    de-jitter pre-SR (jitterX/Y + compensación decode) fuera — POST-SR
    nunca jittera.
A7. Panel: filas Pre-SR (legacy+real) fuera, textos que mencionen pre-SR
    reescritos; gate-wait texto (R93h decía "inert in pre-SR" → fuera).
    tools/ctl.cpp: caso 30 y help.
A8. Build /W4 /WX 0/0 + ctest 3/3 (o los que queden) + deploy + verificación
    (feeder offline si el juego está cerrado; espejo ctl sin campo 30).

## Fase B — Auditoría paralela (subagentes, verificación mía)
B1. offload_session.cpp + nvngx_host.cpp + proxies (línea a línea).
B2. engine/* + shaders/* (línea a línea).
B3. panel/* + tools/* + shared/* + CMakeLists (línea a línea).
B4. Documentación: README, wiki (28), worklog (14) — honestidad, frescura
    (nada describiendo modos muertos como vivos), información privada
    (rutas personales, usuario, GPU del autor), estructura para GitHub.
Criterio común: comentarios que mienten = bug; dead code = bug; POC sin
legacy; Num32BitValues == consts; IDs holes documentados.

## Fase C — Cierre
C1. Aplicar fixes de auditoría verificados.
C2. README de proyecto público (qué es, cómo funciona, limitaciones
    honestas). LICENSE check (MIT declarado).
C3. .gitignore / sin binarios ni logs en el árbol; git status limpio.
C4. Commit final + tag pre-publicacion.

## Ejecución (2026-10-05 noche)

Fase A cerrada en `8571d8f` (ver detalles arriba + tag r95-post-only).

Fase B: 4 subagentes (deleg_c22a095c), transcripts task-0..3.log. Cada
hallazgo fue re-verificado por mí con lectura de código ANTES de tocar.

Fase C aplicada en 5 rondas, todas build /W4 /WX 0/0 + ctest 3/3:
- `4a116b9` ronda 1: **BLOCKER engine** — gain/flow tiles sized al grid
  WORK pero escritos por el dispatch RENDER del decode (OOB UAV con
  ws<1) → grid propio gtX/gtY en sizing/UAV/copies/consumers CPU; clamp
  final del gain [0.5,2] mataba el rango 2..8 documentado del gainBound →
  clamp viaja con dGainBound; textos panel Offload/NR describían la
  arquitectura SR-offload extirpada → reescritos a la verdad; split
  default 500→0 (paridad host); comentarios mentirosos (SealColorPost
  legacy, nvngx_host compose-en-lista, dxgi helper-thread x2, engine
  zero-delta→unity, guide-ring doc); muertos fuera (Reason::Placement,
  trío NrDetail/NrColour/NrAutoSkin, CoprocLdrLoadExW, ngx_proxy.cpp);
  ledger ctl (holes 30+3 documentados, nr.residual nombrado); higiene
  (build-dbg 421 ficheros + logtest + bats personales untracked, capturas
  raíz borradas, .gitignore).
- `3baa9aa` ronda 2: **nrDeadStreak latch permanente** (10 fallos = NR
  muerto de por vida, invisible al watchdog del host) → re-probe 5 s con
  log; muertos (stabDepthBound, depthRealFrames, colorBaseX/Y,
  ComposeGpu::inW/inH, trío saved panel); docs que mienten (feeder WARP
  "no NVIDIA hardware", abi.h offset, log.h CrashHandlerInstall, log
  primer frame); rutas personales genericizadas.
- `7171852` ronda 3: **raza gameQueueForFlush** (ComPtr escrito por hilo
  juego / reseteado por drain / leído por compose sin lock) → las 3 touch
  sites bajo pendingCs con copia local; edge >3 s del drain logueado.
- `69c9290` ronda 4: textos de usuario del panel sin jerga interna
  (golpe 4/fork/R-codes/RDR2/resolución personal fuera).
- `82e0783`+`1b8d890` docs: README público reescrito (arquitectura
  post-R95, quick start F6/F7/F8, limitaciones honestas, sli_ctl en
  build/), glossary filas stale, rutas personales en wiki/docs.

Scrub final: 0 rutas personales fuera de analysis//worklog/ (que se
deciden privados en el empaquetado); F6/F7/F8 verificados vivos en el
host (RegisterHotKey, offload_session.cpp:1104-1118).

Pendiente opcional pre-push: empaquetado (excluir analysis//worklog/ si
se quiere público mínimo), LICENSE ya MIT, decidir nombre del repo.
