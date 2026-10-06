# Panel implementation spec (F4) — ControlSpec single table

Adopted design. The panel is Dear ImGui
(Win32+DX11 backend, vendored). ONE ControlSpec table drives HWND-free
rendering, ini keys, ctl fields, ranges, defaults and the ALWAYS-VISIBLE
help text. No parallel arrays anywhere.

## ControlSpec fields

```cpp
struct ControlSpec {
    const char* label;        // short English label
    const char* key;          // ini key
    int ctlField;             // ctl v3 field id (or -1 = local only)
    CtrlType type;            // Checkbox / SliderF / SliderI / Combo / Static
    float lo, hi, def, step;  // slider bounds (ignored otherwise)
    bool overrideable;        // game-override semantics (shows "Game: <v>" + OVR checkbox)
    const char* what;         // WHAT IT DOES (plain language, always visible)
    const char* sends;        // WHAT IS ACTUALLY SENT (field name, when read, re-arm cost)
    const char* deviate;      // SIDE EFFECT OF DEVIATING
};
```

## Tabs (5)

1. **Upscaling** — backend combo (DLSS only; FSR/XeSS "(planned)" disabled),
   offload checkbox (live A/B), GPU combo, then the game-override sliders
   (sharpness, mvScale x/y, jitter x/y, pre-exposure, exposure scale, mv
   offset x/y, frame time) each with Override checkbox; without OVR they
   show "Game: <value>" from the mirror.
2. **Frame Generation** — placeholder combo (Off / DLSS-G (planned) /
   FSR-FG (planned)) + status text.
3. **Neural Rendering** — nrOn checkbox; style combo; knob sliders
   (ALWAYS-VALUE, no override): intensity, local structure, local tone,
   skin structure, work scale (warn: re-arm ~2 s), boost (warn: A/B knob
   only, 1.0 = fork exact); delta-tint checkbox (live, no re-arm).
4. **Debug & Verification** — capture frames, reset history, host status
   dot + heartbeat, engine counters (produce/done/engineResult), event log
   (last N ctl ops with ACK state).
5. **About** — versions (panel/ctl protocol/ABI), build hash, repo link,
   license, credits.

## Semantics rules

- Apply = live (ctl SET batch) immediately; Save = persist ini; the host
  reloads ini at (re)arm. "Saved but not applied" indicator when they
  diverge.
- Mirror polling every 500 ms (PING + GET mirror) — the UI shows the REAL
  state, never just the local one.
- Re-arm-costing controls (gpuIndex, workScale) show a ⚠ "re-arms (~2 s)"
  hint inline.
- Nothing free-text: bounded inputs only.

## Status derivation

hostLive = PING ack < 2 s; engineLive = mirror.engineResult==1 &&
done counter advancing. Show ● / ○ accordingly.
