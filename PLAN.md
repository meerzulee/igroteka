# Igroteka — Execution Plan

Status legend: `[ ]` not started · `[~]` in progress · `[x]` done
Update this file when an exit criterion is met. Last updated: 2026-07-05 (project start).

> This file is **Track 1** (Dvijoke — engine-source ports). Track 2 — sourceless
> games via a from-scratch Win32-on-wasm runtime, flagship Rome: Total War —
> lives in [FORTOCHKA.md](FORTOCHKA.md).

---

## Phase 0 — Headless sim in the browser (target: 3–6 weeks)

Goal: prove compiler, memory model, filesystem, and determinism before writing any
graphics code. This kills ~90% of unknown project risk.

- [ ] Fork GeneralsGameCode → `zh-web/`; get a **native** build running first
      (known-good baseline for comparison; GeneralsX fork is the portability diff reference)
- [ ] Emscripten toolchain: emsdk pin, CMake preset, `-fwasm-exceptions`,
      initial memory 512MB, `ALLOW_MEMORY_GROWTH`
- [ ] Stub renderer: null device behind `DX8Wrapper`
      (`Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.*`) — engine boots with no GPU
- [ ] Stub audio (null OpenAL), stub movies (skip Bink playback)
- [ ] Filesystem: wire engine file layer to Emscripten FS; `.big` upload UI →
      OPFS; MEMFS + small asset subset for fast dev iteration
- [ ] Boot sequence in browser: INI parse → subsystem init → map load → sim ticks
- [ ] Determinism harness: per-tick game-state checksum, dumped from both native and
      WASM runs of the same skirmish start; diff tooling for divergence hunting

**Exit criterion:** browser console shows a skirmish map loaded, AI-vs-AI sim ticks
advancing, and N-tick checksum identical to the native run.

**2026-07-09 findings — the determinism harness is mostly pre-built upstream.**
The SuperHackers fork already ships a headless replay-CRC validator we can reuse
instead of writing one:
- `ReplaySimulation::simulateReplays` (`Core/GameEngine/Source/Common/ReplaySimulation.cpp`)
  — `-replay <file> -headless` boots straight into it via `GameMain.cpp:48`, loops
  `GameLogic::UPDATE()` while `isPlaybackInProgress()`, and returns a nonzero **exit code
  on `sawCRCMismatch()`**. That exit code IS the determinism verdict.
- The CRC compute+emit (`GameLogic::getCRC(CRC_RECALC)` → `MSG_LOGIC_CRC`, `GameLogic.cpp:3860`)
  and mismatch plumbing (`Network::sawCRCMismatch`) are **production code, not DEBUG_CRC-gated**
  — they run in the current Release `wasm` build. `DEBUG_CRC` (auto-on with `RTS_DEBUG_LOGGING`)
  is only needed to *localize* which tick diverges, via per-frame CRC logs.
- Float divergence (the "existential" risk below) is already mitigated: `cmake/gamemath.cmake`
  sets `SAGE_USE_DETERMINISTIC_MATH=ON` (fdlibm) for cross-platform replay validation.
- `DETERMINISTIC` build flag (`RandomValue.cpp:100`) force-seeds RNG=0, but replays don't
  need it — a replay stores + replays its own seed. Re-running a *fresh* skirmish twice would
  diverge on the wall-clock seed, so REPLAY (not re-run) is the correct harness.

Corrected critical path: (1) get skirmish-vs-AI to START + TICK in WASM (HANDOFF's "next big
test" — the real gate); (2) play it once → auto-records `LastReplay.rep`; (3) run
`-replay LastReplay.rep -headless` in a **Web Worker or Node** (needs no WebGL2, blocking OK)
→ read exit code; (4) if mismatch, build a `wasm-crc` logging variant to find the diverging tick.
Milestones 1a/1b (shell map + menu, below) are DONE per HANDOFF — status not yet ticked there.

Known risks here:
- 32/64-bit assumptions — wasm32 pointers match the engine's 32-bit heritage (helps),
  but `long` sizes and struct packing need auditing
- FPU determinism: engine calls `setFPMode()` for consistent x87 rounding; WASM floats
  are IEEE-754 strict — likely *better*, but native-vs-WASM checksum may legitimately
  diverge on float ops. If so: fall back to WASM-vs-WASM determinism (sufficient for
  multiplayer) and note it.
- Engine main loop must yield to the browser: restructure `GameEngine::execute()` into
  an `emscripten_set_main_loop` callback

---

## Phase 1 — Pixels: d8web renderer (target: 2–3 months)

Goal: WebGL2 backend behind `DX8Wrapper`, built on d3d9-webgl (MIT, credited).

- [ ] Scope the real API surface: grep engine for every D3D8 call `DX8Wrapper` makes;
      produce `d8web/INTERFACE.md` (expected: ~50–70 methods, not the full ~200)
- [ ] `d8web/` repo: fork d3d9-webgl; strip GunZ-specific hacks (lightmap stage
      special-casing noted in its comments)
- [ ] D3D8 interface shim over the D3D9-shaped core (crosire/d3d8to9 as the mapping
      reference — interface deltas, no shader translation needed since FFP-only)
- [ ] **Backend seam refactor** (the WebGPU insurance — do it early, it's cheap here):
  - [ ] All D3D8 render state → snapshot structs; backends read snapshots at draw time
  - [ ] FFP shader *generator* module: state struct in → GLSL out (WGSL emitter slots
        in beside it later)
  - [ ] State-hash → program/pipeline cache (WebGL: program lookup; WebGPU later:
        pipeline objects)
  - [ ] `updateBuffer()` abstraction over Lock/Unlock (WebGL: `bufferSubData`;
        WebGPU later: ring buffer + `queue.writeBuffer`)
- [x] d8web v0 bootstrap (2026-07-05): own frontend/seam/WebGL2 backend written from
      scratch (d3d9-webgl kept as reference); rotating lit cube renders correctly in
      browser through pure D3D8 calls. Bugs fixed en route: Emscripten
      `-sGROWABLE_ARRAYBUFFERS=0` (Chrome rejects resizable views in bufferSubData),
      cull-mode winding inverted (D3D y-down vs GL y-up screen space), stage-0
      texture defaults, fog distance sign, alpha-test quantization, light-count clamp.
- [ ] Codex review findings still open (from 2026-07-05 consult, session in
      `.context/codex-session-id`):
  - [ ] D3DMCS material color sources (COLORVERTEX, DIFFUSE/AMBIENT/EMISSIVE
        MATERIALSOURCE) — currently hardcoded, needs ShaderKey bits
  - [ ] L8/A8L8 sampled as-is — need .rrr / .rrrg swizzle keyed by texture format
  - [ ] NORMALIZENORMALS ignored; normal matrix assumes rigid transforms
        (fine for ZH? verify — W3D may use scaled world matrices)
  - [ ] Stencil, fill mode, shade mode, specular recorded but not applied
  - [ ] Texture transforms (D3DTSS_TEXTURETRANSFORMFLAGS) not implemented
- [ ] Close d3d9-webgl gaps ZH needs:
  - [ ] Directional lights (ZH's sun — d3d9-webgl only has 3 point lights)
  - [ ] Fog mode audit (linear implemented; verify ZH never needs EXP/EXP2)
  - [ ] Alpha test coverage (foliage, fences, UI use it heavily)
  - [ ] Texture-stage combine ops the terrain multi-texturing uses
- [ ] Milestone ladder (each one demo-able):
  - [ ] 1a. Shell map renders (the animated main-menu background battle)
  - [ ] 1b. Main menu UI usable (.wnd rendering + mouse picking)
  - [ ] 1c. Skirmish terrain renders in-game
  - [ ] 1d. Units render + animate (skeletal animation through vertex paths)
  - [ ] 1e. Particles, projectiles, explosions
  - [ ] 1f. Full AI skirmish visually correct vs native side-by-side screenshots

**Exit criterion:** complete skirmish vs AI, start to victory screen, visually
comparable to native GeneralsX.

Debugging oracle: WineD3D source for D3D8 semantic edge cases (read, never copy — LGPL).

---

## Phase 2 — Playable single-player (target: 1 month)

- [ ] Input feel: SDL3→browser mouse capture, edge scrolling, right-drag scroll,
      keyboard shortcuts (browser hijacks some — remap or PWA fullscreen)
- [ ] Audio: OpenAL → Emscripten OpenAL; music streaming from OPFS
- [ ] Saves/replays → OPFS persistence
- [ ] Performance: 60fps on latest iPad Safari + mid-range Android Chrome;
      profile draw-call hot spots, texture upload stalls
- [ ] Asset onboarding UX: guided "point at your Steam folder" flow, validation
      (the base-vs-ZH archive split matters — see CLAUDE.md build gotchas),
      OPFS cache management UI
- [ ] Mod loading: `-mod` equivalent in UI (loads with overwrite=TRUE; Contra,
      ShockWave, ROTR are pure-data and should ride along free)

**Exit criterion:** a stranger with a Steam copy of ZH gets from URL to playing
skirmish in under 10 minutes, no docs.

---

## Phase 3 — The café: multiplayer (target: 1–2 months)

- [ ] Transport: replace dead GameSpy/UDP layer with WebRTC DataChannels
      (unreliable/unordered mode) behind the engine's transport interface
- [ ] Signaling + lobby server: rooms, player lists, NAT punching (STUN; TURN relay
      fallback) — Cloudflare Worker + Durable Objects scale is plenty
- [ ] Lockstep integration: ZH already sends per-turn command packets; map onto
      DataChannel; desync detection via the Phase 0 checksum harness
- [ ] Lobby web UI: create/join room, map picker, army picker, chat
- [ ] Latency tuning: command turn length vs RTT; spectator-safe pause on drop
- [x] In-game HUD (top-right): player name (truncated if long) + live ping in ms,
      per-player rows with host crown, room code + player count. **Built
      2026-07-10** as an HTML overlay on the play page (`site/play/zh/index.html`)
      reading `window.CafeUdp.status()`. Ping source = **DataChannel RTT** from the
      bridge (`getStats()` candidate-pair `currentRoundTripTime`, polled 2s) — no
      engine change needed, so it's live now. Player names are HTML-escaped.
> **Sequencing decision (2026-07-10):** fix the in-game disconnection stall FIRST
> (matches must play through), THEN build mid-game rejoin on top. Rejoin depends on
> a stable, non-stalling in-match transport anyway, so this order is also technical.

- [ ] **Disconnection Menu / mid-match lockstep stall** (observed 2026-07-10: match
      loads + renders, then the engine pops "DISCONNECTION MENU" mid-game). Root
      cause: the deterministic-lockstep turn barrier can't advance because a peer's
      turn-N command packets stop arriving in time. The DataChannel is
      unreliable/unordered (UDP-like) and the engine's own `Transport`/
      `ConnectionManager` ACK-resend is supposed to cover loss, but under the wasm
      single-threaded main loop a slow/blocked recv starves the JS event loop, so
      DataChannel messages don't get delivered → the barrier times out → disconnect
      vote. Needs: (a) confirm the transport ACK/resend actually runs each frame on
      wasm (the non-blocking state-machine barrier, not a spin-wait Sleep), (b)
      desync-vs-drop distinction via the Phase-0 CRC harness, (c) tune turn length
      vs RTT. This is THE core Phase-3 netcode task.
- [ ] Wire the Disconnection Menu's empty ping column to the same latency source
      (`Connection::m_averageLatency` or the bridge RTT). Cosmetic vs the stall above.
- [ ] Auto-join / autopilot: boot params (`?host=1` / `?autojoin=1`) drive the
      engine straight into host/join, skipping the LAN-lobby hunt. Igroteka is the
      gathering layer (public area + party rooms); "Start" opens every player's tab
      with the param → straight into the game. (Auto-join built 2026-07-09.)
- [ ] **Mid-game reconnect** ("close tab → reopen → rejoin the LIVE match" — user
      ask 2026-07-10). Retail ZH could NOT do this (a dropped player was gone), so
      it's a custom feature. What already works: the cafe **identity/token persists**
      (localStorage keyed by room, 30-min TTL), so reopening reconnects to the ROOM
      and — if the match hasn't started yet — the autopilot rejoins the lobby. The
      wall is the **simulation state**: deterministic lockstep means a client's game
      state IS `seed + every command from turn 0`; a closed tab loses the in-memory
      sim entirely. To rejoin a running match the returner must rebuild that exact
      state. Mechanic:
        1. **Determinism gate first** (Phase-0 CRC harness green) — else replay won't
           reproduce the state and you desync. Hard prerequisite; not built yet.
        2. A live peer (or the host) **buffers the full command log** (+ periodic state
           snapshots so catch-up isn't always from turn 0). ZH's replay system already
           records command logs — reuse it.
        3. On reopen: fetch the log/snapshot from a peer over a DataChannel, **replay
           deterministically** at high speed to the current turn, then splice into live
           lockstep.
        4. During catch-up the other players **pause** (the existing Disconnection
           Menu vote is the natural hook — "wait for player").
      Sub-milestone that's cheaper + useful now: **graceful match-in-progress handling**
      — when a reopen finds the LAN game already started (gone from the lobby list),
      the autopilot currently waits forever; instead detect it and show "match already
      in progress — can't rejoin yet" rather than hanging.

**Exit criterion (v1.0):** two browsers on different networks complete a 1v1 skirmish.
The café moment.

**2026-07-09 seam map (from code exploration + Codex + advisor — all converged):**
- **Transport swap point = the concrete `UDP` class** (`Core/GameEngine/Source/GameNetwork/udp.cpp`).
  Reimplement `Bind`/`Write`/`Read`/`SetBlocking` over a WebRTC DataChannel — everything above
  (`Transport` CRC/XOR/queues, the whole lockstep system, desync detection) stays byte-for-byte.
  No virtual transport interface exists (both `UDP` and `Transport` are concrete) → reimplement
  the UDP body, don't refactor. Inject at `new Transport` (`NAT.cpp:608` / `ConnectionManager.cpp:1603`).
- **Library:** `datachannel-wasm` (MIT, same `rtc::` API as native libdatachannel). Don't hand-roll
  EM_JS — buffer lifetime + `bufferedAmount` backpressure + connection state get nasty.
- **Two impedance mismatches** (the actual shim work): (a) engine demuxes peers by `(uint32 IP,
  uint16 port)` — map `(addr,port)` ↔ synthetic peer-id; (b) one shared `Transport` serves ALL
  peers (`ConnectionManager.cpp:2056`) — multiplex N DataChannels behind the one UDP shim.
- **Barrier is already non-blocking poll-based** (`isFrameDataReady()` gate, `GameEngine.cpp:943`)
  — maps onto the rAF loop. Init-path blockers to neutralize: 1000ms busy-wait bind
  (`Transport.cpp:117`), pregame `while(!isProgressComplete()){Sleep(100)}` (`GameLogic.cpp:2310`),
  blocking `gethostbyname`. Cap `doRecv` drain per tick. DataChannel `onmessage` → append to inbox
  only, consume at a deterministic point in `Network::liteupdate()` (never mutate sim from callback).
- **Infra: no self-hosted network needed.** Signaling = CF Worker + Durable Object (write `cafe/`,
  MIT). STUN+TURN = `turn.cloudflare.com` (TURN $0.05/GB relayed; most traffic pure P2P = $0).
  Game traffic is browser↔browser P2P, never through our server.
- **Gated on Phase 0 determinism** (above). `cafe/` signaling is the only piece with zero blockers.

---

## Phase 4 — Platform (ongoing)

- [ ] Renegade port (`renegade-web/`): EA source released; older W3D revision —
      diff its DX8Wrapper against Generals', adapt d8web shim
- [ ] W3D Hub free games (A Path Beyond, Interim Apex): freely distributable assets
      → the public demo nobody has to own a game for. Talk to W3D Hub early.
- [ ] Replays + spectate mode (lockstep makes this nearly free)
- [ ] WebGPU backend (v2.0): WGSL emitter beside GLSL, pipeline cache over the
      existing state-hash layer, alpha-test→`discard`. Runtime backend pick with
      WebGL2 fallback.
- [ ] Optional native backend (Dawn or sokol): one renderer for desktop ports too;
      candidate upstream contribution to GeneralsX/GeneralsGameCode
- [ ] BFME readiness: if EA ever releases it, it's the same SAGE lineage — wrapper
      should slot in

---

## Community & comms

- [ ] RFC post in GeneralsGameCode discussions before Phase 0 ends — plant the flag,
      recruit expertise (they know where the engine's bodies are buried)
- [ ] Public repo from day one (GPL obligations + recruiting); org: `igroteka-games`
      or `playigroteka` (both free as of 2026-07-05; `github.com/igroteka` is taken)
- [ ] Demo video per milestone — shell map rendering in a browser tab is already
      a screenshot-worthy first post

## Standing risks

| Risk | Mitigation |
|---|---|
| Renderer takes longer than pixels-optimism suggests | Milestone ladder keeps morale + external interest alive; each rung is demo-able |
| Native-vs-WASM float divergence breaks the checksum plan | WASM-vs-WASM determinism is what multiplayer actually needs |
| EA legal posture shifts | GPL engine + no hosted assets + no trademark use = the defensible position; W3D Hub titles keep the public demo clean |
| Solo-project stall | Upstream RFC + public milestones convert watchers into contributors |
| d3d9-webgl abandonment | It's 13.7k lines MIT — we own our fork outright either way |


## Asset onboarding plan (2026-07-06)

Principle: game bits touch only the user's devices and the user's clouds.
Igroteka infrastructure never stores or transmits assets — only engine builds,
the wizard, and user-created data (saves/settings/replays, later via cafe/).

Wizard import doors (all → OPFS, validated client-side by name+SHA-256 manifest):
1. Local folder — showDirectoryPicker (Chromium) / webkitdirectory fallback (v1)
2. Asset pack .zip — single-file pick; the iPad path via AirDrop/Files (v1)
3. User's own cloud — iCloud Drive via Files picker; Google Drive & Dropbox via
   client-side OAuth (CORS APIs, tokens never touch our server) (v1.x)
4. Export asset pack — slim the Steam install to required .bigs, zip for
   AirDrop to other devices (v1.x; manual Finder-compress instructions in v1)

Legal red lines (documented for ToS): no asset hosting even per-user
(MP3.com/Cablevision line), no Steam credentials ever (steamcmd-on-server is
banned), Steam OpenID sign-in later = identity + ownership badge only.

iPad: transfer story = asset pack; the real gate is memory — needs the lazy
OPFS VFS (range-read .bigs on demand) instead of full MEMFS preload.
