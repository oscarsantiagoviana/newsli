# 41 — R91b: modo normal adelgazado — sacar del camino crítico todo lo prescindible

Fecha: 2026-10-04. Go user: "sacar del modo procesamiento normal todo lo
posible... quitar ese código del medio en ejecución normal, no solo para
optimizar sino para simplificar el camino crítico".

## Hallazgo base (medido en código)

`LogRate` (log.h:38) formatea el buffer SIEMPRE y solo después
`LogRateLine` consulta la tabla de throttle: cada llamada por frame paga
_snprintf_s + copia aunque se descarte. Con 3 sitios que laten cada 1-2 s
en los hilos del juego (evaluate #, produce N, submit ignored) el coste
es real y escala con fps.

## Cambios

1. **log.h/log.cpp — gate ANTES de formatear**: nuevo `RateDue(ms,key)`
   (consulta+actualiza la tabla bajo el CS existente); `LogRate` pasa a
   `if (!RateDue(ms, fmt)) return;` y SOLO entonces formatea y emite.
   Semantics idénticas (a lo sumo una emisión por ventana).
2. **Throttles rutinarios 5 s → 60 s** (siguen siendo forenses, dejan
   de latir): `evaluate #` (nvngx_host), `produce N queue-signaled` y
   `submit ignored` (offload_session), `gainnorm med` (nr_vendor).
   `vendor frame DELTA` cada 300 → cada 1800 (mismo espíritu).
   Los eventos RAROS (degraded/watchdog/poison/reconfig/cold start)
   siguen SIN throttle — ojo clínico intacto.
3. **Rejilla de exposición muerta**: RDR2 siempre manda pre-exposure
   (`exposureHeld` desde frame 1) — la copia GPU del bufTiles→readback
   y la mediana CPU (nth_element ~5k) corren para nada. Gate: si
   `exposureHeld`, ni copia ni mediana (el miembro ya existe; el copy
   se condiciona en ProcessFrame).
4. **GainNorm muestreado**: mediana por canal sobre 1 de cada 8 tiles
   (~1.360 valores vs 10.880) — la red anti-runaway de R82e no necesita
   estadística completa para clavar ~1.0; nth_element 8× más barato y
   el readback sigue igual (el store del shader no cambia).
5. **Flow copy**: ya gated por knob (R90 #6) en el lado CPU; el copy
   GPU va en el mismo segment — se deja (170 KB, no está en el hilo
   del juego).

## Regla de despliegue

Host+engine se tocan → kill juego, deploy AMBOS desde build/deploy/,
relanzar (Launcher + intro solo en título). Panel sin cambios.

## Validación

- Build 0 err/0 warn + ctest 3/3.
- Boot limpio, 0 degrades, DELTAs en rango, y el log de la sesión
  siguiente debe mostrar los latidos a ~1/min en vez de ~57/min.
