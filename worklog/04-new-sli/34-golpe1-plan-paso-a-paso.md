# Golpe 1 — plan detallado paso a paso (2026-10-03)

Mandato user (verbatim decisiones):
- **De golpe**: #1 PQV, #2 produce-wait por evento, #3 un FlushSeg, #5 anillo
  por sealedFrame, #6 micro-limpiezas, #7 compose ring, #8 hook ECL afilado,
  #11 output ring.
- **#4 (depth) RECHAZADO como "redundante"** — hipótesis user: "estoy casi
  seguro de que lo que pasa es que no se la estamos dando bien o en el
  formato que espera o con las dimensiones correctas". Va en **golpe 2**
  contrastando con los proyectos de referencia (REGLA ORO REFERENCIAS). NO
  tocar depth en este golpe.
- **Golpe 3**: #9 nrPreSr (A/B de imagen con el user delante).
- **Golpe 4**: #10 matched-residual (offline con feeder primero).

Numeración = explicación al user (worklog 33 / wiki 28). Ningún cambio aquí
usa frameskip ni presenta sin NR (LEY 1); nada con riesgo ghosting/flicker.

## Paso 0 — lectura completa (REGLA ORO LECTURA, obligatoria antes de tocar)

Leer ENTEROS y enumerar en cada uno los puntos calientes:

| Fichero | Qué enumerar |
|---|---|
| src/engine/loop.cpp (657) | usos del contador privado `frame`; TODAS las indexaciones %3 (guideM/guideD/bufIn); los 3 FlushSeg y qué barreras hace cada segmento; qué espera ProcessFrame del estado de la lista |
| src/engine/nr_vendor.h (1249) | Init PQV (723-732); ProcessFrame (barreras propias vs del seg); UpdateGainNorm/FlowPeakPx alocaciones (1092, 1127-1129); copyGuide depth |
| src/host/offload_session.cpp (2862) | MakeSharedBuf del out y CÓMO se pasa el handle al engine (SpawnEngine/Handshake); ComposeStageDraw completo (alloc/cmd/fence/flush); OnListSubmitted/seal; CtlLoadIni/BuildCtlMirror/cases ctl |
| src/host/nvngx_host.cpp (777) | HookedECL entero: qué corre dentro del SRWLock, GetDesc/GetTickCount64/LogRate |
| src/shared/abi.h | Tuning offsets + static_asserts; Handshake hic/hoc/hpf/hdf; contrato out actual |
| src/shared/ctl_common.h + panel_internal.h + panel_ui.cpp + panel_host.cpp | kSpecs, KeyIdx, batch, patrón de una fila con re-arm (workScale) |
| src/shared/log.cpp | LogRate bajo CS (para #8/#9 del hook) |
| src/tools/ctl.cpp + host_smoke.cpp + replay_feeder.cpp | recompilar con ABI nueva; feeder usa el contrato out |

## Paso 1 — ABI y contrato (se congela primero; engine+host recompilan juntos)

1a. **Tuning.reserved8@8 → nrPerfQuality (float)**. Renombrar SIN mover
    offset (lección R89b: los offsetof static_asserts pinnean; 104 B
    intactos). static_assert offsetof==8. Default 2.0 (balanced, el valor
    que siempre corrió). Clamp host 0.0-4.0.
1b. **Output ring 3 slots**: preferencia de diseño — los 3 slots viven en el
    MISMO heap compartido (placed buffers, offsets fijos documentados en
    abi.h), SIN handles nuevos en el Handshake ( layout+static_asserts
    intactos). `hoc` pasa de "handle del buffer out" a "handle del heap out"
    con offsets constantes ABI. La lectura del paso 0 confirma si el vehículo
    actual es buffer-suelto o heap y se adapta 1:1. Contrato escrito EN
    abi.h: "slot = frame % 3; engine escribe el slot del sealedFrame; host
    compone leyendo el slot del frame N; margen 2 frames".
1c. **ctl id 40 = CTL_NR_PERFQUALITY**, CTL_FIELD_COUNT 40→41. IDs
    append-only; huecos muertos siguen muertos.

## Paso 2 — knob PQV (create-time, REGLA ORO PANEL)

2a. nr_vendor Init: `set_flt(P, fslot, "PerfQualityValue", tuning->nrPerfQuality)`
    + uint shadow (doble escritura, lección R82f). ScalingRatio sigue
    pinned 1.0. Comentario: sustituye el literal 2.0; documentar
    0=MaxPerf..4=MaxQuality y el accidente R82g (respuesta al 43%).
2b. Host: CtlLoadIni "nrPerfQuality" (clamp 0-4, default 2.0); case id 40 →
    aplicar con RE-ARM en cambio real (patrón workScale/nrParamSeq,
    create-time ~1 s); BuildCtlMirror f32; sli.ini AMBOS lados
    (+nrPerfQuality=2.000).
2c. Panel: slider acotado 0-4 step 1 (0=MaxPerf, 2=Balanced, 4=MaxQuality),
    tag "re-arms the model (~1 s)", texto honesto: "modo de esfuerzo del
    modelo NR. 0 = máxima velocidad — limpieza más suave, nunca desplazada;
    2 = balanced (default); 4 = máxima calidad". static_assert KeyIdx
    ampliado. Apply live + Save (recargado al armar el offload).

## Paso 3 — engine: produce-wait por evento (#2)

3a. Event auto-reset creado una vez al iniciar el thread.
3b. Sustituir poll Sleep(1) (loop.cpp:384-392): target = frame+1;
    `SetEventOnCompletion(target, ev)` → `WaitForSingleObject(ev, restante)`
    con presupuesto total 60 s (restante decae entre reintentos si hace
    falta re-armar el completion). Semántica conservada EXACTA: timeout NO
    avanza frame; contador de timeouts + log igual; poison (UINT64_MAX)
    check después del wait.
3c. `sealedFrame = GetCompletedValue()` tras despertar — contrato c12aee9
    INTACTO (SetEventOnCompletion sobre valor ya completado señaliza al
    instante: mismo comportamiento que el poll).
3d. Si SetEventOnCompletion devuelve error → log + camino de timeout actual.

## Paso 4 — engine: indexación por sealedFrame (#5)

4a. Con la enumeración del paso 0: sustituir TODAS las indexaciones de
    anillo que usen el contador privado por `sealedFrame % 3`. El host
    sella guide*[s.frame%3] y señaliza produce=N con s.frame=N → slot
    correcto = sealedFrame%3 SIEMPRE (lockstep o no).
4b. El contador `frame`: revisar usos restantes (logs/stats); si muere →
    eliminar (no-legacy). engineHostFrame/done NO se tocan.

## Paso 5 — engine: micro (#6)

5a. FlowPeakPx solo bajo `if (thr > 0.0f)` — byte-idéntico con knob on,
    cero trabajo con knob off (loop.cpp:441-444).
5b. Prealloc miembros: 3 vectors de UpdateGainNorm + 1 de FlowPeakPx,
    resize al (re)arm (patrón tileLums, nr_vendor.h:140).

## Paso 6 — engine: un FlushSeg (#3)

6a. Fusionar seg1+seg2+seg3 en UNA lista por frame: barreras+copia in →
    ProcessFrame (guías+encode+evaluate+decode+copybacks) → copia out →
    Close → ECL → Signal → UN wait de fence.
6b. Verificar con la lectura qué barreras hace ProcessFrame él mismo y
    cuáles dependían del estado dejado por el seg1/seg3 (transiciones
    COPY_DEST→SRV etc.) — replicarlas en el orden correcto dentro de la
    lista única.
6c. Consecuencia asumida y documentada: se pierde la atribución fina de los
    códigos de error 6/7 (loop.cpp:615-634); el catch-all cubre. El timer
    "vendor frame N" pasa a abarcar el frame completo (mejor para medir).
6d. ZeroDeltaToOut y catchs: caminos de excepción不变 — siguen con lista
    propia si la necesitan.

## Paso 7 — host: compose ring (#7)

7a. `c.alloc/c.cmd` únicos → anillo [3] allocators+lists + valor de fence
    por slot. InitCompose ampliado.
7b. Present NR: slot = n++%3; reciclaje no bloqueante — si
    GetCompletedValue(slot fence) < valor enviado de ese slot, esperar
    (solo si vamos ≥3 slots por detrás: nunca en régimen); grabar+submit+
    Signal; ELIMINAR WaitForSingleObject(fenceEvt,2000) del camino del
    Present. Hang → watchdog 10 s del gate (ya existe). Orden del flip =
    FIFO de la cola del juego (hallazgo #10 doc).
7c. DoDrain: esperar el anillo completo (fence al último valor) antes de
    destruir recursos.

## Paso 8 — host: hook ECL afilado (#8)

8a. Con la lectura: enumerar qué corre dentro del SRWLock exclusivo.
8b. Early-exit SIN lock cuando no hay listas pendientes que matchear
    (check atómico) — el caso común de submits ajenos al evaluate.
8c. LogRate del contador ECL FUERA del lock (hoy hace I/O de fichero con
    el lock tomado cada 5 s).
8d. GetDesc/GetTickCount64 fuera del lock o solo para listas candidatas.

## Paso 9 — output ring (#11) — el cambio gordo, al final

9a. Host (arm): out = heap compartido con 3 slots placed (offsets fijos
    ABI del 1b); seal/producción sin cambios.
9b. Engine: copyback del decode escribe out[sealedFrame%3] (el target del
    copy del paso 6 apunta al slot).
9c. Host compose: stage copy lee out[N%3] (N = frame que se compone);
    texDelta propia sigue única.
9d. Hazard analysis (escrito en el worklog de resultados): engine puede
    reescribir slot (N+3)%3 mientras host stagea N → margen 2 frames; el
    host stagea dentro del Present N (inmediato) → sin fence nuevo SI el
    análisis confirma el margen; si no, shared fence "consumed" host→engine
    (decisión documentada, no silenciosa).
9e. done/engineHostFrame/gate: SIN cambios (el gate sigue esperando
    host==N exacto — LEY 1 intacta).

## Paso 10 — build + pruebas offline

10a. Build 0 warnings (/W4 /WX); ctest 3/3.
10b. Recompilar TODOS los consumidores de ABI (engine, host, panel, ctl,
    smoke, feeder). Verificar md5 de los 3 binarios de deploy.

## Paso 11 — deploy + verificación in-game (REGLA ORO ALINEACIÓN)

11a. Kill RDR2 si corre (autorizado) → cp 3 binarios → md5 en pares →
    sli.ini ambos lados idéntico → relaunch (powershell Start-Process
    Launcher.exe) → intro en el título → confirmar arming+READY.
11b. Estabilidad: 0 degrades ≥10 min; sin stalls; ECL/produce/deltas
    fluyendo.
11c. **Medición A/B contra baseline 2026-10-03** (overlay 5 fps / 1% 4;
    composes 4.44-5.2/s; vendor DELTA 113-130 ms; produce ~5.8/s): misma
    escena, 120 s: overlay fps+1% low, composes/s, DELTA ms medio,
    GPU1/GPU2 util+W.
11d. **A/B del knob PQV in-game**: panel 2↔0↔4 (re-arm ~1 s cada cambio),
    medir DELTA ms en cada modo — valida o refuta las -45-70 ms estimadas
    (discrepancia worklog-33 vs auditores la resuelve esta medición).
11e. Capturas + vision ×2.00: sin ghosting/flicker/artefactos (criterio de
    abort).

## Paso 12 — cierre

12a. Commit del golpe (revert puntual por paso si algo se rompe a mitad).
12b. Worklog 35 con resultados medidos + wiki 28 actualizada con números
    reales (estimaciones → medidas).

## Criterios de abort/revert

- Cualquier degrade/watchdog en régimen → parar e investigar ese paso.
- Ghosting/flicker en capturas → revert del cambio sospechoso (LEY 2).
- PQV 0 no baja DELTA → el knob se queda (honesto), la previsión de fps se
  recalibra con lo medido — sin inventos para cuadrar el número.

## Métrica de éxito del golpe (a medir, no a prometer)

- DELTA medio ≤90 ms con PQV=2 (solo por #2/#3/#5/#6/#11).
- Con PQV=0: DELTA adicional si la evidencia R82g se confirma.
- composes/s ≥7 y overlay fps sube respecto a 5/1%-4.
- 0 degrades, 0 stalls, capturas limpias.

## Registro de golpes siguientes (NO ejecutar ahora)

- **Golpe 2 (#4 depth)**: contrastar con referencias (OptiScaler,
  Streamline, forks del reference-inventory, ficha neural-coprocessor:
  "depth byte-idéntico con su param set — solo color cruza"). Hipótesis
  user: la guía NO sobra — se entrega mal (formato/dims/contenido).
  Decidir formato correcto (¿R32F? ¿dims render vs work? ¿no-bind?).
- **Golpe 3 (#9)**: nrPreSr=1, A/B de imagen con el user delante.
- **Golpe 4 (#10)**: matched-residual, offline con replay_feeder primero.

---

## EJECUCIÓN (2026-10-03 20:xx) — commit d69831c

- Paso 0: 13 ficheros leídos completos. Decisiones confirmadas: hoc ya era
  handle de HEAP (ring sin tocar Handshake); reserved8@8 libre para PQV.
- Pasos 1-9 aplicados íntegros (ABI Tuning.nrPerfQuality@8 + kOutRingSlots
  contract; ctl 40 + mirror 41; engine event-wait/un-FlushSeg/sealedFrame%
  3/PQV-init+rebuild/prealloc/FlowPeakPx-guard; host OutRing/compose ring
  3 slots sin flush/PQV ini+ctl+mirror/ECL afilado; panel fila Perf quality;
  ctl.cpp nombre; feeder ring).
- Incidentes de edición: 1 borrado accidental de UICorrection (restaurado
  inmediatamente); 1 duplicado NR_LS en panel_ui (corregido). Ambos
  verificados en el diff final.
- Build: primer intento NO-OP silencioso (cmd //c se comía el comando);
  bat en scratch + cmd /c → errores reales (kOutRingSlots sin namespace,
  firmas de guías) → corregidos → 0 err/warn /W4 /WX, ctest 3/3.
- Deploy: kill 4696/20648/16276 → cp → md5 nvngx 300fd8cd… / engine
  87ab2a0a… / panel e228befb… → ini idéntico → relaunch 20:12:52.
- Boot verificado (engine pid 14628, arm 20:16:43, READY 20:17): **0
  degrades, 0 watchdogs, 0 excepciones**; produce ~10.2/s (antes 5.8);
  composes 600/19 s arranque + 5.5/s crucero; vendor DELTA 140-146 ms
  (timer ahora abarca TODO el frame: in-copy + out-copy + UpdateGainNorm —
  no comparable 1:1 con los 113-130 de seg-2-only).
- PENDIENTE de verificar con user en partida: overlay fps/1% low + A/B PQV
  2↔0 desde el panel (el knob viaja por nrParamSeq, rebuild ~1 s).

---

## A/B PQV IN-SITU (20:28, pre-probe) — RESULTADO NEGATIVO HONESTO

- User movió slider 2→0 (Apply 20:28:31). Cadena VERIFICADA completa:
  panel 40=0 → host ctl batch → engine rebuild 300 ms después.
- Cronómetro frame completo: pre 139.8 ms (n=22) vs post 153.1 (n=5, misma
  ventana corta) — SIN CAMBIO; composes ~5.3/s igual.
- Imagen: sin cambio visible (el accidente R82g habría sido evidente).
- Conclusión provisional: o el runtime ignora PerfQualityValue para este
  modelo/stream, o el valor no llegó al bloque. Los logs actuales NO
  distinguen (la línea de rebuild no imprime pqv; el self-check no sondea
  esa clave).
- ACCIÓN: probe commit (log pqv en cada create + fila PerfQualityValue en
  el self-check). Engine md5 e65b8a3b… redeploy 20:4x; user re-entra y
  repite el A/B → la línea "feature rebuilt … pqv X.X" + "block
  [PerfQualityValue] …" decidirá.
