# 21 — R82l: repaso general (defectos, legacy, desalineamientos)

Ronda 2026-10-01 → commit `624ece6`. Origen: el fantasma sobrevivió a todos
los A/B (detail 0/1, FPS igualados con ws=0.5) y el user ordenó repaso
general: TODO el código propio leído completo + los proyectos de referencia
con sus docs y mensajes de commit, buscando defectos, legacy,
desalineamientos o mejoras.

## Metodología

- Lectura COMPLETA (regla de oro) de: host (offload_session.cpp ~2810 L,
  nvngx_host.cpp, dxgi_proxy.cpp, offload_session.h), engine (loop.cpp,
  nr_vendor.h 1183 L, main.cpp, engine_ctx.h), los 3 shaders, shared
  (abi.h, ctl_common.h, log.cpp/h), panel (5 TU), tools (ctl.cpp,
  replay_feeder.cpp, test_log.cpp), CMakeLists y los 18 commits.
- Dos subagentes en paralelo (auditoría panel/tools/shared; deep-review de
  UNCANNY + commits/docs de los forks). **Cada hallazgo de subagente se
  re-verificó en el código antes de editar** — uno venía INVERTIDO.
- UNCANNY cerrado: repo = docs/releases (closed source); ZIP v0.20
  auditado. Es **cross-process como nosotros** (NeuralHost64.exe + bridge
  add-on de ReShade), con **deadline de Present de 100 ms y DESCARTE del
  resultado tardío** ("retains source and discards that late result — no
  stale-frame reuse") y Ghosting Guard que rechaza historia por
  flow-confidence. Es el único reference con nuestra topología y eligió
  espera acotada; aun así documenta "not a low-latency or FPS-parity
  guarantee".

## Veredicto fantasma (consolidado)

Captura con DOBLE imagen = dos copias de la geometría **en el frame** (la
persistencia del ojo no sale en capturas). Con detail=0 nuestro compose es
identidad bit-exacta (gain multiplicativo; lerp→enc ⇒ ratio=1 ⇒ gain=1) ⇒
el doble es **el TAA del propio juego a 6-9 fps** (el gate estrangula al
ritmo del engine; NR-off corre 20-30). La vista NR está limpia porque el
payload tint dibuja OPAQUE (reemplaza y esconde el smear del TAA del juego);
quality multiplica y lo deja ver. Cámara→delante / personaje→detrás: firma
clásica de TAA con historia warpeada a pasos angulares grandes. Test
decisivo (user): NR-off + cap 6-7 fps + paneo.

## Hallazgos y fixes (todos en `624ece6`)

**Críticos**
- Panel `HostView::mirror[32]/mirrorSource[32]`: R82k.7 arregló CtlMsg pero
  la copia del panel quedó corta — los readouts "Host:" de los knobs 33-36
  leían memoria pisada (los A/B de detail se calibraron con valores falsos).
  → dimensionado por CTL_FIELD_COUNT + copia por COUNT.
- GPU combo desalineada: `EnumGpus` corría antes de conocer el LUID del
  juego → el índice N del panel ≠ índice N del host (elegir la GPU del juego
  armaba otra). → edge-detect del LUID en PollHost + re-enum.
- `sli_ctl get/set <f>` sin acotar (AV posible con f≥37) + nombres 27/30-36
  ausentes + `seq` sin Interlocked. → clamp + FieldName completo + barrier.
- `SealColorPost` sin guard de feature: sellaba para CUALQUIER evaluate NGX
  (stale de 1 frame si RDR2 evaluara una 2ª feature). → SS-only.
- `mvRing` ELIMINADO (no-legacy): 8 texturas render-res (~75 MB VRAM del
  juego) + copia por frame + DIAG Close/Reopen en el camino caliente del
  seal; su propio comentario decía "forensics and future FG work".
- Defaults panel≠host en INT/LS/LT/SS: instalación limpia = 0.0 VIVO
  (knobOr trata 0 como valor) = NR inoperante silencioso. → lo=def=-1
  ("vendor default"), clamp por abajo en ApplyCtlField.

**Altos**
- Feeder: guías en `(n+2)%3` = 2 frames detrás del live (el engine lee
  `(n+1)%3`, contador 1-based). → `(n+1)%3` (same-frame parity). Y depth
  proxy 0.0 (=far) con depthInverted=1 → 1.0 (near=1, como prometían sus
  comentarios). Los probes offline medían otra física que el juego.
- `capture` no se cargaba del ini (panel lo guarda, host lo ignoraba).
- Maquinaria override MUERTA eliminada de raíz: campo `overrideable`,
  `ovr/appliedOvr/savedOvr`, checkbox Override, rama "Game:", `GameValue` y
  el bloque de texto que prometía game-overrides (REGLA ORO PANEL).
- LUID HighPart con signo en engine main.cpp ( HighPart negativo jamás
  igualaba). → casts explícitos.
- Clamp de ini en el panel (combo/slider fuera de rango).
- LogRate sin CS (carrera host/engine) → bajo g_log.cs. Crash handler:
  TryEnter + append directo (sin deadlock si el CS está tomado).
- Panel: HRESULT de ResizeBuffers logueado + skip del frame con g_rtv null;
  ping contaminado por Apply → pingMs=0; SameLine con ventana <200 px;
  LogEvent a GetTickCount64 (wrap 49.7 días); mutex single-instance sin log.
- CMake: `CSO` muerta ×3, cabecera garbled, tests: `t_log` (rotación) ya no
  muere; `t_crash` separado con wrapper que verifica el MINIDUMP (ctest no
  puede expresar "segfault esperado" honestamente; WILL_FAIL enmascaraba
  asserts reales). `enable_testing()` antes del primer add_test.
- Comentarios de era aditiva corregidos (loop.cpp, offload_session.h,
  engine_ctx.h, abi.h, CMake): el transporte es GAIN multiplicativo desde
  R81; "base + 0" era falso. Y la doc "guías one-behind (fork parity)" de
  abi.h/engine_ctx.h describía un lag que el código NUNCA tuvo — el engine
  lee guías del MISMO frame (paridad fork real). Doc corregida, código
  intacto (el comentario era el bug).
- nrBoost doc "8-10 = visible A/B" → "2-4 (clamp 4.0)"; nrTint doc binaria.

## Estado

Build 0 warnings /W4 /WX; ctest 3/3 PASS; deploy 5/5 md5 OK (gamedir +
build/deploy); commit `624ece6`; old panel killed, new panel live;
the game waiting at the menu for manual input. ABI/ctl sin
cambios de layout (416 B). Worklog extendido: `worklog/04-new-sli/23-r82l-repaso-general.md`.

## Lecciones (skill actualizado)

1. Subagente con hallazgo invertido por un comentario mentiroso: el código
   es el árbitro; verificar la algebra de slots contra el contador real.
2. ctest WILL_FAIL = trampa (invierte cualquier fallo). Para "el crash ES
   el test": wrapper que verifica el efecto (minidump), no el exit code.
3. Cuando cambia el contrato del transporte, grep de "delta/additive/base+0"
   obligatorio — los comentarios de la era vieja costaron el misread R82k.3.
4. Repaso general = clase de bug, no lista: texto que promete features
   muertas, arrays peers desactualizados, guards por-estado en vez de por-
   feature, "for future work" en camino caliente, contratos ini partido.
