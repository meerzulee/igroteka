# CLAUDE.md — Igroteka

Browser ports of open-sourced 2000s W3D/SAGE games (C&C Generals Zero Hour first).
Native WASM ports — no streaming, no emulation. Read `README.md` for vision and
architecture, `PLAN.md` for the current execution plan and phase status.

## Hard rules

- **Reviews go through Codex.** For any design/correctness second opinion, route it to
  Codex (`/codex:rescue`), not the built-in `advisor` tool. Give it adversarial,
  file:line-scoped prompts; it diagnoses, we apply. (Standing user preference.)
  Preferred Codex model is `gpt-5.6`; it is **not enabled on the current
  Codex/ChatGPT account** (rejected: "not supported when using Codex with a
  ChatGPT account"), so use the default Codex model until the account gains it —
  do not keep retrying `--model gpt-5.6`.

## Model routing (standing user preference)

- **Default model is Fable 5.** The user sets it via `/model`. Fable sometimes
  auto-escalates to Opus 4.8 for heavy work; that switch is a harness behavior,
  not something to fight from inside a turn.
- **Delegate neutral / mechanical work to Fable subagents** (`Agent` with
  `model: "fable"`): writing corpus rungs, mechanical HLE stub batches, byte-
  layout fills, rote refactors, search/enumeration. Fable is terse and fast and
  should not refuse this work — it is a lawful, user-owned game-porting project
  (DRM-free GOG assets, MIT runtime written from scratch). Give it clean,
  self-contained specs.
- Keep on the main model (Opus/whatever the session resolves to): security-
  sensitive judgment, the threading/scheduler design, license-boundary calls,
  and anything where a wrong stub silently corrupts the guest.
- Advice/review still routes to **Codex**, never the `advisor` tool.
- **Never commit game assets.** `*.big` and `game-data/` are gitignored on purpose —
  they are EA's copyrighted data. Users bring their own files at runtime (OPFS).
- **License boundary:** `d8web/` is MIT and must stay engine-free — no code from the
  GPL engine fork may be copied into it. `zh-web/` (engine fork) is GPL v3.
  WineD3D may be *read* for D3D8 semantics but never copied (LGPL).
- **Backends stay dumb.** In `d8web`, all intelligence (state tracking, shader
  descriptions, state-hash caching) lives above the backend seam. A backend only
  translates state snapshots into API calls. Never call WebGL/WebGPU directly from
  frontend code — this discipline is what makes the WebGPU backend a swap, not a rewrite.

## Build gotchas

- Engine must compile with `-fwasm-exceptions` — SAGE uses C++ exceptions as control
  flow (INI parse errors throw); without it the engine dies on first bad parse.
- Engine INI parser crashes report the *block start* line, not the failing field —
  see `ReleaseCrashInfo.txt` pattern from native GeneralsX.
- BIG archives load alphabetically with **first-wins** collision rules
  (`ArchiveFileSystem::loadIntoDirectoryTree`, overwrite=FALSE). Mod files use `!`
  prefix to win. `-mod` loads with overwrite=TRUE (beats everything).

## Reference material

- Engine source: fork of https://github.com/TheSuperHackers/GeneralsGameCode
  (GeneralsX fork https://github.com/fbraz3/GeneralsX has the SDL3/POSIX/macOS work —
  useful diff reference for portability fixes)
- Renderer prior art: https://github.com/LostMyCode/d3d9-webgl (MIT) — D3D9 FFP →
  WebGL2 wrapper from the GunZ browser port; `d8web` builds on its approach
- D3D8 semantics oracle: WineD3D (`wine/dlls/wined3d/`) — read-only reference
- The GPU choke point in the engine: `Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.*`
  (all D3D8 calls funnel through class `DX8Wrapper`)
- User has a working native ZH install (GeneralsX Beta 12, macOS) at
  `~/GeneralsX/GeneralsZH` — use it to generate reference behavior/checksums

## Conventions

- Plain CMake + Emscripten toolchain file; no exotic build wrappers
- Milestones are demo-able or they don't count; each phase in PLAN.md has an exit
  criterion — update PLAN.md status when one is met
