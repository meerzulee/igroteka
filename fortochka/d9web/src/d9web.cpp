#include "d9web/d9web.h"

#include <cstdio>
#include <string>
#include <vector>

namespace d9web {

using runtime::Machine;
using runtime::MachineError;

namespace {

// D3DCOLOR (0xAARRGGBB) → opaque framebuffer ARGB (0xFFRRGGBB).
inline uint32_t d3dcolor_to_argb(uint32_t c) { return 0xFF000000u | (c & 0x00FFFFFF); }

// Per-object state layout: dword at obj+4 is the IUnknown refcount.
constexpr uint32_t OBJ_STATE_BYTES = 8;
constexpr uint32_t OBJ_REFCOUNT = 4;

// One guest process per Machine in tier 0, so a global is fine here (matches
// u32web). Vtables are created lazily on first Direct3DCreate9 — NOT in
// install(), because run_entry() resets the guest heap after install().
struct State {
    uint32_t width = 640, height = 480;
    std::vector<uint32_t> backbuffer; // ARGB 0xFFRRGGBB, width*height
    bool presented = false;
    uint32_t d3d9_vtbl = 0, device_vtbl = 0; // 0 until first use
    void ensure_bb() {
        if (backbuffer.size() != (size_t)width * height)
            backbuffer.assign((size_t)width * height, 0xFF000000u);
    }
};
State g_state;

// A vtable slot: the API name (for diagnostics + as the scope document),
// the stdcall dword count including `this` (so ret() pops correctly), and the
// handler (null → unimplemented, throws named).
struct Method {
    const char* name;
    uint8_t nargs;
    void (*fn)(Machine&, const Method&);
};

// --- generic IUnknown (shared by every interface) ---
void ret_ok(Machine& m, const Method& mm) { m.ret(mm.nargs, 0); } // S_OK no-op
void com_addref(Machine& m, const Method& mm) {
    uint32_t obj = m.arg(0), rc = m.read32(obj + OBJ_REFCOUNT) + 1;
    m.write32(obj + OBJ_REFCOUNT, rc);
    m.ret(mm.nargs, rc);
}
void com_release(Machine& m, const Method& mm) {
    uint32_t obj = m.arg(0), rc = m.read32(obj + OBJ_REFCOUNT);
    if (rc) rc--;
    m.write32(obj + OBJ_REFCOUNT, rc); // never freed: HLE objects are immortal
    m.ret(mm.nargs, rc);
}
void com_query_interface(Machine& m, const Method& mm) {
    // QueryInterface(this, REFIID, void** ppv). Tier-0 simplification: hand back
    // the same object for any IID (games mostly re-query the interface they
    // hold). AddRef per COM contract.
    uint32_t obj = m.arg(0), ppv = m.arg(2);
    m.write32(ppv, obj);
    m.write32(obj + OBJ_REFCOUNT, m.read32(obj + OBJ_REFCOUNT) + 1);
    m.ret(mm.nargs, 0);
}

// --- IDirect3DDevice9 methods ---
void dev_clear(Machine& m, const Method& mm) {
    // Clear(this, Count, pRects, Flags, Color, Z, Stencil).
    if (m.arg(1) != 0) throw MachineError{"IDirect3DDevice9::Clear with rects unimplemented"};
    State& s = g_state;
    s.ensure_bb();
    if (m.arg(3) & 1 /*D3DCLEAR_TARGET*/) {
        uint32_t argb = d3dcolor_to_argb(m.arg(4));
        for (auto& px : s.backbuffer) px = argb;
    }
    m.ret(mm.nargs, 0);
}
void dev_present(Machine& m, const Method& mm) {
    g_state.presented = true;
    m.ret(mm.nargs, 0);
}

// IDirect3DDevice9 vtable in d3d9.h declaration order — POSITION IS THE INDEX.
// nargs matters only for handled rows; null-fn rows throw before returning.
// The SetRenderState/SetFVF no-ops are future state-tracker inputs (recorded
// when the FFP synthesizer lands), not lies about output.
constexpr Method kDeviceVtbl[] = {
    {"QueryInterface", 3, com_query_interface}, {"AddRef", 1, com_addref},
    {"Release", 1, com_release}, {"TestCooperativeLevel", 1, ret_ok},
    {"GetAvailableTextureMem", 0, nullptr}, {"EvictManagedResources", 0, nullptr},
    {"GetDirect3D", 0, nullptr}, {"GetDeviceCaps", 0, nullptr},
    {"GetDisplayMode", 0, nullptr}, {"GetCreationParameters", 0, nullptr},
    {"SetCursorProperties", 0, nullptr}, {"SetCursorPosition", 0, nullptr},
    {"ShowCursor", 0, nullptr}, {"CreateAdditionalSwapChain", 0, nullptr},
    {"GetSwapChain", 0, nullptr}, {"GetNumberOfSwapChains", 0, nullptr},
    {"Reset", 0, nullptr}, {"Present", 5, dev_present},
    {"GetBackBuffer", 0, nullptr}, {"GetRasterStatus", 0, nullptr},
    {"SetDialogBoxMode", 0, nullptr}, {"SetGammaRamp", 0, nullptr},
    {"GetGammaRamp", 0, nullptr}, {"CreateTexture", 0, nullptr},
    {"CreateVolumeTexture", 0, nullptr}, {"CreateCubeTexture", 0, nullptr},
    {"CreateVertexBuffer", 0, nullptr}, {"CreateIndexBuffer", 0, nullptr},
    {"CreateRenderTarget", 0, nullptr}, {"CreateDepthStencilSurface", 0, nullptr},
    {"UpdateSurface", 0, nullptr}, {"UpdateTexture", 0, nullptr},
    {"GetRenderTargetData", 0, nullptr}, {"GetFrontBufferData", 0, nullptr},
    {"StretchRect", 0, nullptr}, {"ColorFill", 0, nullptr},
    {"CreateOffscreenPlainSurface", 0, nullptr}, {"SetRenderTarget", 0, nullptr},
    {"GetRenderTarget", 0, nullptr}, {"SetDepthStencilSurface", 0, nullptr},
    {"GetDepthStencilSurface", 0, nullptr}, {"BeginScene", 1, ret_ok},
    {"EndScene", 1, ret_ok}, {"Clear", 7, dev_clear},
    {"SetTransform", 0, nullptr}, {"GetTransform", 0, nullptr},
    {"MultiplyTransform", 0, nullptr}, {"SetViewport", 0, nullptr},
    {"GetViewport", 0, nullptr}, {"SetMaterial", 0, nullptr},
    {"GetMaterial", 0, nullptr}, {"SetLight", 0, nullptr},
    {"GetLight", 0, nullptr}, {"LightEnable", 0, nullptr},
    {"GetLightEnable", 0, nullptr}, {"SetClipPlane", 0, nullptr},
    {"GetClipPlane", 0, nullptr}, {"SetRenderState", 3, ret_ok},
    {"GetRenderState", 0, nullptr}, {"CreateStateBlock", 0, nullptr},
    {"BeginStateBlock", 0, nullptr}, {"EndStateBlock", 0, nullptr},
    {"SetClipStatus", 0, nullptr}, {"GetClipStatus", 0, nullptr},
    {"GetTexture", 0, nullptr}, {"SetTexture", 0, nullptr},
    {"GetTextureStageState", 0, nullptr}, {"SetTextureStageState", 0, nullptr},
    {"GetSamplerState", 0, nullptr}, {"SetSamplerState", 0, nullptr},
    {"ValidateDevice", 0, nullptr}, {"SetPaletteEntries", 0, nullptr},
    {"GetPaletteEntries", 0, nullptr}, {"SetCurrentTexturePalette", 0, nullptr},
    {"GetCurrentTexturePalette", 0, nullptr}, {"SetScissorRect", 0, nullptr},
    {"GetScissorRect", 0, nullptr}, {"SetSoftwareVertexProcessing", 0, nullptr},
    {"GetSoftwareVertexProcessing", 0, nullptr}, {"SetNPatchMode", 0, nullptr},
    {"GetNPatchMode", 0, nullptr}, {"DrawPrimitive", 0, nullptr},
    {"DrawIndexedPrimitive", 0, nullptr}, {"DrawPrimitiveUP", 0, nullptr},
    {"DrawIndexedPrimitiveUP", 0, nullptr}, {"ProcessVertices", 0, nullptr},
    {"CreateVertexDeclaration", 0, nullptr}, {"SetVertexDeclaration", 0, nullptr},
    {"GetVertexDeclaration", 0, nullptr}, {"SetFVF", 2, ret_ok},
    {"GetFVF", 0, nullptr}, {"CreateVertexShader", 0, nullptr},
    {"SetVertexShader", 0, nullptr}, {"GetVertexShader", 0, nullptr},
    {"SetVertexShaderConstantF", 0, nullptr}, {"GetVertexShaderConstantF", 0, nullptr},
    {"SetVertexShaderConstantI", 0, nullptr}, {"GetVertexShaderConstantI", 0, nullptr},
    {"SetVertexShaderConstantB", 0, nullptr}, {"GetVertexShaderConstantB", 0, nullptr},
    {"SetStreamSource", 0, nullptr}, {"GetStreamSource", 0, nullptr},
    {"SetStreamSourceFreq", 0, nullptr}, {"GetStreamSourceFreq", 0, nullptr},
    {"SetIndices", 0, nullptr}, {"GetIndices", 0, nullptr},
    {"CreatePixelShader", 0, nullptr}, {"SetPixelShader", 0, nullptr},
    {"GetPixelShader", 0, nullptr}, {"SetPixelShaderConstantF", 0, nullptr},
    {"GetPixelShaderConstantF", 0, nullptr}, {"SetPixelShaderConstantI", 0, nullptr},
    {"GetPixelShaderConstantI", 0, nullptr}, {"SetPixelShaderConstantB", 0, nullptr},
    {"GetPixelShaderConstantB", 0, nullptr}, {"DrawRectPatch", 0, nullptr},
    {"DrawTriPatch", 0, nullptr}, {"DeletePatch", 0, nullptr},
    {"CreateQuery", 0, nullptr},
};
static_assert(sizeof(kDeviceVtbl) / sizeof(Method) == 119,
              "IDirect3DDevice9 has 119 vtable entries");

// --- IDirect3D9 methods ---
void d3d9_create_device(Machine& m, const Method& mm) {
    // CreateDevice(this, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
    //   pPresentationParameters, ppReturnedDeviceInterface).
    uint32_t pp = m.arg(5), ppOut = m.arg(6);
    uint32_t w = m.read32(pp + 0), h = m.read32(pp + 4);
    // BackBufferWidth/Height 0 means "derive from window" (windowed) — our
    // 640x480 default is the right fallback. Cap to bound the allocation.
    if (w && w <= 4096) g_state.width = w;
    if (h && h <= 4096) g_state.height = h;
    g_state.ensure_bb();
    uint32_t dev = m.create_com_instance(g_state.device_vtbl, OBJ_STATE_BYTES);
    m.write32(dev + OBJ_REFCOUNT, 1);
    m.write32(ppOut, dev);
    m.ret(mm.nargs, 0);
}

constexpr Method kD3d9Vtbl[] = {
    {"QueryInterface", 3, com_query_interface}, {"AddRef", 1, com_addref},
    {"Release", 1, com_release}, {"RegisterSoftwareDevice", 0, nullptr},
    {"GetAdapterCount", 0, nullptr}, {"GetAdapterIdentifier", 0, nullptr},
    {"GetAdapterModeCount", 0, nullptr}, {"EnumAdapterModes", 0, nullptr},
    {"GetAdapterDisplayMode", 0, nullptr}, {"CheckDeviceType", 0, nullptr},
    {"CheckDeviceFormat", 0, nullptr}, {"CheckDeviceMultiSampleType", 0, nullptr},
    {"CheckDepthStencilMatch", 0, nullptr}, {"CheckDeviceFormatConversion", 0, nullptr},
    {"GetDeviceCaps", 0, nullptr}, {"GetAdapterMonitor", 0, nullptr},
    {"CreateDevice", 7, d3d9_create_device},
};
static_assert(sizeof(kD3d9Vtbl) / sizeof(Method) == 17,
              "IDirect3D9 has 17 vtable entries");

// Dispatch one vtable call: look the method up in the interface's table, throw
// a named error if unimplemented, else run it.
template <const Method* Vtbl, size_t N>
void dispatch(Machine& m, unsigned i, const char* iface) {
    if (i >= N || !Vtbl[i].fn)
        throw MachineError{std::string("unimplemented ") + iface + "::" +
                           (i < N ? Vtbl[i].name : "?") + " (vtable " +
                           std::to_string(i) + ")"};
    Vtbl[i].fn(m, Vtbl[i]);
}
void device_method(Machine& m, unsigned i) {
    dispatch<kDeviceVtbl, 119>(m, i, "IDirect3DDevice9");
}
void d3d9_method(Machine& m, unsigned i) {
    dispatch<kD3d9Vtbl, 17>(m, i, "IDirect3D9");
}

} // namespace

void install(Machine& m) {
    g_state = State{};

    m.add_handler([](Machine& m, const std::string& dll,
                     const std::string& name) -> bool {
        if (dll != "d3d9.dll") return false;
        if (name == "Direct3DCreate9") {
            // Create the shared vtables on first use (heap is stable now — after
            // run_entry's reset). One vtable per interface class, forever.
            if (!g_state.d3d9_vtbl) {
                g_state.d3d9_vtbl = m.create_com_vtable(17, d3d9_method);
                g_state.device_vtbl = m.create_com_vtable(119, device_method);
            }
            uint32_t obj = m.create_com_instance(g_state.d3d9_vtbl, OBJ_STATE_BYTES);
            m.write32(obj + OBJ_REFCOUNT, 1);
            m.ret(1, obj);
            return true;
        }
        return false;
    });
}

const uint32_t* framebuffer(uint32_t& width, uint32_t& height) {
    if (!g_state.presented || g_state.backbuffer.empty()) return nullptr;
    width = g_state.width;
    height = g_state.height;
    return g_state.backbuffer.data();
}

} // namespace d9web
