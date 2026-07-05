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

## Immediate next step

Synchronous background-queue drain in `TextureLoader::Update()` under
`__EMSCRIPTEN__`, rebuild, reload — expect real menu art instead of magenta.
Then stage `MapsZH.big` for the shell map.
