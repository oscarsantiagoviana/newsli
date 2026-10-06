# 44 — Golpe 5: pre-SR REAL — write-back antes del DLSS del juego

Fecha: 2026-10-04 (noche). Directiva user: "si estamos diciendo PRE-SR lo
normal es que hagamos todo PRE-SR". Padre: worklog 43 (veredicto R92g —
el híbrido input-pre/output-post es la causa física del shimmer).
Tag rollback: `pre-golpe5-presr-real` (cde0d14).

## El fallo de diseño de hoy

Modo actual "pre-SR": sellamos el raster crudo (jitterado, sin resolver)
→ modelo en GPU2 → devolvemos un GAIN → se compone en el Present sobre
el frame FINAL (ya SR + tonemap). El modelo responde a un plano que
respira; su respuesta aterriza sobre uno quieto. El −j del sello aligna
dominios pero no añade la integración temporal que falta — solo el TAA
del juego puede hacerla, y lo excluimos del camino.

## Referencias (consultadas antes de diseñar — REGLA ORO)

- Fork NeuRotic DlssNr_Dx12.cpp:5500-5670 — pre-SR v10: scratch UAV →
  re-jitter Mode 5 (bilinear +j con MvScaleX/Y = jitterX/Y) → CopyResource
  al Color ORIGINAL del juego → la evaluate nativa consume. NO espera
  nada: su NR corre inline en GPU1 (1-3 ms). Sostiene un reset one-shot
  en el primer handoff (preDlaaSavedReset).
- Fork OptiScaler-DLSSNR-PreSR-Multipass (DlssNr_DeferredSr.inl:500-530):
  en vez de sobrescribir la textura del juego, **p->Set(Color, g.residualInput)
  — sustituye el RECURSO en el bloque de parámetros** que la evaluate
  nativa lee. Sin tocar estados de llegada del recurso del juego.
- Nuestro R85w (skill r85w-wedge-verdict): wait CPU dentro de evaluate =
  insatisfiable — produce(N) se señaliza en el SUBMIT del juego, que no
  llega mientras evaluate no retorna. Un gate en evaluate con wait CPU
  = deadlock estructural.
- Nuestro R85d: write-back a recursos del juego funciona (dstOutput) si
  se respeta el estado de llegada y la familia DXGI.

## El diseño elegido (tras las referencias)

**Cambios de recurso por parámetro (patrón Multipass) + espera GPU-side
(patrn R90/R85g) + re-jitter en el engine (patrón NeuRotic).**

Flujo evaluate del juego, modo pre-SR real:
1. OfferFrameToSession normal (seal pre-evaluate, como hoy).
2. Host crea UNA textura propia `texNr` (mismo desc que srcColor del
   frame, mismo formato familia) y el engine escribe ahí el color NR.
3. El evaluate hook, ANTES de reenviar al nativo:
   a. `queue->Wait(doneFence, value(N))` en la cola del juego — la GPU
      espera, el CPU nunca se bloquea (evaluates en la misma cola se
      ordenan detrás; las colas async del juego no dependen de done).
   b. p->Set(NVSDK_NGX_Parameter_Color, texNr) + subrect base 0.
   c. reenvía al nativo → el DLSS del juego consume el color NR.
4. El engine (nuevo payload): color = raster(-j)×gain muestreado +j
   (re-jitter inverso del sello). Jitter 0 = identidad bit-exacta.
5. PresentGate en modo pre-SR real: NO compone (nada que componer —
   el NR ya viajó por dentro del SR). Fail-open permanente: done fence
   poisoned / engine muerto → latch degrade → Color vuelve a ser el
   del juego (nativo puro) — pantalla nunca rota.

Deadlock check (la clase R85w): el wait es GPU-side sobre la MISMA cola
que correrá el SR; produce(N) ya fue señalizado en el submit del frame
N (OnListSubmitted corre en el submit del juego, ANTES de que la GPU
llegue a ejecutar el wait). El engine procesa N → done(N) → el wait
satisface. Si el engine muere, la fence compartida muere con él → el
wait GPU quedaría colgado → watchdog 10 s → degrade permanente a nativo
(igual que hoy; la fence done es cross-adapter compartida, un TDR en
GPU2 la envenena y PollHealth la detecta — igual que hoy).

## Trade-off de cadencia (honesto)

El frame del juego encola: render → [wait done(N)] → SR → present. El
NR (~42-51 ms a ws 1) se serializa con el SR en la MISMA cola → fps
~15-20 como el pre-SR actual, pero el SR integra el NR con su TAA:
estabilidad POST-SR + detalle del raster completo.

## Cambios por fichero

1. `abi.h`: Tuning.nrPreSr doc update (semántica nueva). El out ring
   sigue siendo R16G16B16A16_FLOAT — el engine escribe COLOR NR ahí
   (el host ya lo lee en PresentGate; en este modo lo copia a texNr).
   Sin ABI breaks: mismo sizeof, mismos offsets.
2. `offload_session.cpp`:
   - `texNr` (lazy, en Session), mismo desc que srcColor, estado
     inicial COMMON; transiciones por frame COPY_DEST→SRV.
   - EvaluateInner camino pre-SR real: SIN compose en Present; el
     swap Color→texNr + queue->Wait(done) ocurre en el hook ANTES del
     forward (nueva función `PreSrSwapColor(params, queue, N)`).
   - PresentGateInner: `if (preSrReal) return ForwardPresent(...)`.
   - El cold-start (primera delta) del gate se conserva: hasta que
     done(1) no existe, Color del juego intacto (nativo).
3. `nr_vendor.h` + `nr_decode/nr_delta.hlsl`: payload pre-SR real =
   base(-j)×gain con re-jitter +j al escribir el out ring. La vista
   NR (tint) sigue mostrando el crudo del modelo (instrumento).
4. `panel`: "Pre-SR input" texto nuevo ("real: el NR entra en el SR
   del juego — el TAA del juego estabiliza; cadencia ~pre-SR").
5. Ini: `nrPreSr` 1 = ESTO (el híbrido muere — rollback via tag).

## Validación (protocolo completo)

1. Build + ctest + feeder (mecanismo: jitter 0 → bit-idéntico; jitter
   Bayer → out ring lleva color re-jitterado ≠ gain).
2. Live: pre-SR real en barandas — veredicto LEY 2 = el FRAME FINAL
   estable (la vista NR puede ondear: es el crudo). A/B vs POST-SR
   ws 0.5 (estabilidad + detalle + cadencia).
3. Forense del log: "pre-sr real swap" línea por frame N con done(N)
   esperado — ancla para medir la serialización real.
