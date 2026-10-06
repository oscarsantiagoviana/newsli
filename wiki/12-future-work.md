# Future work

## Near (roadmap phases)

- **F2-F4**: engine delta-only, host FSM, panel v3 (see
  the execution plan for gates).
- **F5**: publish pass — visual verification in a presenter game (CS2).

## Backends (F6)

FSR2 via public AMD SDK (GetProcAddress, context on GPU 2) → XeSS (DP4a
runs fine on the 3060s) → ffx-api FSR3.1. DLSS-D passthrough possible.

## Frame generation

Own module: swapchain layer (Present hook + HUD fix + pacing). Anti-cheat
risk documented; single-player first. OptiFG-style in-evaluate FG is NOT
pursued (their own changelog marks it experimental).

## Known debts (tracked, not hidden)

- Vendor runtime leak (~10 MB/s) — recycle by budget mitigates; a real fix
  means runtime internals.
- vtable ECL patch assumes the queue layout — signature check in debug
  builds, documented limitation.
- Per-game present-path asymmetry table needs population as games are
  tested (RDR2 = not presented; CS2 = presented).
- Distillation of the vendor model into an own engine (the TRT line's
  original goal) — only if the vendor path ever closes.
