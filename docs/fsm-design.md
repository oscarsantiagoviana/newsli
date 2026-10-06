# Host session FSM — design doc (F3 prerequisite)

The single biggest structural risk carried over from the POC is hot re-arm:
FOUR independent paths (gpuIndex change, workScale change, DRS geometry
change, late guides) each did partial teardown + re-arm with ad-hoc flags.
R70d proved that resetting resources possibly in flight = device lost.
This document defines the ONE state machine that replaces all four.

## States

```
                 ┌────────────────────────────────────────────┐
                 │                                            │
   ┌────────┐    ▼   ┌─────────┐   ok    ┌──────┐            │
   │  IDLE  ├───────►│ ARMING  ├────────►│ LIVE ├────────┐    │
   └───▲────┘        └────┬────┘         └──┬───┘        │    │
       │                  │ fail            │ reconfig   │    │
       │                  ▼                 ▼ queued     │    │
       │             (teardown →           ┌──────────┐  │    │
       │              COOLDOWN)            │ DRAINING │◄─┘    │
       │                                   └────┬─────┘       │
       │            ┌───────────┐               │ drained     │
       └────────────┤ COOLDOWN  │◄──────────────┤             │
                    └─────┬─────┘  teardown     │
                          │ timer (2 s)         │
                          └──────────► IDLE ◄───┘ (full teardown path)
```

- **IDLE** — no engine process, no shared resources owned. Entry point after
  load and after full teardown. The proxy forwards everything to native NGX.
- **ARMING** — one-shot, under `session.cs`:
  1. Resolve target GPU (gpuIndex config or auto: first NVIDIA ≠ game adapter).
  2. Create shared heaps/buffers (inColor/out/guides×3), shared fences
     (produce/done), the named frame mapping.
  3. Write Handshake (handles duplicated AFTER CreateProcess-suspended).
  4. Spawn engine (hidden console, SUSPENDED), duplicate NT handles into it,
     write handshake, resume.
  5. Wait READY (poll hs->ready == READY_MAGIC, timeout 30 s — 165 MB of
     weights load; on timeout: kill by PID, teardown, → COOLDOWN).
  6. First evaluate passes → LIVE.
  All resources are created HERE and nowhere else.
- **LIVE** — normal per-frame flow (seal → produce → compose). A reconfig
  request (gpuIndex/workScale/DRS/late-guides) does NOT act immediately:
  it enqueues `{ReconfigReason}` and pokes the session thread. Multiple
  requests coalesce (latest wins per field).
- **DRAINING** — entered from LIVE on any reconfig or teardown request:
  1. Stop recording new seals (evaluate returns forward-to-native
     immediately — the game never loses frames).
  2. Wait for the engine's in-flight frame: done fence reaches the last
     produced value OR 2 s timeout.
  3. Poison produce fence (UINT64_MAX) → engine exits cleanly.
  4. Wait engine process handle (5 s) or kill by PID.
  5. Release OUR shared resources (heaps/buffers/fences/mapping) — safe now:
     nothing references them.
- **COOLDOWN** — 2 s timer, then IDLE. Prevents crash-loops from
  re-arming instantly into the same failure (the POC's 1051-spawn night).
  Teardown-on-game-exit goes IDLE directly (no re-arm will follow).

## Invariants

1. **Resources exist only in LIVE/ARMING.** No texture reset, heap release,
   or fence signal outside DRAINING's defined order. (R70d, R70f, R70i.)
2. **The game thread NEVER waits on the FSM.** Evaluate-path checks state
   with a single atomic load; anything ≠ LIVE = forward to native NGX and
   return false (the game's frame is never blocked — R69j).
3. **One writer per transition.** All transitions happen on the session
   thread under `session.cs`; ctl/hotkeys/DRS only enqueue.
4. **Delivery state dies with the path.** Leaving LIVE clears every
   delivery flag (compose pending, delta ready, history valid) — the
   R70i stale-texNrColor freeze class is structurally impossible.
5. **Kill by PID only**, never by image name; zombie check after any kill.
6. **Recycle (WS budget)** enters DRAINING with reason=Recycle — identical
   path as a workScale change; the difference is only logging.

## Reconfig coalescing table

| Trigger | Fields | Re-arm needed? |
|---|---|---|
| gpuIndex change | transport GPU | full (resources on new adapter) |
| workScale change | engine work dims | full (engine textures) |
| DRS geometry change | render dims | full (buffer sizes) |
| Late guides (armed without guides) | guide metadata | full |
| NR knob change | nrParamSeq bump | NO — engine rebuilds feature itself |
| boost/tint/forceReset | tuning floats | NO — next frame reads them |
| offloadOn=0 | — | NO — evaluate forwards to native while set |

## Race notes

- The queue-observer (submit hook) runs on the game's submit thread and
  MUST NOT take session.cs (R69i): it reads the atomic state; if LIVE it
  pushes the seal command list; anything else is a no-op.
- The ctl poll thread applies fields to the tuning block (outside cs) and
  enqueues reconfigs (inside a tiny mutex on the queue only).
- Game exit (DLL unload / Shutdown export): synchronous DRAINING with a
  1.5 s cap — best effort; the engine's own poison-watch also exits when
  produce goes quiet for 60 s (self-healing orphan).
