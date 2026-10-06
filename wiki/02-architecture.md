# Architecture

## The idea

The game's own DLSS Super Resolution keeps running exactly as installed —
we never replace the upscaler. Around it, a second GPU runs the DLSS-NR
neural-rendering model and produces a **display-domain delta** ("what NR
would change about this frame"), which is composed back into the game's
presented image where the game allows it. If anything fails at any point,
the game falls back to its unmodified native path — frame pacing is never
at risk.

```
        GAME PROCESS (GPU 0)                      ENGINE PROCESS (GPU 1)
┌──────────────────────────────┐          ┌──────────────────────────────┐
│ RDR2 / any DLSS DX12 game    │          │ sli_engine.exe               │
│  └ dxgi.dll  (load vector)   │          │  └ vendor DLSS-NR runtime    │
│     └ nvngx.dll (OUR proxy)  │          │    (NGX feature 18, local)   │
│        ├ forward to real NGX│          │  └ codec: HDR ⇄ BGRA8 carrier │
│        ├ seal Color+Depth+MV│──share──►│  └ output: DELTA ONLY [-1,1]  │
│        └ compose delta back │◄─fences──└──────────────────────────────┘
│ sli_panel.exe ◄─ctl v3─► host│
└──────────────────────────────┘
```

## Components

| Binary | Role |
|---|---|
| `dxgi.dll` | Load vector sitting next to the game: forwards the real dxgi exports and lazily loads our nvngx on first use (never from DllMain). |
| `nvngx.dll` | The NGX proxy: forwards every export to the driver's real NGX; intercepts `EvaluateFeature` to (1) seal the frame's color/depth/MV into shared cross-adapter buffers at the exact in-list point, (2) optionally compose the NR delta after the real evaluate. Init is forwarded once and cached (the driver retried 1.2M times against a naive proxy). |
| `sli_engine.exe` | Hidden worker on GPU 2. Opens the shared transport, runs the vendor NR model in-process (feature 18) with the BGRA8 codec, and writes ONLY the delta (RGBA16F, display domain, saturation-guarded). Zero-delta on any failure — base + 0 = untouched frame. |
| `sli_panel.exe` | ImGui panel: every control bounded and explained in plain language (what it does / what the engine actually receives / the side effect of deviating). Talks ctl v3 to the host. |
| `sli_ctl.exe` | CLI for the same protocol (scripting/diagnosis). |

## Data flow per frame

1. **Evaluate interception** (game thread, inside the game's own evaluate
   call): read params 3-step typed→untyped; resolve dims/subrects. Not
   eligible → forward to native, done (return false, game never blocked).
2. **Seal** (in-list, same command list the game's evaluate runs on): copy
   Color into the shared inColor buffer at the subrect seam; depth via the
   DSV-identical clone (net-zero transitions); MVs on the frame's own seal
   list submitted right behind the frame (never barriers on game textures
   inside the game's list — device corruption, POC R69q).
3. **Produce** (queue-observer hook, after the real submit): signal the
   produce fence GPU-ordered behind the frame.
4. **Engine frame** (GPU 2, one command session): copy guides from the
   triple-buffer ring [(N-1)%3] → encode HDR→BGRA8 (exposure from last
   frame's tiles) → vendor evaluate at work dims → decode as DELTA with
   saturation guard → resample to render dims → write to the shared out
   buffer → done fence.
5. **Compose** (host, in-list after the real evaluate, on the texture the
   game will actually present — per-game arrival config): delta is
   MV-reprojected and temporally accumulated (fork ResidualBlend 0.08 —
   uncorrelated per-frame noise averages to zero, the enhancement
   persists), then applied: `out = base + accumulated_delta * boost`.
   The delta-tint flag paints the delta red/blue instead (verification).

## Session lifecycle (FSM)

`IDLE → ARMING → LIVE → DRAINING → COOLDOWN → IDLE` — full design in
[docs/fsm-design.md](../docs/fsm-design.md). Resources are created only in
ARMING; every reconfig (GPU, workScale, DRS, late guides) drains first.
The game thread never waits on the FSM: not-LIVE = forward to native.

## Verification surfaces

- **Delta-tint** (structural): paints the delta itself — if the tint
  reaches screen, the whole chain is alive. Live knob, no re-arm.
- **Counters** (diag): produce/done/engineResult/engineHostFrame readable
  from the mapping — the RDR2-grade verification (RDR2 never presents the
  composed texture; see the verification page).
- **Dumps** (forensics): one-shot readbacks at pipeline boundaries.

## Present-path asymmetry (the R73c truth)

RDR2 copies the DLSS output before presenting it — a post-SR compose there
is invisible on screen (proven with a write-only checkerboard while 52.8k
composes flowed). Visible NR in such games requires the pre-SR route
(retired by design: it substituted the SR's input frame) or a
game-specific present hook (future work). Games that DO present the
upscaler output (e.g. Cities: Skylines II) get the full visible path;
that asymmetry is documented per game, not hidden.
