# Spec R79 — Same-frame Present-gate composition (user directive 2026-09-29)

Governing directive (fidelity over throughput, R79): fps cost is
ACCEPTED and will be recovered later with FG or SR. The NR output must never
be distorted by artifices — no frame mixing, no per-frame skips, no temporal
band-aids. Frame N on screen = game frame N + delta N, or the system is in a
clean permanent-native degraded state.

User decision (VERBATIM intent): the on-screen frame N must carry ITS OWN delta N,
always. Slower fps is acceptable (will be recovered via workScale downscale or FG
later). NEVER mix frames — delayed composition "loses all quality always".

## Why Present-time (mechanics, verified)

- Today: seal(N) executes inside the game's frame-N GPU work; engine (GPU2)
  needs produce(N) before it can even start delta N (~39-43 ms at workScale 1.0,
  measured R76); delta N can only be composed onto frame N AFTER frame N is fully
  submitted -> the only safe blocking point is the game's Present(N) call
  (fork parity: ApplyToFinishedPicture composes on the swapchain backbuffer).
- Blocking earlier (Evaluate/AfterRealEvaluate) DEADLOCKS: frame N's command list
  would never be submitted, produce(N) never signals.

## Changes

### 1. dxgi_proxy.cpp — swapchain Present hook
- We already forward dxgi exports. Wrap `CreateSwapChain` / `CreateSwapChainForHwnd`
  (and `CreateSwapChainForCoreWindow` if forwarded) to return a thin proxy
  IDXGISwapChain (implement IDXGISwapChain + IDXGISwapChain1 + IDXGISwapChain2 +
  IDXGISwapChain3 + IDXGISwapChain4 by casting on QueryInterface; forward every
  method to the real object; override only `Present` and `Present1`).
- Proxy::Present(Sync, Flags):
  1. call `sli::PresentGate(m_real, Sync, Flags, /*present1=*/false, ...)`.
  2. PresentGate returns the HRESULT of the real call (or S_OK skip).
  - `Present1` same with DXGI_PRESENT_PARAMETERS* passed through.
- Register/deregister proxy must be leak-safe for swapchain recreation (re-arms).

### 2. offload_session — PresentGate + additive draw
New entry (called from dxgi_proxy, game thread at Present):
```
HRESULT PresentGate(IDXGISwapChain* real, UINT sync, UINT flags);
```
- Fast path: session not Live, or tuning.offloadOn==0, or tuning.nrOn==0 ->
  return real->Present(...) untouched (ZERO overhead when off).
- N = the host frame counter of the sealed frame (g_s.frame as bumped by the last
  Evaluate/seal; Present(N) happens after ECL(N) in the same frame).
- Wait for the engine's delivery of frame N: reuse the existing delivery staging
  logic (engineHostFrame / done fence path). The wait is EVENT-BASED, NOT a
  time budget: WaitForMultipleObjects on [delta-delivered event, engine process
  handle]. While the engine is ALIVE the wait is UNBOUNDED — slow engine means
  a longer frame time, never a skipped delta (user directive: no frame mixing,
  no per-frame skips, slowness is acceptable and paid in fps).
  Death handling (the only fallbacks, both DEGRADE PERMANENTLY to native —
  never per-frame flicker):
  - engine process handle signals (crash/driver reset) -> log once,
    ComposeHalt() (offload stays native until re-arm), present native.
  - session re-arm/generation change while waiting -> same: the frame number
    being awaited no longer exists; halt, present native.
  - Optional last-resort hang watchdog: a generous timeout (10 s) that only
    fires if the engine is alive but stuck (GPU2 hang without process death);
    action is the same permanent degrade + log, NOT a per-frame skip.
- Compose ON THE BACKBUFFER, additive fixed-function blend:
  - Get sc->GetBuffer(0) (cache RTV per buffer pointer, up to 4; recreate if
    GetBuffer returns a different resource, e.g., resize).
  - Fullscreen-triangle PSO on the GAME device (gameDev, already in Session):
    OM blend = ADD (SrcAlpha=ONE, Dest=ONE, Op=ADD for RGB and Alpha).
    Pixel shader: sample texDelta (RGBA16F, render dims, SRV linear) at
    screenUV * renderDims; d = delta.rgb * boost; output float4(d, 0).
    Tint mode (tuning.nrTint==1): paint by sign (red=+/blue=- saturated,
    amp 20*boost like nr_delta) INSTEAD of additive — verification path.
  - Backbuffer state at Present is typically PRESENT: transition to
    RENDER_TARGET, draw, transition back, then real->Present.
  - The command list must be the game's graphics queue: record into a NEW host
    command list on the game device's direct queue IF we can get one (we have
    gameDev; create our own direct queue at arm time), execute, wait (the game
    is blocked in Present anyway — a CPU-side ExecuteCommandLists+flush-fence
    wait of a tiny draw is fine; NEVER touch the game's own open list).
- PSO resources (root sig, heap, sampler, PSO, RTV cache) live in ComposeGpu —
  replace the compute compose members (see removals) with the present-draw ones.

### 3. Latency shave in the engine (verify, then only if safe)
loop.cpp currently waits `produce >= frame + 1` before processing frame F.
Check the fence value convention in the host (what value produce is signaled
with at the seal of frame N). If produce value == N after seal N executes, the
engine should wait `>= N` (not N+1) for frame N — one frame less latency.
ONLY do this if the fence semantics confirm; otherwise leave and document.

### 4. Removals (dead code rule — the lag mitigations are superseded)
- AfterRealEvaluate compose path entirely (AfterRealEvaluateInner becomes a no-op
  or is removed from the ECL hook; keep the ECL hook itself for Evaluate/seal).
- Accumulator: texHist[2], hist swap, chain-reproject apply branch, histValid,
  residualHistoryPrime logic, cBlend/cHistValid constants.
- texMv per-seal refresh (added R78) and the compose texMv usage — no reprojection.
- LumaWeight in compose_delta.hlsl — the whole compute compose shader is unused
  now: DELETE shaders/compose_delta.hlsl and its compilation/embedding. New
  small shader: shaders/present_add.hlsl (the fullscreen-tri PS above).
- nrTint==2 (structural) — was accumulator-based. Tristate becomes 0=off /
  1=raw-tint paint (keep for chain verification). Panel row text updated if it
  documents tint modes (panel.cpp: any mention of tint=2 — update label/help).
- Keep: seal/capture, engine, delivery staging (delta + engineHostFrame + done
  fence), F6/F7/F8 hotkeys, ctl, ini, ABI (unused fields stay — ABI stability).

### 5. Build & offline gates
- `cmd /c build_env.bat cmake --build build` — 0 warnings (/W4 /WX).
- host_smoke: all 9 must PASS (update if any test referenced the compose path).
- replay_feeder G1/G2/G3 must PASS (engine path untouched).

## Invariants (do not break)
- NEVER block the game thread outside Present; inside Present the wait is
  event-bounded (delta event / engine death / generation), not time-bounded
  except the 10 s stuck-engine watchdog whose action is a PERMANENT native
  degrade — per-frame skipping is forbidden (user directive R79).
- NEVER call Reset/free on in-flight resources; the draw list is our own.
- dxgi.dll keeps ALL 38 forwarders intact (R75 rule).
- The ECL/Evaluate shields (try/catch -> re-arm) stay as they are.
- CRLF->LF aware; comments in EN; ancla unica en patches.

## Out of scope (do not do)
- No luma-weighted apply at present-time (needs dst read; revisit only if
  dark-scene grain persists with same-frame deltas).
- No FG, no downscale changes, no multipass (next rounds).

## Reference discipline (user directive R79b)
- The fork is the REFERENCE for its CURRENT working functionality only.
  Discriminate optional/beta/experimental fork features (ResidualAcrossRR
  accumulator was itself opt-in in the fork) — do not port them. No
  artifices on the NR output: fidelity first; fps recovered later via
  workScale (downscale before NR — VERIFIED WORKING) or FG/SR.
- Keep the swapchain proxy structured as the future hook point for FG and
  SR composition (both nvidia and alternative backends): the Present-gate
  draw helper must be backend-agnostic (a named compose stage, not a
  one-off NR hack).
