**1. Runtime Path**

I would not assume RTW falls back to D3D9.

Public documentation I found confirms the shipped game requires DirectX 9.0b and a “Hardware Accelerated video card with Shader 1 support” on the current Steam Collection page, but that does not prove it uses the `IDirect3D9` API at runtime. DirectX runtime version and Direct3D interface version are separate facts: a D3D8 game can require the DX9 runtime.

Given your disassembly facts, the strongest evidence is local to the binary:

- `Direct3DCreate8` result is stored globally at `[0x2b1c3a4]`.
- NULL is fatal before renderer init completes.
- That global has 26 references clustered in renderer wrapper code.
- RTW imports both `Direct3DCreate8` and `Direct3DCreate9`, but the D3D8 object is not behaving like an optional one-shot probe.

So my diagnosis is: for this GOG `RomeTW.exe`, the default/early path is almost certainly a real D3D8 renderer path, or at minimum a D3D8 wrapper that must survive well beyond creation. I did not find a reliable public source for a `-dx9` flag or a `preferences.txt` key selecting D3D9 for original RTW. Treat the exact D3D8-vs-D3D9 selector as empirically unresolved, not publicly established.

Sources: Steam system requirements for RTW Collection list DirectX 9.0b and Shader 1 support; Microsoft COM/IUnknown and stdcall docs establish the ABI basics.  
https://store.steampowered.com/app/4760/Rome_Total_War__Collection/  
https://learn.microsoft.com/en-us/windows/win32/api/unknwn/nn-unknwn-iunknown  
https://learn.microsoft.com/en-us/cpp/cpp/stdcall

**2. Recommendation**

Do not spend time trying to “force” RTW into D3D9 unless you first observe a real D3D9 path being selected by the shipped binary.

Recommended option: **(b) implement a real `IDirect3DDevice8` frontend that bridges into your existing d9web-style backend/core.**

Justification: your current evidence says NULL D3D8 is fatal and the D3D8 object is referenced repeatedly. A fake D3D8 startup object risks failing at the next renderer call, or worse, passing caps and then selecting behavior you cannot draw. D3D8 and D3D9 fixed-function are close enough that a D3D8 frontend over the existing VB/IB/texture/render-state/software-raster path is lower risk than betting on an unproven D3D9 selector. Keep the bridge semantic, not source-derived from WineD3D: use headers/docs for ABI, Wine only as an oracle.

**3. D3D8 Vtables**

ABI: COM interfaces inherit `IUnknown`; Microsoft documents `IUnknown`’s three methods as first vtable entries. `__stdcall` means callee pops arguments; for your HLE trampolines, count stack dwords including `this`.

`IDirect3D8`: **16 methods total**

1. `0 QueryInterface`  
2. `1 AddRef`  
3. `2 Release`  
4. `3 RegisterSoftwareDevice`  
5. `4 GetAdapterCount`  
6. `5 GetAdapterIdentifier`  
7. `6 GetAdapterModeCount`  
8. `7 EnumAdapterModes`  
9. `8 GetAdapterDisplayMode`  
10. `9 CheckDeviceType`  
11. `10 CheckDeviceFormat`  
12. `11 CheckDeviceMultiSampleType`  
13. `12 CheckDepthStencilMatch`  
14. `13 GetDeviceCaps`  
15. `14 GetAdapterMonitor`  
16. `15 CreateDevice`

Your current `kD3D8Nargs = {3,1,1,2,1,4,2,4,3,6,7,6,6,4,2,7}` matches this layout.

`IDirect3DDevice8`: **97 methods total**

```text
0 QueryInterface
1 AddRef
2 Release
3 TestCooperativeLevel
4 GetAvailableTextureMem
5 ResourceManagerDiscardBytes
6 GetDirect3D
7 GetDeviceCaps
8 GetDisplayMode
9 GetCreationParameters
10 SetCursorProperties
11 SetCursorPosition
12 ShowCursor
13 CreateAdditionalSwapChain
14 Reset
15 Present
16 GetBackBuffer
17 GetRasterStatus
18 SetGammaRamp
19 GetGammaRamp
20 CreateTexture
21 CreateVolumeTexture
22 CreateCubeTexture
23 CreateVertexBuffer
24 CreateIndexBuffer
25 CreateRenderTarget
26 CreateDepthStencilSurface
27 CreateImageSurface
28 CopyRects
29 UpdateTexture
30 GetFrontBuffer
31 SetRenderTarget
32 GetRenderTarget
33 GetDepthStencilSurface
34 BeginScene
35 EndScene
36 Clear
37 SetTransform
38 GetTransform
39 MultiplyTransform
40 SetViewport
41 GetViewport
42 SetMaterial
43 GetMaterial
44 SetLight
45 GetLight
46 LightEnable
47 GetLightEnable
48 SetClipPlane
49 GetClipPlane
50 SetRenderState
51 GetRenderState
52 BeginStateBlock
53 EndStateBlock
54 ApplyStateBlock
55 CaptureStateBlock
56 DeleteStateBlock
57 CreateStateBlock
58 SetClipStatus
59 GetClipStatus
60 GetTexture
61 SetTexture
62 GetTextureStageState
63 SetTextureStageState
64 ValidateDevice
65 GetInfo
66 SetPaletteEntries
67 GetPaletteEntries
68 SetCurrentTexturePalette
69 GetCurrentTexturePalette
70 DrawPrimitive
71 DrawIndexedPrimitive
72 DrawPrimitiveUP
73 DrawIndexedPrimitiveUP
74 ProcessVertices
75 CreateVertexShader
76 SetVertexShader
77 GetVertexShader
78 DeleteVertexShader
79 SetVertexShaderConstant
80 GetVertexShaderConstant
81 GetVertexShaderDeclaration
82 GetVertexShaderFunction
83 SetStreamSource
84 GetStreamSource
85 SetIndices
86 GetIndices
87 CreatePixelShader
88 SetPixelShader
89 GetPixelShader
90 DeletePixelShader
91 SetPixelShaderConstant
92 GetPixelShaderConstant
93 GetPixelShaderFunction
94 DrawRectPatch
95 DrawTriPatch
96 DeletePatch
```

Header reference for layout/signatures:  
https://github.com/mingw-w64/mingw-w64/blob/master/mingw-w64-headers/include/d3d8.h

**4. Caps Gate / Minimal Persona**

I cannot confirm RTW’s exact caps gates from public docs. From your disassembly, the immediate known gate is non-NULL `Direct3DCreate8`; the next likely gates are `CheckDeviceType`, `CheckDeviceFormat`, `CheckDepthStencilMatch`, `GetDeviceCaps`, then `CreateDevice`.

Minimal D3D8 persona I would start with:

```text
DeviceType = D3DDEVTYPE_HAL
AdapterOrdinal = 0
DevCaps includes D3DDEVCAPS_HWRASTERIZATION
DevCaps includes D3DDEVCAPS_HWTRANSFORMANDLIGHT
MaxTextureWidth = 4096
MaxTextureHeight = 4096
MaxTextureRepeat = 8192
MaxTextureAspectRatio = 4096
MaxTextureBlendStages = 8
MaxSimultaneousTextures = 4
MaxActiveLights = 8
MaxUserClipPlanes = 0
MaxVertexBlendMatrices = 0
MaxPrimitiveCount = 0x000fffff
MaxVertexIndex = 0x00ffffff
MaxStreams = 8
MaxStreamStride = 255
VertexShaderVersion = D3DVS_VERSION(1,1)
PixelShaderVersion = D3DPS_VERSION(1,1)
MaxVertexShaderConst = 96
MaxPixelShaderValue = 1.0f
TextureCaps includes D3DPTEXTURECAPS_PERSPECTIVE
TextureCaps includes D3DPTEXTURECAPS_ALPHA
TextureCaps includes D3DPTEXTURECAPS_MIPMAP
TextureAddressCaps includes WRAP, MIRROR, CLAMP, BORDER
TextureFilterCaps includes POINT, LINEAR, MIPPOINT, MIPLINEAR
ZCmpCaps = all common compare ops
AlphaCmpCaps = all common compare ops
SrcBlendCaps/DestBlendCaps = ZERO, ONE, SRCALPHA, INVSRCALPHA, DESTALPHA, INVDESTALPHA, SRCCOLOR, INVSRCCOLOR, DESTCOLOR, INVDESTCOLOR, SRCALPHASAT
StencilCaps = 0 initially, unless RTW asks for stencil formats
PresentationIntervals includes IMMEDIATE and ONE
```

I would not advertise `VertexShaderVersion = 0` / `PixelShaderVersion = 0` as the first RTW persona, because Steam’s current requirements explicitly say “Shader 1 support.” If advertising shader 1.1 causes RTW to create shaders immediately, stub/log `CreateVertexShader` and `CreatePixelShader` first, then decide whether to translate SM1 or temporarily lie downward.

**Confidence And Open Questions**

Well-grounded: COM/IUnknown ordering, `__stdcall` stack behavior, `IDirect3D8` count/order, `IDirect3DDevice8` count/order, and the diagnosis that NULL D3D8 is not an acceptable startup behavior.

Inferred from your disassembly: RTW’s GOG build is materially wired to D3D8 during startup and likely rendering.

Needs empirical verification: exact D3D8-vs-D3D9 selector, whether any `-dx9`/config path exists in this binary, and the exact `D3DCAPS8` bits RTW gates on. The next best probe is logging every D3D8 method call and out-param before `CreateDevice`, then replaying with progressively filled caps.

Codex session ID: 019f4bf3-62d4-7360-8eb7-4e885b4690a0
Resume in Codex: codex resume 019f4bf3-62d4-7360-8eb7-4e885b4690a0
