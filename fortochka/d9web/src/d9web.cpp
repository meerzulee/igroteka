#include "d9web/d9web.h"

#include <cstdio>
#include <string>
#include <vector>

namespace d9web {

using runtime::Machine;

namespace {

// IDirect3D9 vtable indices we handle (others throw until a caller appears).
enum : unsigned { D3D9_Release = 2, D3D9_GetDeviceCaps = 4 + 10 /*14*/,
                  D3D9_CreateDevice = 16 };
// IDirect3DDevice9 vtable indices (D3D9 order).
enum : unsigned {
    DEV_Release = 2, DEV_TestCooperativeLevel = 3, DEV_Present = 17,
    DEV_BeginScene = 41, DEV_EndScene = 42, DEV_Clear = 43,
    DEV_SetRenderState = 57, DEV_DrawPrimitiveUP = 83, DEV_SetFVF = 89,
};

constexpr unsigned D3D9_NUM_METHODS = 17;   // through CreateDevice
constexpr unsigned DEVICE_NUM_METHODS = 120; // full IDirect3DDevice9 vtable

struct State {
    uint32_t width = 640, height = 480;
    std::vector<uint32_t> backbuffer; // ARGB 0xFFRRGGBB, width*height
    bool presented = false;
    void ensure_bb() {
        if (backbuffer.size() != (size_t)width * height)
            backbuffer.assign((size_t)width * height, 0xFF000000u);
    }
};
State g_state;

// D3DCOLOR (0xAARRGGBB) → opaque framebuffer ARGB.
inline uint32_t d3dcolor(uint32_t c) { return 0xFF000000u | (c & 0x00FFFFFF); }

void device_method(Machine& m, unsigned method);

// Fill the whole backbuffer with a D3DCOLOR (Clear with no rects = full target).
void do_clear(Machine& m) {
    // Clear(this, Count, pRects, Flags, Color, Z, Stencil) — 7 dwords.
    State& s = g_state;
    s.ensure_bb();
    uint32_t argb = d3dcolor(m.arg(4));
    for (auto& px : s.backbuffer) px = argb;
    m.ret(7, 0); // D3D_OK
}

void device_method(Machine& m, unsigned method) {
    switch (method) {
        case DEV_Clear: do_clear(m); return;
        case DEV_BeginScene: m.ret(1, 0); return;
        case DEV_EndScene: m.ret(1, 0); return;
        case DEV_Present:
            g_state.presented = true;
            m.ret(5, 0); // Present(pSrc,pDst,hWnd,pDirty) + this
            return;
        case DEV_TestCooperativeLevel: m.ret(1, 0); return;
        case DEV_SetRenderState: m.ret(3, 0); return; // this,state,value
        case DEV_SetFVF: m.ret(2, 0); return;         // this,fvf
        case DEV_Release: m.ret(1, 0); return;
        default:
            throw runtime::MachineError{"unimplemented IDirect3DDevice9 method " +
                                        std::to_string(method)};
    }
}

void d3d9_method(Machine& m, unsigned method) {
    switch (method) {
        case D3D9_CreateDevice: {
            // CreateDevice(this, Adapter, DeviceType, hFocusWindow,
            //   BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface)
            uint32_t pp = m.arg(5), ppOut = m.arg(6);
            uint32_t w = m.read32(pp + 0), h = m.read32(pp + 4);
            if (w && w <= 4096) g_state.width = w;
            if (h && h <= 4096) g_state.height = h;
            g_state.ensure_bb();
            uint32_t dev = m.create_com_object(DEVICE_NUM_METHODS, 8, device_method);
            m.write32(ppOut, dev);
            m.ret(7, 0); // D3D_OK
            return;
        }
        case D3D9_Release: m.ret(1, 0); return;
        default:
            throw runtime::MachineError{"unimplemented IDirect3D9 method " +
                                        std::to_string(method)};
    }
}

} // namespace

void install(Machine& m) {
    g_state = State{};

    m.add_handler([](Machine& m, const std::string& dll,
                     const std::string& name) -> bool {
        if (dll != "d3d9.dll") return false;
        if (name == "Direct3DCreate9") {
            // IDirect3D9* Direct3DCreate9(UINT SDKVersion). Returns the COM
            // object; its vtable thunks dispatch through d3d9_method.
            uint32_t obj = m.create_com_object(D3D9_NUM_METHODS, 8, d3d9_method);
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
