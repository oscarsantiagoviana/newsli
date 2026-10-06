# Vision and goals

## Framing (why this repository exists)

This is a personal research laboratory, not a product effort — optimal
operation was never the goal. It serves two purposes at once:

1. **An AI-methodology exercise**: the project was built by steering a
   fleet of generative-AI tools (zcode, the Hermes agent, z.ai models)
   through a domain far from the author's specialization, at a scale
   where unit tests cannot cover the whole integration surface — the
   verification culture documented across this wiki (gates, A/B
   discipline, honest logging) is as much the experiment as the code.
2. **A materialization seed**: a working sketch of what the author
   believes is the future shape of digital animation — dual engines,
   where a rule engine generates scenario/plot/characters/actions and a
   media engine (the one you train on styles and scenography) renders
   the audiovisual content from those directives. Real-time neural
   post-processing that sits beside a host app, consumes its outputs
   over an interop transport, computes on separate hardware and returns
   a correction within the frame's own deadline is a small concrete
   piece of that second engine.

## What

Offload neural rendering (DLSS-NR) to a second GPU while the game's own
DLSS Super Resolution runs untouched — and give the user live, explained
control over every parameter.

## Why

- The NR model costs GPU time the primary GPU cannot spare; a second GPU
  sitting idle can run it for free.
- The vendor runtime runs the model in-process only; making it work in a
  separate process on another adapter took the whole POC (see the
  historical worklog) — this project is that knowledge, distilled.
- Upscaler choice should be the user's (DLSS today; FSR2/XeSS are
  architecturally anticipated through the same evaluate interception).

## Ground rules (inherited from the POC, kept enforceable)

1. **The game never loses frames.** Any failure forwards to the native
   path; frame pacing is sacred.
2. **Structural fixes only** — if something is needed, it IS the code
   path, never an option.
3. **No dead code** — deleted, recovered from git if ever needed.
4. **Every parameter explained** in the panel (what it does / what the
   engine really receives / side effect of deviating).
5. **A/B before believing** any hypothesis; measure in the regime where
   the problem is visible (a day A/B does not validate night).
6. **Read the reference before theorizing** (OptiScaler-family sources
   are on disk for contrast — the golden rule born from 5 avoidable
   crashes).

## Hardware this was built and measured on

2× RTX 3060 12 GB (sm_86), driver 616.64, Windows 10 19044. Test game:
RDR2 (2560×1080, DRS) + Cities: Skylines II as the presenter-game for
visual verification.
