# Igroteka

**игротека** — the 2000s gaming café, in your browser.

Igroteka brings classic 2000s PC games to the browser as real native ports — no streaming,
no emulation. Game engines compiled to WebAssembly, rendering through a Direct3D 8
translation layer, multiplayer over WebRTC. Bring your own game files, click a link, play
a skirmish with friends like it's 2004 at the computer club.

## Why this is possible now

- EA released the source code for **C&C Generals / Zero Hour** and **Renegade** under GPL v3 (Feb 2025)
- Both run on the W3D/SAGE engine, which funnels every GPU call through a single
  internal layer (`DX8Wrapper`) — one narrow surface to reimplement on WebGL2
- The engine is deterministic-lockstep C++ — ideal for WASM and thin-command multiplayer
- Nobody has done it: no SAGE game runs in a browser, and no D3D8→WebGL/WebGPU
  translation layer exists anywhere (verified July 2026)

## Architecture

```
Game engine (GPL fork, compiled to WASM via Emscripten)
   └─ DX8Wrapper (engine's own D3D8 choke point)
        └─ d8web frontend: D3D8 interface + state tracker + FFP shader generator
             └─ Backend: WebGL2 (v1) │ WebGPU (v2) │ native (maybe v3)
Assets: user-supplied .big files → OPFS (never hosted by us)
Netcode: WebRTC DataChannels (unreliable) + lobby/signaling server
```

Backend rule: backends stay dumb. All intelligence (state tracking, shader description,
state-hash caching) lives above the seam; a backend only translates state snapshots into
API calls. Adding WebGPU later = new WGSL emitter + new translator, nothing else moves.

## Components

| Component | License | Purpose |
|---|---|---|
| `d8web` | MIT | D3D8 → WebGL2/WebGPU translation layer (builds on [d3d9-webgl](https://github.com/LostMyCode/d3d9-webgl), MIT) |
| `zh-web` | GPL v3 | [GeneralsGameCode](https://github.com/TheSuperHackers/GeneralsGameCode) fork + Emscripten toolchain |
| `cafe` | MIT | Lobby web app, WebRTC signaling, OPFS asset onboarding |
| `site` | — | Landing page, docs, demos |

License boundary matters: `d8web` stays MIT and engine-free so any project can adopt it;
GPL engine code never leaks into it.

## Roadmap

| Ver | Milestone | Renderer |
|---|---|---|
| 0.1 | Headless: sim ticks in browser, deterministic checksum matches native run | null |
| 0.2 | Shell map + main menu render | WebGL2 (d8web v0) |
| 0.3 | Skirmish vs AI playable end-to-end; d8web refactored around the backend seam | WebGL2 |
| 0.5 | Audio, saves, mods (`-mod` UI), 60 fps on iPad | WebGL2 |
| **1.0** | **Two browsers, one skirmish** — WebRTC lockstep + lobby | WebGL2 |
| 1.x | Renegade port + W3D Hub free games (public demo — freely distributable assets) | WebGL2 |
| 2.0 | WebGPU backend (WGSL emitter, pipeline cache on existing state-hash layer) | dual |
| 2.x | Optional native backend (Dawn/sokol) — same renderer for desktop ports | triple |

## Technical notes

- Compile with `-fwasm-exceptions` — the engine uses C++ exceptions as control flow
- d8web gaps to close over d3d9-webgl: D3D8 interface shim, directional lights
  (ZH's sun), fog mode coverage, texture-stage generalization
- WebGL2 chosen over WebGPU for v1: maximum device reach (every iPad since ~2021,
  Chromebooks, school laptops) and the only existing prior art is WebGL2.
  ZH's workload (a few hundred draw calls) never touches WebGL2's limits.
- WineD3D source is the reference for D3D8 semantic edge cases (read, don't copy — LGPL)

## Legal

- Engine forks are GPL v3 and stay open source
- Game assets are copyrighted and never hosted — users supply their own files
  (Steam), cached locally in OPFS. W3D Hub titles have freely distributable assets
  and serve as the public demo
- This project is not affiliated with or endorsed by EA
