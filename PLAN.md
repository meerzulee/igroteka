# Igroteka — Execution Plan

Status legend: `[ ]` not started · `[~]` in progress · `[x]` done
Update this file when an exit criterion is met. Last updated: 2026-07-05 (project start).

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

## Café accounts (started 2026-07-10)

Identity layer ahead of the lobby (lobby is the next step and consumes these
users/sessions). Lives in `cafe/` (MIT), deployed as the igroteka worker
(`wrangler.toml` `main`), same-origin API under `/api/*`; D1 for storage.

- [x] Guest-first accounts: anonymous profile → OAuth link promotes in place
      (same user id survives the claim); orphan guests GC'd on logout/switch
- [x] OAuth sign-in: Google / GitHub / Discord (arctic), sessions = random
      token cookie + sha256 row in D1, sliding 30-day renewal, rotation on login
- [x] Cloud-save connections: Google Drive (`drive.appdata`, hidden app folder)
      + Dropbox (app folder). Worker stores AES-GCM-encrypted refresh tokens and
      mints short-lived access tokens; **file bytes go browser→cloud directly,
      never through Igroteka** (deviation note: for *saves* the refresh token
      does live server-side encrypted — the asset-wizard "tokens never touch
      our server" red line still holds for game assets)
- [x] XP-native UI: start-menu header = live identity; User Accounts window
      (sign-in, linking, rename, cloud connect/disconnect, log off)
- [ ] Provider consoles: create Google/GitHub/Discord/Dropbox apps, set
      secrets (`cafe/PROVIDERS.md` has the exact checklist) — **user-side**
- [ ] Save-sync layer: engine saves/replays → OPFS → user's cloud via
      `cafe.js` `cloudToken()` (Phase 2 tie-in)
- [ ] Rate limiting before lobby launch (guest INSERT is unauthenticated)

**Exit criterion:** sign in with a provider on two devices and see the same
profile; connect Drive/Dropbox and mint a working access token from the browser.

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

**Exit criterion (v1.0):** two browsers on different networks complete a 1v1 skirmish.
The café moment.

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
