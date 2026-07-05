# Igroteka — Session Handoff (2026-07-06)

State of the WASM port at the end of the last session, and exactly where to resume.

## Where we are

The C&C Generals Zero Hour engine **compiles to WebAssembly, boots in a browser
tab, and runs end-to-end** through the d8web D3D8→WebGL2 translation layer:

- Full engine builds via the `wasm` CMake preset (no vcpkg). Output:
  `zh-web/build/wasm/GeneralsMD/GeneralsXZH.{js,wasm}` (~9 MB wasm).
- Boots, loads `.big` archives from the browser FS, parses all INI + CSF
  (6422 strings), inits every subsystem, loads `Menus/MainMenu.wnd`.
- Drives **13,500+ D3D8 draw calls per frame** through the d8web bridge to
  WebGL2, staying responsive via an `emscripten_set_main_loop` RAF loop.

## The one open bug: nothing renders (canvas black)

**Confirmed by a magenta-clear probe:** d8web owns the visible canvas — a forced
`glClearColor(1,0,1,1)` paints the whole canvas magenta. So the pipeline reaches
the screen. But all 13,500 draws execute (no GL errors) and produce **zero
visible fragments** on top of the clear.

That isolates the bug to per-draw geometry/state, not plumbing. Ranked suspects:

1. **Global cull winding (most likely).** The cube demo needed
   `glFrontFace(GL_CCW)` for `D3DCULL_CCW`, hand-tuned to the cube's winding. The
   engine's real geometry may wind the opposite way, so everything back-face
   culls → invisible. Test: force `glDisable(GL_CULL_FACE)` unconditionally in
   `applyFixedState` and re-screenshot. If content appears, the winding
   convention is inverted — figure out the correct mapping against real engine
   geometry (W3D meshes), not the demo cube.
2. **XYZRHW UI transform.** The shell/menu draws are pre-transformed. If
   `uViewportSize` is 0 at draw time, the NDC divide is NaN and quads vanish.
   Verify `m_viewport.Width/Height` are non-zero when UI draws run (they log
   `vp=1024x768` on clear, but confirm at draw).
3. **Depth test.** If ZENABLE is on and the depth buffer isn't cleared per frame,
   everything can z-reject. Check the engine's per-frame Clear includes
   `D3DCLEAR_ZBUFFER`.

Diagnostic harness already in place (behind `-DD8WEB_DEBUG_CLEAR` and the
`m_drawCount/m_clearCount/m_texUploads` counters in `gl_backend.cpp`). Add a
per-draw dump of cull mode + depth func + first transformed vertex position to
see whether geometry lands in NDC range.

## Second issue (independent): textures load as placeholder magenta

`texUpload` logs show `first=FF00FF7F` (magenta placeholder) even after staging
`TexturesZH.big`/`W3DZH.big`. The engine's texture loader isn't finding the real
art in the archives — likely a VFS path/case mismatch or the loader looking in a
`.big` we didn't mount. Fix after the render bug (invisible geometry masks it
anyway). Also seen: `[ASSET_FAIL] shaders/Trees.vso` — the engine probes for
`.vso`/`.pso` shader files; harmless (we report no programmable shaders), but the
asset-path normalization is worth a look.

## How to build + run

```bash
cd ~/Work/experiments/igroteka/zh-web
emcmake cmake --preset wasm
cmake --build build/wasm --target z_generals    # ~2 min incremental
# serve (already may be running on 8932):
cd build/wasm/GeneralsMD && python3 -m http.server 8932
# game .big files symlinked into build/wasm/GeneralsMD/gamedata/ from ~/GeneralsX/GeneralsZH/
```

Open `http://localhost:8932/boot.html` in a **headed** browser (headless Chromium
has no WebGL2). Use gstack `/browse --headed` — every command needs the `--headed`
flag or it spawns a second daemon. boot.html stages the `.big` files, shows a live
autoscrolling log, and renders to a 1024×768 `#canvas`.

## Key files (all changes guarded `#ifdef __EMSCRIPTEN__`, no behavior change off wasm)

- `zh-web/cmake/wasm-deps.cmake` — the whole wasm dependency + link strategy
- `zh-web/cmake/{sdl3,dx8,gamespy,config-build}.cmake` — per-dep Emscripten branches
- `zh-web/wasm/d8web_bridge/d8web_bridge.cpp` — COM adapter: DXVK d3d8 vtables → d8web
- `zh-web/wasm/{fontconfig_stub,wasm_compat.h}` — libc/font gap fills
- `Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp` — static bridge factory
- `GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp` — RAF main loop
- `GeneralsMD/Code/Main/SDL3Main.cpp` — no-Vulkan window path
- `d8web/src/backends/webgl2/gl_backend.cpp` — the WebGL2 backend (+ debug harness)

## Commits

- `d8web` (igroteka repo, master): through `f76c8ab` — bridge readiness
- `zh-web` (fork, branch `igroteka-wasm`): through `4fe8edc` — boots to main menu

## Immediate next step

Disable culling unconditionally, rebuild, screenshot. That one test most likely
turns the black canvas into the actual main menu.
