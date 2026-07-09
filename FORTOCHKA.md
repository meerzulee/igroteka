# Fortochka — Win32 games in a browser tab, no source required

**форточка** — the small hinged pane in a Russian window; also Russian slang for
Microsoft Windows ("форточки"). A small window into Windows. Track 2 of Igroteka.

Status legend: `[ ]` not started · `[~]` in progress · `[x]` done
Update this file when an exit criterion is met. Last updated: 2026-07-10 (F0 started).

---

## Thesis

Track 1 (Dvijoke) answers: *what if the engine source exists?* Port the engine.

Track 2 (Fortochka) answers: *what if it doesn't?* **Port Windows instead.**

Most beloved 2000s PC games will never get a source release. Rome: Total War,
Age of Mythology, Battle for Middle-earth, Stronghold Crusader — sourceless
forever. The only honest way to run them in a browser is to run the shipped
`.exe`. Fortochka is a from-scratch Win32 runtime for WebAssembly: the game's
x86 machine code runs under our CPU emulator, and **every Windows API it
touches is our own native-wasm implementation**. No Wine port. No full-system
emulator. No streaming. The user brings their own DRM-free install; we bring
Windows.

**Flagship tenant: Rome: Total War (2004).** Chosen because it is the hardest
worthwhile target in the era we care about: D3D9, big single-threaded sim,
shipped middleware DLLs, registry use, CD-audio-era sound stack. If Fortochka
runs RTW, the catalog below it comes nearly free.

Constraint policy for this document: **we do not plan around what exists; we
plan around what is buildable.** Boxedwine, Hangover, v86, CheerpX are design
references and existence proofs, not dependencies and not ceilings. Nobody has
built high-level Win32 emulation on wasm. We are first. That is the point.

---

## Architecture

```
RomeTW.exe + install-dir DLLs (binkw32, …)      ← guest: emulated x86, untouched bytes
   │
   │  PE loader binds imports to host stubs; COM vtables point at host thunks
   ▼
════════════════ the boundary (thunks, both directions) ════════════════
   │
   ├─ zhelezo      x86 CPU: interpreter now, tiered x86→wasm JIT later
   ├─ k32web       kernel32/ntdll-lite: memory, files→OPFS, threads, TLS, SEH
   ├─ u32web       user32/gdi32: window, message pump, wndproc reverse thunks
   ├─ d9web        D3D9 → backend seam → WebGL2 | WebGPU   (d8web's sibling)
   ├─ dsweb        DirectSound → AudioWorklet ring buffer
   ├─ inweb        dinput8 / winmm input & timers
   └─ regweb       advapi32 registry → JSON document in OPFS
   │
   ▼
Browser: OPFS (game files) · WebGL2/WebGPU · WebAudio · Workers + SAB
```

### The three load-bearing ideas

**1. Identity-mapped memory.** Guest is 32-bit x86; wasm32 is 32-bit. Same
pointer width, same endianness. Guest virtual address = offset into linear
memory. No paging emulation, no marshalling: a `D3DMATRIX*` the game passes is
a pointer our native code dereferences directly; `VertexBuffer::Lock()` returns
a raw heap pointer the game memcpys into, and `Unlock()` feeds the same bytes
to `bufferSubData`. Structures cross the boundary by cast, not serialization.
We own the memory map: image base at 0x400000, guest stack and heaps carved
from a reserved guest arena, partitioned from the Emscripten runtime heap.
Cost: no page protection (wasm has none) — guard pages and PAGE_NOACCESS
become no-ops. Games of this era do not care.

**2. HLE only at the OS boundary.** Everything inside the install directory is
guest code and runs emulated as-is — `binkw32.dll` decodes its own videos,
bundled middleware runs itself. We implement **only what Windows provides**,
and not "Windows" — the game's import table. A 2004 game imports ~10 DLLs,
300–600 functions, half of them trivial. The import dump *is* the scope of the
project. Everything else is a logged stub until a game proves it needs it.

**3. All intelligence above the seam** — the d8web rule, now project law.
zhelezo doesn't know about DLLs; DLL modules don't know about x86; d9web's
backends stay dumb snapshot-translators exactly as in Track 1. Each module is
independently testable against a corpus binary. This is what makes the
elephant chunkable.

---

## Components

| Component | What it is | License | Hard part |
|---|---|---|---|
| `zhelezo` (железо, "the iron") | x86 CPU core: interpreter tier + JIT tier | MIT | The JIT is a compiler project — see Phase F6 |
| `peload` | PE/COFF loader: sections, relocs, imports→stubs, TLS callbacks, resources | MIT | Import binding to host thunk table |
| `k32web` | kernel32 + the ntdll slice games actually hit | MIT | SEH (`fs:[0]` chains), VirtualAlloc semantics, green threads |
| `u32web` | user32/gdi32-enough: one window, message pump, cursors | MIT | **Reverse thunks** — DispatchMessage re-enters guest code at wndproc |
| `d9web` | D3D9 frontend + state tracker + shader translation, over the **existing d8web backend seam** | MIT | SM1/SM2 bytecode→GLSL ES compiler; D3DCAPS9 forgery |
| `dsweb` | DirectSound: buffers, streaming, notify positions → AudioWorklet | MIT | Latency + the notify-event timing games rely on |
| `inweb` | dinput8 + winmm timers | MIT | Pointer-lock feel; `timeGetTime` monotonicity |
| `regweb` | Registry over a JSON doc in OPFS, pre-seeded per game | MIT | Nothing — deliberately dumb |
| `cartridges/` | Per-game manifest: exe path, caps persona, quirks, asset manifest for the onboarding wizard | — | Compat knowledge, accumulated |

Whole runtime is MIT, written from scratch. Oracles (read, never copy): WineD3D
for D3D9 semantics (LGPL), Wine for API behavioral edge cases (LGPL), Boxedwine
for CPU-core design ideas (GPL2), Proton for game-specific quirk knowledge
(mixed/LGPL — quirk *facts* feed cartridges, code never), ReactOS (GPL2) for
NT internals Wine doesn't model — TEB/PEB, ntdll SEH dispatch/RtlUnwind, ldr
sequencing, registry semantics — plus its apitests: both projects' conformance
suites document exact Windows behavior; we reimplement the *assertions* as our
own corpus tests. Struct layouts come from mingw-w64 headers (permissive).
Intel SDM for the ISA. Same discipline as Track 1's WineD3D rule.

**Permissive tier (verified 2026-07-10, may lift with attribution):**
FEX-Emu (MIT) — the x86→ARM64 translator under Valve's arm64 Proton; decoder
tables, deferred-eflags trick, JIT block cache, x87-reduced-precision option,
thunk-lib HLE pattern. Box64 (MIT) — dynarec + library-thunking precedent for
our import-table boundary. v86 (BSD-2) — the wasm-module-per-block JIT
mechanic zhelezo tier 1 uses. Any lifted code keeps its copyright notice and
is marked at the lift site.

---

## d9web: what carries from d8web, what's new

Carries (~70%): the backend seam and both backends' futures, texture/VB/IB
resource management, DXT decode, present/swapchain, the state-hash→program
cache machinery, the FFP GLSL synthesizer (D3D9 keeps the full FFP —
`SetTextureStageState` pipeline intact).

New: sampler state split out of texture stages (`SetSamplerState`), vertex
declarations as objects, and the real work — a **shader bytecode translator**:
`vs_1_1`/`vs_2_0`, `ps_1_1`–`ps_1_4`/`ps_2_0` token streams → GLSL ES 3.00.
ps_1_x is the nasty dialect: clamped fixed-point ranges, `texbem`/`texm3x3*`
address ops, co-issue. Bounded nasty — the instruction set is small and fully
documented; the translator slots in beside the FFP synthesizer behind the same
state-hash cache, WGSL emitter beside it later, nothing else moves.

**Caps forgery is a first-boot gate.** Games read `D3DCAPS9` at startup and
choose their renderer path from it. We impersonate one coherent real 2004 GPU
(Radeon 9700 persona: SM2.0, 8 texture stages, DXT, the exact caps-bit set the
real driver reported) rather than a franken-caps set. Persona lives in the
cartridge, overridable per game.

Open question the recon phase answers: RTW's floor is GeForce3/Radeon 8500
(first shader parts), but era engines almost always shipped FFP fallbacks and
era forums show RTW on DX7-class cards. If forcing `MaxShaderModelPS=0` under
desktop Wine still renders, the d8web synthesizer covers RTW's baseline and
the bytecode translator demotes to an effects milestone. One registry key
resolves the single biggest unknown in d9web. Do it in week 1.

---

## zhelezo: the CPU plan

**Tier 0 — interpreter.** Boring, correct, fully instrumentable: every memory
access checked, `fs:` segment modeled (SEH needs it), x87 on f64 with a
determinism warning flag, precise faults. Perf target ≥ 50 emulated MIPS in
wasm — enough for menus, 2D, turn-based campaign logic. This tier is also the
permanent debugger: single-step, breakpoints, call-trace of the boundary.

**Tier 1 — hot-block JIT.** Profile in tier 0; compile hot basic blocks /
superblocks to fresh wasm modules on the fly (`WebAssembly.Module` codegen in
a worker, side-exit chains back to the interpreter, code-cache with eviction).
Browsers compile small modules in microseconds; v86 proves the mechanic. Target
≤ 5× native — comfortably real-time for a game whose min spec is a 1 GHz P3.

**Tier 2 (research, optional) — install-time AOT.** Static-recompile the whole
exe to wasm **in the user's browser at import time**, per user, output cached
in OPFS, never distributed. Kills JIT warmup entirely. Legally interesting
(transient private transformation, like an emulator's JIT cache, but worth a
real legal read before shipping). Not on the critical path; noted because we
are not planning around limits.

SEH under the JIT is the known trap: stack-based `fs:[0]` handler chains must
keep working when frames belong to compiled code. Design the JIT's frame
layout around this from day one; do not bolt it on.

---

## The dev loop: corpus before game

We never debug against RTW first — it's a 15-million-instruction black box.
We build a **conformance corpus**: tiny 32-bit PEs we compile ourselves with
era tooling (old DirectX SDK samples and hand-written probes), each exercising
one seam:

`hello.exe` (console out) → `window.exe` (pump + wndproc round-trip) →
`triangle.exe` (d9web clear/draw/present) → `dynvb.exe` (Lock/Unlock churn) →
`shader.exe` (ps_1_1 and ps_2_0 paths) → `sound.exe` (streaming + notify) →
`seh.exe` (throw across frames) → `thread.exe` (CreateThread + green
scheduler) → `bink.exe` (guest-DLL loading, calls into binkw32).

Every corpus binary is a demo-able rung and becomes a CI test forever. RTW
enters only when the corpus is green. Second game enters only against the same
corpus — that's how compat knowledge compounds instead of resetting.

---

## Execution plan

### Phase F0 — Recon (target: 1 week, gates everything)

- [ ] Obtain RTW; verify a **lawful DRM-free x86 PE** exists (retail is
      SafeDisc; check the Steam/GOG-era exe's DRM status — PCGamingWiki row +
      section-table inspection). **No DRM circumvention, ever — hard rule.**
      If no clean exe exists, RTW demotes to design-reference and another
      flagship is chosen (candidates: Stronghold Crusader, AoM).
- [ ] Import-table dump (`pefile`) of RTW.exe + every shipped DLL → the
      canonical scope list, ranked trivial / mechanical / hard
- [ ] CRT linkage check (static CRT = smaller surface) + which D3D9 entry
      points it actually imports
- [ ] Shader-path experiment: RTW under desktop Wine, `MaxShaderModelPS=0` —
      decides d9web's critical path (see above)
- [ ] Boundary census from a native run: `WINEDEBUG=+d3d9` call-frequency
      trace of menu vs campaign vs battle — the perf budget's raw data
- [x] Corpus toolchain: MinGW-w64 (`i686-w64-mingw32-gcc`, no container needed)
      producing minimal-import 32-bit PEs — `fortochka/corpus/`; `hello.exe`
      is 6 KB, imports exactly 3 kernel32 functions. F0 recon worksheet with
      Linux commands: `fortochka/RECON.md`

**Exit criterion:** a one-page RECON.md with the import table ranked, DRM
verdict, shader verdict, and call-frequency table. Every estimate below gets
re-based on it.

### Phase F1 — Iron (target: 4–6 weeks)

- [~] `zhelezo` tier 0: IA-32 user-mode interpreter (int + x87 + the SSE1 RTW
      uses), `fs:` segment, precise faults, single-step debugger
      — integer core LIVE (`fortochka/zhelezo/`): ~150 opcodes, eager flags,
      precise faults, hostcall seam, 14/14 asm corpus tests green under
      ASan/UBSan, 86 MIPS native unoptimized (budget: 50). Missing: x87, SSE1,
      decode cache. Corpus driver: `tests/run.py` (MinGW as → flat bin → assert)
- [~] `peload`: map sections, apply relocs, bind imports to host thunk table,
      run TLS callbacks — LIVE (`fortochka/peload/`): PE32 map at preferred
      base, HIGHLOW relocs, IAT→hostcall-slot binding, TLS callbacks surfaced
      (not yet invoked)
- [~] `k32web` minimum: identity-mapped guest arena, VirtualAlloc/heaps, TLS,
      GetModuleHandle/GetProcAddress (host-stub aware), files→OPFS (reuse
      Track 1 VFS), stdout — STARTED (`fortochka/k32web/`): stdcall dispatch,
      GetStdHandle/WriteFile/ExitProcess; grows stub-log driven
- [ ] Boundary thunk generator: stdcall/cdecl arg lifting from guest stack →
      native call → EAX/EDX:EAX return, generated from an IDL-ish table, both
      directions (hand-rolled per-import for now; generator when count grows)

**Exit criterion:** `hello.exe` — a real PE we compiled — prints via emulated
`printf` → `WriteConsole` in a browser tab.
**2026-07-10: native half MET** — `zhrun corpus/bin/hello.exe` prints through
the full peload→zhelezo→hostcall→k32web pipeline, exit 0, output identical to
the Wine oracle. Remaining for full F1: Emscripten build + browser shell.

### Phase F2 — Window (target: 3–4 weeks)

- [ ] `u32web`: RegisterClass/CreateWindow (one canvas-backed window),
      PeekMessage/GetMessage/DispatchMessage, **reverse thunk**: re-enter
      interpreter at wndproc EIP with args on the guest stack
- [ ] Message-pump ↔ rAF integration: frame pacing, spin-loop detection
      (PeekMessage-spin games burn 100% CPU — detect and yield)
- [ ] `inweb`: mouse/keyboard events → messages + dinput8 state; pointer lock
- [ ] SEH: `fs:[0]` chain walk, guest handler invocation, unwind — `seh.exe`
      corpus binary passes

**Exit criterion:** `window.exe` opens a window, paints on WM_PAINT, responds
to input — full round trip guest→host→guest.

### Phase F3 — Pixels (target: 6–10 weeks, parallel with F2 after F1)

- [ ] `d9web` frontend on the d8web seam: device, swapchain, resources,
      state tracker, sampler split, vertex declarations
- [ ] Caps persona (Radeon 9700), CheckDeviceFormat table
- [ ] FFP path: port/extend the d8web synthesizer to D3D9 stage semantics
- [ ] Shader translator: vs_1_1 → GLSL ES first, then ps_1_1–ps_1_4, then
      SM2.0 — each tier is a corpus binary
- [ ] Draw-submission budget enforced by the existing state-hash dedup;
      instrumentation counts crossings and µs/draw from day one
- [ ] `dsweb`: static + streaming buffers, notify positions → AudioWorklet

**Exit criterion:** entire graphics+sound corpus green, including `shader.exe`
SM2 water-like effect, at 60fps.

### Phase F4 — RTW boots (target: 4–8 weeks of pure compat grinding)

- [ ] `regweb` seeded from a real RTW install's registry footprint
- [ ] Guest-DLL loading: binkw32 et al. load and run emulated; video plays
      (slow is fine — cutscenes are skippable)
- [ ] Stub-log-driven development: boot, hit unimplemented import, implement,
      repeat — the import table from F0 says exactly how long this tunnel is
- [ ] Menu renders, options work, campaign map loads

**Exit criterion (the screenshot):** RTW main menu, then the campaign map,
interactive in a browser tab. First public demo of Track 2.

### Phase F5 — Campaign playable (target: 4–6 weeks)

- [ ] Interpreter perf pass: dispatch tightening, memory fast paths, boundary
      batching — campaign is turn-based and event-driven, tier 0 must carry it
- [ ] Save/load → OPFS; settings; the onboarding wizard grows an RTW cartridge
      (asset manifest for the `.pak`/`.idx`/`.cas` set, validation, import doors
      reused from Track 1's asset plan)
- [ ] Battles explicitly deferred: auto-resolve only, honestly labeled

**Exit criterion:** a stranger with a lawful RTW install plays a campaign
turn-cycle (move armies, fight auto-resolved battles, manage cities, end turn)
in the browser, start to save-game.

### Phase F6 — zhelezo-jit: the battles (target: research-grade, 3–6 months)

- [ ] Tier 1 JIT as designed above; SEH-aware frames; x87 strategy decided by
      measurement (f64 vs software 80-bit on flagged hot paths)
- [ ] Battle perf ladder: 400-man skirmish → 1k → 3k+ siege
- [ ] If tier 1 lands ≤5× native, RTW battles run at min-spec-or-better speed
      on any modern laptop

**Exit criterion:** a real-time battle, thousands of units, ≥30fps sustained.
The trailer moment for the whole platform.

### Phase F7 — The catalog (ongoing)

- [ ] Second tenant through the same corpus (Stronghold Crusader or AoM —
      pick the one whose F0 recon is cleanest) — proves generality
- [ ] Cartridge format hardened: caps persona, quirk flags, asset manifests,
      per-game compat notes — the emulator-scene compat-DB model
- [ ] d9web WebGPU backend rides Track 1's WGSL emitter work
- [ ] Multiplayer research: era GameSpy is dead anyway; a `wsock32` shim over
      Track 1's WebRTC transport is the moonshot — deterministic-lockstep
      games only, RTW is not one, park it
- [ ] Fortochka standalone release: MIT runtime any project can embed —
      the d8web strategy at platform scale

---

## Performance budgets (checked against F0 census, revised then)

| Path | Budget | Basis |
|---|---|---|
| zhelezo tier 0 | ≥ 50 MIPS | menu/campaign logic headroom over era CPUs' idle loops |
| zhelezo tier 1 | ≤ 5× native slowdown | 1 GHz P3 min-spec × modern single-thread ≈ real-time with margin |
| Boundary crossing | ≤ 300 ns amortized | thunk = arg lift + call; no serialization thanks to identity mapping |
| Draw submission | ≤ 15 ms/frame @ 3k draws | WebGL call ≈ 1–5 µs; state-hash dedup + batching; WebGPU bundles later |
| Memory | fits wasm32 4 GB | 32-bit game working set < 1 GB; GPU assets live outside linear memory; memory64 avoided (bounds-check tax) |

## Standing risks

| Risk | Mitigation |
|---|---|
| JIT is a genuine compiler project | It's phase-gated: campaign ships on the interpreter; battles are the JIT's demo, not its hostage. v86's mechanic is the existence proof |
| No lawful DRM-free RTW exe | F0 gate #1, checked before any code; flagship swaps to another 2004 title, runtime plan unchanged |
| x87 80-bit vs f64 divergence breaks game logic | Interpreter flags precision-sensitive patterns; software-x87 fallback on flagged regions; single-player = no desync partner |
| SEH × JIT interaction | Designed into tier-1 frame layout from day one; `seh.exe` corpus binary is a permanent regression test |
| Caps forgery misses a bit and RTW picks a broken path | Persona copied from a real Radeon 9700 driver dump, not hand-assembled; cartridge override for quirks |
| Reverse-thunk edge cases (nested SendMessage, reentrancy) | Corpus binaries model each pattern before RTW does; interpreter's full observability makes these debuggable |
| Import table reveals a monster (DirectPlay, weird COM) | F0 exists precisely to find it in week 1, not month 6 |
| Solo-scale project | Every phase exits on a demo; corpus rungs are contributor-sized; runtime is MIT to recruit beyond the GPL-averse |

## Legal posture

- **Clean-room HLE from documentation** — Wine's own legal foundation,
  reinforced by *Google v. Oracle* (API reimplementation). We ship zero
  Microsoft, SEGA, or Creative Assembly bytes.
- **User-supplied assets only**, OPFS, never hosted — Track 1's red lines
  apply verbatim (no asset hosting, no credentials, no trademark use in
  naming or marketing; "runs Rome: Total War" is nominative fair use,
  the product name is Fortochka).
- **No DRM circumvention** — DMCA §1201 is a separate offense with no
  fair-use defense. SafeDisc exes are untouchable; Steam-wrapped exes that
  demand a live client are out (steam_api emulation is §1201-gray — banned
  here). DRM-free exe or the game doesn't onboard. This rule is load-bearing;
  it is what keeps the entire project defensible.
- **License hygiene**: Fortochka is MIT, written from scratch. Wine/WineD3D/
  Boxedwine are read-only oracles — same standing rule as d8web's WineD3D
  discipline. Any GPL-derived experiment lives in a separate repo, never in
  the runtime.
- Not affiliated with or endorsed by Microsoft, SEGA, or Creative Assembly.

## Relationship to Track 1

Shared and load-bearing: the d8web backend seam and both its backends, the
FFP synthesizer lineage, the state-hash cache design, OPFS VFS + the asset
onboarding wizard and its legal framework, the site/catalog, and eventually
the WebRTC transport. Track 1 taught us the discipline (dumb backends, demo-able
milestones, license walls); Track 2 applies it to a target nobody has touched.

Dvijoke is the engine we were given. Fortochka is the window we cut ourselves.
