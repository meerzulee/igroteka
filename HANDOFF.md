# Igroteka — Session Handoff (2026-07-06, session 2)

State of the WASM port and exactly where to resume.

## Where we are

The C&C Generals Zero Hour engine **runs in a browser tab and renders its main
menu with working text** through the d8web D3D8→WebGL2 layer:

- Full engine builds via the `wasm` CMake preset. Output:
  `zh-web/build/wasm/GeneralsMD/GeneralsXZH.{js,wasm}`.
- Boots, loads `.big` archives, inits every subsystem, reaches MainMenu.wnd.
- **Renders**: GENERALS ZERO:HOUR logo, all six menu buttons with labels
  (SOLO PLAY / MULTIPLAYER / LOAD / OPTIONS / CREDITS / EXIT GAME), version
  string, FPS counter (~30 fps).

## Fixed this session

1. **Black canvas (was the big open bug)** — everything was back-face culled.
   The D3DCULL→glFrontFace mapping had been calibrated against the cube demo,
   whose indices were wound CCW-visual (GL habit) while the comment claimed CW.
   Engine truth (menu draws use `D3DCULL_CW`, non-RHW path): **D3DCULL_CW must
   keep GL-CCW front faces**; XYZRHW path takes the opposite (shader y-flip
   mirrors winding). Fixed in `d8web/src/backends/webgl2/gl_backend.cpp`
   (`applyFixedState` now takes the ShaderKey). Cube demo rewound to D3D
   convention. Commit `e80cbea`.
2. **No text** — two independent causes, both needed:
   - No font file: the fontconfig stub resolves every query to
     `/fonts/default.ttf`, but nothing was staged there → `FT_New_Face` failed
     silently. boot.html now fetches `gamedata/default.ttf` (serve any sans
     TTF there; Arial.ttf from macOS works, not committed) into `/fonts/`.
   - `CopyRects` was a stub in d8web. The engine rasterizes glyphs into an
     A4R4G4B4 image surface and transfers it into sentence textures via
     `_Copy_DX8_Rects` → sentence textures stayed transparent. Implemented as
     a same-format CPU blit + texture-level re-upload. Commits `fa9ac0f`
     (d8web), `2bd18fb` (zh-web; also tracks boot.html under `wasm/`).
   - Diagnostics: `[FONT]` log lines (guarded `__EMSCRIPTEN__`) in
     `render2dsentence.cpp` show font resolution in the boot log.

## The one remaining visual bug: placeholder-magenta textures

The whole menu background is W3D missing-texture magenta (`FF00FF7F`), and
button/window art besides the logo is absent. The logo texture loads, so the
texture pipeline works — most textures never get their real bits.

**Top suspect (investigated, not yet fixed):** W3D's texture loading is
asynchronous. `LoaderThreadClass::Thread_Function`
(`Core/Libraries/Source/WWVegas/WW3D2/textureloader.cpp:1009`) pops
`_BackgroundQueue` and loads mips on a background thread. On wasm (no
threads compiled in), that thread never runs, so every texture that goes
through the background path stays at its thumbnail/missing placeholder
forever. `TextureLoader::Update()` (line ~870) is called per-frame on the
main thread and already processes the foreground queue.

**Suggested fix:** on `__EMSCRIPTEN__`, drain `_BackgroundQueue` synchronously
inside `TextureLoader::Update()` — pop task, `task->Load()`, push to
`_ForegroundQueue` — i.e. inline exactly what `Thread_Function` does, before
the normal foreground processing. Keep it guarded so native builds are
unchanged.

Also: the menu 3D shell map needs `MapsZH.big` (not currently staged in
boot.html) — add it to the staging list once textures load, or the background
will stay a flat plane.

Second look later: `[ASSET_FAIL] shaders/*.vso/.pso` — fillCaps() advertises
programmable shaders, so terrain probes for .pso files. Harmless now; consider
capping `VertexShaderVersion`/`PixelShaderVersion` to 0 in the bridge.

## How to build + run

```bash
cd ~/Work/experiments/igroteka/zh-web
emcmake cmake --preset wasm            # configure (once)
cmake --build build/wasm --target z_generals    # ~2 min incremental
cd build/wasm/GeneralsMD && python3 -m http.server 8932
# gamedata/: symlinks to ~/GeneralsX/GeneralsZH/*.big  +  default.ttf (any sans TTF)
# boot.html source of truth: zh-web/wasm/boot.html (copy into the serve dir)
```

Open `http://localhost:8932/boot.html` in a **headed** browser (headless has no
WebGL2). gstack `/browse --headed` — pass `--headed` on every command. The
browse daemon occasionally restarts to a welcome tab; `goto` back to boot.html.

## Key files

- `d8web/src/backends/webgl2/gl_backend.cpp` — cull mapping (applyFixedState),
  debug counters, `D8WEB_DEBUG_CLEAR`/`D8WEB_DEBUG_NOCULL` probe hooks
- `d8web/src/frontend/device.cpp` — CopyRects, format conversion, resources
- `zh-web/wasm/boot.html` — test harness (staging list, log panel)
- `zh-web/wasm/d8web_bridge/d8web_bridge.cpp` — COM adapter over d8web
- `zh-web/Core/Libraries/Source/WWVegas/WW3D2/textureloader.cpp` — async
  texture loading (the remaining bug lives here)
- `zh-web/Core/Libraries/Source/WWVegas/WW3D2/render2dsentence.cpp` — FreeType
  text pipeline + [FONT] diagnostics

## Commits

- `d8web` (igroteka repo, master): `e80cbea` cull fix → `fa9ac0f` CopyRects
- `zh-web` (fork, branch `igroteka-wasm`): `2bd18fb` fonts + boot.html tracked

## Session 2 late update — shell map fully renders

Magenta is fixed. Root cause was layered:
1. ZH is an expansion: base-game archives hold much of its art. boot.html now
   sets `CNC_GENERALS_PATH=/game-base` and stages base Textures/W3D/Terrain/
   Window .bigs (plus TerrainZH + MapsZH for the shell map). ZH mounts first
   and wins conflicts (zh-web commit d8897f8).
2. No loader thread on wasm: background texture queue drained synchronously
   in TextureLoader::Update() under __EMSCRIPTEN__ (same commit).
3. d8web: DXT2/DXT4 aliased to DXT3/DXT5 (309 shell-map textures, drew white)
   — commit 1573648.
4. d8web: GL_TEXTURE_MAX_LEVEL clamped to actually-uploaded mip levels
   (unuploaded levels sampled black → big black terrain triangles);
   L8/A8L8 CPU-expanded to RGBA8 (no texture swizzle in WebGL2);
   texture-coordinate transforms + TCI texgen implemented (cloud-shadow
   layer sampled garbage → tile-aligned dark squares) — commit ad47644.

The main menu now renders the full animated battle with correct terrain,
vegetation, vehicles, effects, text.

## Remaining polish items

- Hard black triangular wedges on some cliff faces. Suspect W3D projected /
  volume shadows: the engine may be drawing shadow geometry that needs
  stencil-buffer setup we don't provide (check D3DRS_STENCIL* handling and
  the W3D shadow render path). Could also be legit baked cliff shadows
  exaggerated by a blend mode gap — compare against native GeneralsX.
- Engine probes `UISABOTR_SKN..w3d`, then keeps appending `.w3d` (harmless
  log spam, pre-existing name-mangling loop; also probes trstrtholecvr.tga
  which this install genuinely lacks).
- Occasional white particle rectangle near explosions (one texture/blend
  combo still off).
- fillCaps advertises programmable shaders so the engine probes .vso/.pso —
  consider zeroing VertexShaderVersion/PixelShaderVersion in the bridge.


## Session 2 final update — THE BLACK TERRAIN BUG IS FIXED

Root cause (one bug, many symptoms): a D3D8 ownership-contract violation.
GetSurfaceLevel must return TEXTURE-OWNED surfaces that survive a caller's
Release(). Both d8web's Texture8 AND the COM bridge's BridgeTexture created a
fresh object per call; the engine's D3DXFilterTexture (CompatLib) releases
each level surface and keeps the pointer as the next mip's source. The freed
wrapper's heap slot was recycled as the next level's wrapper, the mip filter
degenerated into same-surface self-copies, and every mip past level 1 stayed
zero-filled. Distant terrain (which samples higher mips) rendered black; the
scrolling cloud stage sampled black cloud mips and darkened everything.

Fixes: cache level surfaces on the texture in BOTH layers (d8web commit
3156a1b, zh-web commit 48751c5). Cloud stage and terrain multi-pass blending
restored — their earlier 'darkening' was this same bug.

Debug tooling built along the way (all still in tree):
- boot.html?args=... (engine CLI args), ?shadows=on|off, wasm cache-buster
- [ATLAS_BANDS]/[ATLAS_MIP] atlas content histograms; /atlas_*.raw dump
  drawable on a canvas via FS.readFile (the 'terrain inspector')
- [FILTER_PASS]/[LSFS] mip-filter tracing, [TILE_FAIL]/[TILE_SHORT]
- Native ground-truth capture: screencapture -l <windowID> (find ID via
  Swift CGWindowListCopyWindowInfo)

Remaining (minor): stencil support for shadow volumes (shadows=off default),
white particle rect near explosions, UISABOTR_SKN name-mangling log spam.
Next big test: skirmish with AI (SkirmishScripts.scb now staged, clock fixed).
