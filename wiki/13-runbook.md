# Runbook

## Deploy (game dir)

1. Close the game (files lock while running).
2. Copy `dxgi.dll`, `nvngx.dll`, `sli_engine.exe`, `sli_panel.exe`,
   `sli_ctl.exe` next to the game executable.
3. The vendor NR runtime (`nvngx.dll_dlssnr.dll` + its `_nvngx.dll` core)
   must be present — the engine loads it from the registry NGX path or the
   game dir (user-provided; never distributed by us).
4. Launch the game. The log (`sli_host.log` in the game dir) shows
   `configure ok` when the proxy arms.

## Signals of life

| What to look at | Healthy |
|---|---|
| `sli_host.log` | `armed` → `engine ready` → composes counting |
| `sli_engine.log` | `first frame computed` then ENHANCED lines |
| Panel status dot | LIVE (heartbeat < 2 s) |
| `sli_ctl` mirror dump | nr.on=1, boost sane, enginePid set |

## Hotkeys (installed at proxy load, not first arm)

- **F7** offload ⇄ native (instant A/B)
- **F8** reset tuning to defaults
- **Ctrl+F9** open/focus the panel

## Tear-down / stuck engine

- Kill ALWAYS by PID (never /IM): engine PID is in the panel About tab and
  the ctl mirror (`enginePid`).
- Black frozen screen with live processes = poisoned delivery: kill
  game+engine+panel by PID and relaunch clean.
- Redeploy requires game AND engine closed; verify with tasklist once more
  after 3 s (launchers relaunch quickly).

## Config

`sli.ini` beside the DLLs (host reloads at arm): persisted panel state.
Overrides use −1 = game wins; NR knobs are always-value.
