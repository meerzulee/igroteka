# Igroteka — Release Notes

## 0.2.0 — Multiplayer & Party (2026-07-10)

Browser multiplayer for the C&C Generals: Zero Hour WASM port, end to end: gather
a party in an XP-desktop UI, launch straight into a networked match over WebRTC.
Engine at **build 31**.

### Multiplayer party
- **Party UI** (`/party`, and as a desktop program window in Igroteka): create or
  join rooms, gather, then launch every player into the game together.
- **Public & private games.** A password makes a **private** party (only people with
  the password get in; a leaked game link is useless without it). No password makes
  a **public** game, listed in a browsable directory anyone can join.
- **In-lobby**: live roster with player colours, chat, ready toggles, host **kick**
  (removes + bans the token — a kicked player must re-enter the password), and a
  soft ready-gate on Start (host is warned if someone isn't ready, never blocked).
- **8-player cap** surfaced with a clean "room is full" message.
- **Host leave/rejoin**: a host who drops rejoins as host via their stored token;
  role never silently migrates to another player.
- **Security** (Codex-reviewed): server-generated high-entropy room codes, PBKDF2
  password verifier + per-room HMAC secret, stateless signed join tokens (survive
  Durable-Object hibernation), per-IP auth rate-limiting, WS-gate enforcement.
- **Lifecycle**: rooms auto-clean their stored state after everyone leaves; dead or
  expired launch links **fail fast** with a native error instead of a hanging splash.

### Transport & netcode (engine)
- WebRTC DataChannel transport shimmed under the engine's `UDP` class; LAN discovery
  + lockstep both ride it. Auto-join autopilot drives host/guest straight into the
  match.
- **Fixed: spurious mid-match Disconnection Menu.** The lockstep barrier stalled on
  packet loss — resend was dial-up-tuned (2000 ms) against a 5000 ms disconnect
  window. Now a semi-reliable channel (`maxRetransmits: 5`) plus a 300 ms in-engine
  retry. (Remaining known cause: a backgrounded/occluded tab pauses its game loop —
  that stall is legitimate; tracked in PLAN.)
- **Fixed: "Duplicate name already in game."** The LAN join autopilot re-joined every
  ~45 ticks forever; now joins exactly once. Party names also de-duplicate on launch.

### Loading & presentation
- **Match loading screen.** The map load is one synchronous frame (WebGL can't
  present mid-load), so we show a game-style overlay in the one paint the engine
  leaves before the block: map name, player roster with factions/colours, animated
  spinner. Multiplayer launches keep the game's own splash (from the user's install
  assets) up through the whole menu walk and skip the 3D shell map.
- **Top-right HUD**: your name + live ping (real WebRTC RTT), room code, player
  count, per-player ping with host crown.
- **XP-native UI everywhere**: shared XP dialog component (error/warning/info),
  XP-style cursors, native taskbar buttons, the party page on the Igroteka desktop
  wallpaper with a game-picker banner and the Windows Tahoma UI font.
- Mobile: landscape enforced (orientation lock / rotate prompt).

### Fixes (engine, WASM/musl portability)
- **Tofu boxes** in the UI (Disconnection Menu vote counts, missing labels): two
  root causes fixed — `%hs` (an MSVC-only wide-printf specifier) mangled by musl at
  33 call sites, and `itoa` returning uninitialised stack garbage on libc++
  (`stringbuf::setbuf` is a no-op). Both fixed engine-wide.
- **Caching**: the game-glue import URL is now versioned so engine rebuilds actually
  reach browsers (a static URL had pinned users to an old build).

### Known / next
- **"a weq" wide inter-word spacing** in the FreeType text renderer — under
  investigation (engine build pending).
- **Mid-game rejoin** (close tab → rejoin a live match) — planned, gated on the
  determinism harness; see `PLAN.md`.
- **Deploy**: cafe worker (`cafe/`) needs `wrangler deploy` for the live site; the
  engine fork branch (`igroteka-wasm`) is pushed separately with the aweq fix.

---

## 0.1.0 — Initial

Single-player WASM port: shell map, main menu, skirmish, custom loading screen,
OPFS asset onboarding.
