#include "d9web/d9web.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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
// FVF bits we recognize.
constexpr uint32_t FVF_XYZ = 0x002, FVF_XYZRHW = 0x004, FVF_DIFFUSE = 0x040,
                   FVF_TEX1 = 0x100;

// Texture object state (past the vtable ptr): refcount, backing VA, W, H.
constexpr uint32_t TEX_REFCOUNT = 4, TEX_BACKING = 8, TEX_W = 12, TEX_H = 16;
constexpr uint32_t TEX_STATE_BYTES = 16;

// 4x4 identity (row-major, D3D row-vector convention: clip = v * M).
inline void mat_identity(float* m) {
    for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
}
inline void mat_mul(const float* a, const float* b, float* out) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                             a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
}

struct State {
    uint32_t width = 640, height = 480;
    std::vector<uint32_t> backbuffer; // ARGB 0xFFRRGGBB, width*height
    std::vector<float> zbuffer;       // depth, width*height, 1.0 = far
    bool presented = false;
    uint32_t fvf = 0; // current SetFVF
    uint32_t rs[256] = {}; // render states (SetRenderState), indexed by state #
    // Bound stream 0 (SetStreamSource) + index buffer (SetIndices): the COM
    // objects, byte offset into the stream, and vertex stride.
    uint32_t stream_vb = 0, stream_offset = 0, stream_stride = 0, stream_ib = 0;
    // Fixed-function transform state (SetTransform / SetViewport). Untransformed
    // XYZ vertices go through World*View*Projection then the viewport map.
    float world[16], view[16], proj[16];
    float vp_x = 0, vp_y = 0, vp_w = 640, vp_h = 480;
    uint32_t tex_stage0 = 0; // SetTexture(0, ...): bound texture object, 0 = none
    uint32_t d3d9_vtbl = 0, device_vtbl = 0, vbuffer_vtbl = 0,
             texture_vtbl = 0; // 0 until first use
    State() {
        mat_identity(world);
        mat_identity(view);
        mat_identity(proj);
    }
    void ensure_bb() {
        size_t n = (size_t)width * height;
        if (backbuffer.size() != n) backbuffer.assign(n, 0xFF000000u);
        if (zbuffer.size() != n) zbuffer.assign(n, 1.0f);
    }
};
State g_state;

// Render-state indices (D3DRENDERSTATETYPE) and enum values we act on.
constexpr uint32_t RS_ZENABLE = 7, RS_ZWRITEENABLE = 14, RS_ALPHABLENDENABLE = 27,
                   RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_CULLMODE = 22;
constexpr uint32_t CULL_NONE = 1, CULL_CW = 2, CULL_CCW = 3;
constexpr uint32_t BLEND_SRCALPHA = 5, BLEND_INVSRCALPHA = 6;

// Seed the render-state array with the D3D9 defaults we act on, so the raster
// path can read them directly without a "0 means default" ambiguity (a guest
// value of 0 is a real choice, e.g. ZWRITEENABLE=FALSE). Everything not seeded
// stays 0, which is the D3D default for the states we don't yet honour.
void seed_render_states(uint32_t rs[256]) {
    rs[RS_ZENABLE] = 0;          // D3DZB_FALSE — depth off until the guest asks
    rs[RS_ZWRITEENABLE] = 1;     // TRUE — write depth on a passing test
    rs[RS_CULLMODE] = CULL_CCW;  // D3DCULL_CCW is the D3D default
    rs[RS_ALPHABLENDENABLE] = 0; // opaque by default
    rs[RS_SRCBLEND] = BLEND_SRCALPHA;
    rs[RS_DESTBLEND] = BLEND_INVSRCALPHA;
}

// Vertex-buffer object state (past the vtable ptr at +0): refcount, the backing
// store's guest VA, and its length in bytes. Index buffers reuse this layout
// (same COM shape) plus one dword for the index format.
constexpr uint32_t VB_REFCOUNT = 4, VB_BACKING = 8, VB_LENGTH = 12, IB_FORMAT = 16;
constexpr uint32_t VB_STATE_BYTES = 12, IB_STATE_BYTES = 16;

// Reinterpret a 32-bit value as an IEEE-754 float (a float passed by value).
inline float read_f32_bits(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
// Read a guest float from memory.
inline float read_f32(Machine& m, uint32_t va) { return read_f32_bits(m.read32(va)); }

// Zero a guest struct of exactly `bytes` (never more — writing past a caller's
// allocation would corrupt adjacent guest memory). write32 is bounds-checked,
// so an out-of-arena pointer is a no-op, not a host write. `bytes` is a
// compile-time struct size, always a multiple of 4.
inline void zero_guest(Machine& m, uint32_t p, uint32_t bytes) {
    for (uint32_t off = 0; off < bytes; off += 4) m.write32(p + off, 0);
}

// Fill a D3DDISPLAYMODE {Width, Height, RefreshRate, D3DFORMAT}.
void fill_display_mode(Machine& m, uint32_t p, uint32_t w, uint32_t h,
                       uint32_t refresh, uint32_t fmt) {
    if (!p) return;
    m.write32(p + 0, w);
    m.write32(p + 4, h);
    m.write32(p + 8, refresh);
    m.write32(p + 12, fmt);
}

// Forge a D3DCAPS9 (sizeof 304) describing a minimal fixed-function software
// card: no programmable shaders (Vertex/PixelShaderVersion = 0) so a game that
// can fall back to the FFP — which our rasterizer implements — does. Only the
// fields a typical D3D9 app gates on are set; the rest stay zero.
constexpr uint32_t D3DCAPS9_SIZE = 304;
void fill_caps(Machine& m, uint32_t p, uint32_t devtype) {
    if (!p) return;
    zero_guest(m, p, D3DCAPS9_SIZE);
    m.write32(p + 0, devtype ? devtype : 1); // DeviceType (D3DDEVTYPE_HAL = 1)
    m.write32(p + 88, 4096);                 // MaxTextureWidth
    m.write32(p + 92, 4096);                 // MaxTextureHeight
    m.write32(p + 100, 8192);                // MaxTextureRepeat
    m.write32(p + 148, 8);                   // MaxTextureBlendStages
    m.write32(p + 152, 1);                   // MaxSimultaneousTextures (stage 0 only)
    m.write32(p + 160, 8);                   // MaxActiveLights
    m.write32(p + 180, 0x000FFFFF);          // MaxPrimitiveCount
    m.write32(p + 184, 0x00FFFFFF);          // MaxVertexIndex
    m.write32(p + 188, 1);                   // MaxStreams
    m.write32(p + 192, 0x0000FFFF);          // MaxStreamStride
    m.write32(p + 196, 0);                   // VertexShaderVersion = none → FFP
    m.write32(p + 204, 0);                   // PixelShaderVersion  = none → FFP
    m.write32(p + 240, 1);                   // NumSimultaneousRTs
}

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
    uint32_t flags = m.arg(3);
    if (flags & 1 /*D3DCLEAR_TARGET*/) {
        uint32_t argb = d3dcolor_to_argb(m.arg(4));
        for (auto& px : s.backbuffer) px = argb;
    }
    if (flags & 2 /*D3DCLEAR_ZBUFFER*/) {
        float z = read_f32_bits(m.arg(5)); // Z clear value (default 1.0)
        for (auto& d : s.zbuffer) d = z;
    }
    m.ret(mm.nargs, 0);
}
void dev_present(Machine& m, const Method& mm) {
    g_state.presented = true;
    m.ret(mm.nargs, 0);
}
void dev_set_fvf(Machine& m, const Method& mm) {
    g_state.fvf = m.arg(1); // state-tracker input; consumed by the draw path
    m.ret(mm.nargs, 0);
}
void dev_set_render_state(Machine& m, const Method& mm) {
    uint32_t state = m.arg(1); // SetRenderState(this, State, Value)
    if (state < 256) g_state.rs[state] = m.arg(2);
    m.ret(mm.nargs, 0);
}
void dev_set_transform(Machine& m, const Method& mm) {
    // SetTransform(this, D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix).
    uint32_t which = m.arg(1), p = m.arg(2);
    float* dst = which == 256 /*WORLD*/ ? g_state.world
               : which == 2 /*VIEW*/    ? g_state.view
               : which == 3 /*PROJECTION*/ ? g_state.proj
                                           : nullptr;
    if (dst)
        for (int i = 0; i < 16; i++) dst[i] = read_f32(m, p + 4 * i);
    m.ret(mm.nargs, 0);
}
void dev_set_viewport(Machine& m, const Method& mm) {
    // SetViewport(this, D3DVIEWPORT9*): {DWORD X,Y,Width,Height; float MinZ,MaxZ}.
    uint32_t p = m.arg(1);
    g_state.vp_x = (float)m.read32(p + 0);
    g_state.vp_y = (float)m.read32(p + 4);
    g_state.vp_w = (float)m.read32(p + 8);
    g_state.vp_h = (float)m.read32(p + 12);
    m.ret(mm.nargs, 0);
}

// One screen-space vertex: position, depth, Gouraud color, and texture coords.
struct Vtx {
    float x, y, z;  // z in [0,1] for depth testing
    uint32_t color; // D3DCOLOR 0xAARRGGBB
    float u, v;
};
inline float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

// Nearest-neighbour texel from a bound texture (A8R8G8B8), UV wrapped. Wraps in
// FLOAT first (u - floor(u) ∈ [0,1)) so the texel-index cast is always in range
// — a huge finite guest UV would otherwise make (int)(u*tw) out-of-range UB.
inline uint32_t sample_tex(Machine& m, uint32_t tex, float u, float v) {
    int tw = (int)m.read32(tex + TEX_W), th = (int)m.read32(tex + TEX_H);
    if (tw <= 0 || th <= 0) return 0xFFFFFFFFu;
    if (!std::isfinite(u) || !std::isfinite(v)) return 0xFFFFFFFFu;
    float fu = u - std::floor(u), fv = v - std::floor(v); // wrap to [0,1)
    int tx = (int)(fu * tw), ty = (int)(fv * th);         // now in [0,dim)
    if (tx >= tw) tx = tw - 1; // guard the [0,1) upper-edge rounding
    if (ty >= th) ty = th - 1;
    return m.read32(m.read32(tex + TEX_BACKING) + ((size_t)ty * tw + tx) * 4);
}

// Software-rasterize one screen-space triangle into the backbuffer (barycentric
// coverage, Gouraud color, optional nearest-sampled texture modulated by the
// vertex color, no depth/cull). Returns the pixel-test count for the work
// budget. Guest floats are attacker-controlled: reject non-finite coords and a
// non-finite 1/area before any float→int cast (UB on NaN/inf/out-of-range) and
// clamp the bbox AS FLOAT to [0, dim-1] so every cast is of an in-range value.
uint64_t raster_triangle(Machine& m, State& s, uint32_t tex, const Vtx& a,
                         const Vtx& b, const Vtx& c) {
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) ||
        !std::isfinite(b.y) || !std::isfinite(c.x) || !std::isfinite(c.y))
        return 0;
    float area = edge(a.x, a.y, b.x, b.y, c.x, c.y);
    if (area == 0) return 0;
    // Backface culling by winding sign (CULL_NONE draws both). In this y-down
    // screen space with the edge() convention, a front-facing
    // (clockwise-on-screen) triangle has area < 0, so D3DCULL_CCW culls area > 0.
    switch (s.rs[RS_CULLMODE]) {
        case CULL_CW:   if (area < 0) return 0; break;
        case CULL_CCW:  if (area > 0) return 0; break;
        case CULL_NONE: default: break; // draw both faces
    }
    float inv = 1.0f / area; // sign handles either winding
    if (!std::isfinite(inv)) return 0; // near-degenerate: weights would be NaN
    const bool depth = s.rs[RS_ZENABLE] && !s.zbuffer.empty();
    const bool blend = s.rs[RS_ALPHABLENDENABLE];
    auto clampf = [](float v, float hi) { return std::clamp(v, 0.0f, hi); };
    int minx = (int)clampf(std::min({a.x, b.x, c.x}), (float)(s.width - 1));
    int maxx = (int)clampf(std::max({a.x, b.x, c.x}), (float)(s.width - 1));
    int miny = (int)clampf(std::min({a.y, b.y, c.y}), (float)(s.height - 1));
    int maxy = (int)clampf(std::max({a.y, b.y, c.y}), (float)(s.height - 1));
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = edge(b.x, b.y, c.x, c.y, px, py) * inv;
            float w1 = edge(c.x, c.y, a.x, a.y, px, py) * inv;
            float w2 = edge(a.x, a.y, b.x, b.y, px, py) * inv;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue; // outside
            size_t idx = (size_t)y * s.width + x;
            if (depth) { // D3DCMP_LESSEQUAL against the z-buffer
                float z = w0 * a.z + w1 * b.z + w2 * c.z;
                if (!std::isfinite(z) || z > s.zbuffer[idx]) continue;
                if (s.rs[RS_ZWRITEENABLE]) s.zbuffer[idx] = z;
            }
            auto chan = [&](int sh) {
                float v = w0 * ((a.color >> sh) & 0xFF) +
                          w1 * ((b.color >> sh) & 0xFF) +
                          w2 * ((c.color >> sh) & 0xFF);
                return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            };
            uint32_t r = chan(16), g = chan(8), bl = chan(0), sa = chan(24);
            if (tex) { // MODULATE: texel * diffuse (D3D default stage op)
                float tu = w0 * a.u + w1 * b.u + w2 * c.u;
                float tv = w0 * a.v + w1 * b.v + w2 * c.v;
                uint32_t t = sample_tex(m, tex, tu, tv);
                r = r * ((t >> 16) & 0xFF) / 255;
                g = g * ((t >> 8) & 0xFF) / 255;
                bl = bl * (t & 0xFF) / 255;
                sa = sa * ((t >> 24) & 0xFF) / 255;
            }
            if (blend && s.rs[RS_SRCBLEND] == BLEND_SRCALPHA &&
                s.rs[RS_DESTBLEND] == BLEND_INVSRCALPHA) {
                // The common src-over: out = src*a + dst*(1-a).
                uint32_t d = s.backbuffer[idx];
                r = (r * sa + ((d >> 16) & 0xFF) * (255 - sa)) / 255;
                g = (g * sa + ((d >> 8) & 0xFF) * (255 - sa)) / 255;
                bl = (bl * sa + (d & 0xFF) * (255 - sa)) / 255;
            }
            s.backbuffer[idx] = 0xFF000000u | (r << 16) | (g << 8) | bl;
        }
    }
    return (uint64_t)(maxx - minx + 1) * (maxy - miny + 1);
}

// A single draw call may cover at most this many primitives (era
// MaxPrimitiveCount is ~5.5M) and touch at most this much fill (64 screens),
// bounding attacker-controlled work regardless of count / triangle size.
constexpr uint32_t kMaxPrimitives = 4'000'000;

// Read one vertex at `p` and produce a SCREEN-space Vtx. XYZRHW vertices are
// already pre-transformed (x,y in pixels); XYZ vertices go through wvp then the
// perspective divide and viewport map. Color from DIFFUSE, else opaque white.
Vtx vertex_screen(Machine& m, uint32_t p, const float* wvp) {
    const uint32_t fvf = g_state.fvf;
    // Component offsets from the FVF: position, then DIFFUSE, then TEX1.
    uint32_t off = (fvf & FVF_XYZRHW) ? 16 : 12;
    uint32_t color_off = off;
    if (fvf & FVF_DIFFUSE) off += 4;
    uint32_t uv_off = off;
    uint32_t col = (fvf & FVF_DIFFUSE) ? m.read32(p + color_off) : 0xFFFFFFFFu;
    float u = 0, vv = 0;
    if (fvf & FVF_TEX1) {
        u = read_f32(m, p + uv_off);
        vv = read_f32(m, p + uv_off + 4);
    }
    if (fvf & FVF_XYZRHW) // pre-transformed: x,y in pixels, z already in [0,1]
        return {read_f32(m, p + 0), read_f32(m, p + 4), read_f32(m, p + 8), col, u, vv};
    // Untransformed XYZ: clip = [x,y,z,1] * wvp, then divide + viewport.
    float in[4] = {read_f32(m, p + 0), read_f32(m, p + 4), read_f32(m, p + 8), 1.0f};
    float clip[4];
    for (int j = 0; j < 4; j++)
        clip[j] = in[0] * wvp[0 * 4 + j] + in[1] * wvp[1 * 4 + j] +
                  in[2] * wvp[2 * 4 + j] + in[3] * wvp[3 * 4 + j];
    float w = clip[3] == 0.0f ? 1e-6f : clip[3];
    float sx = (clip[0] / w * 0.5f + 0.5f) * g_state.vp_w + g_state.vp_x;
    float sy = (0.5f - clip[1] / w * 0.5f) * g_state.vp_h + g_state.vp_y; // y flips
    return {sx, sy, clip[2] / w, col, u, vv}; // z in [0,1] (D3D projection)
}

// Rasterize `count` triangles. `vaddr(j)` returns the guest address of the j-th
// vertex (j in 0..3*count-1) — a linear stream for DrawPrimitive[UP], or an
// index lookup for DrawIndexedPrimitive. Validates the FVF, computes WVP once,
// and enforces the primitive cap + fill budget.
template <typename Vaddr>
void draw_core(Machine& m, uint32_t count, Vaddr vaddr) {
    const uint32_t fvf = g_state.fvf;
    // Position required, exactly one of XYZ / XYZRHW (mutually exclusive);
    // DIFFUSE + TEX1 optional. Other components (normals, > stream-0, extra tex
    // coords) aren't in the layout — throw named, add on demand. Runs BEFORE any
    // vertex_screen parse, so an unsupported FVF never reaches the parser.
    bool has_xyz = fvf & FVF_XYZ, has_rhw = fvf & FVF_XYZRHW;
    bool ok = (has_xyz != has_rhw) &&
              !(fvf & ~(FVF_XYZ | FVF_XYZRHW | FVF_DIFFUSE | FVF_TEX1));
    if (!ok)
        throw MachineError{"draw: unsupported FVF 0x" + std::to_string(fvf)};
    if (count > kMaxPrimitives)
        throw MachineError{"draw: PrimitiveCount too large"};
    g_state.ensure_bb();
    float wv[16], wvp[16];
    mat_mul(g_state.world, g_state.view, wv);
    mat_mul(wv, g_state.proj, wvp);
    const uint32_t tex = (fvf & FVF_TEX1) ? g_state.tex_stage0 : 0;
    const uint64_t fill_budget = 64ull * g_state.width * g_state.height;
    uint64_t filled = 0;
    for (uint32_t t = 0; t < count; t++) {
        filled += raster_triangle(m, g_state, tex,
                                  vertex_screen(m, vaddr(3 * t), wvp),
                                  vertex_screen(m, vaddr(3 * t + 1), wvp),
                                  vertex_screen(m, vaddr(3 * t + 2), wvp));
        if (filled > fill_budget) break; // bound total attacker-controlled fill
    }
}

void dev_draw_primitive_up(Machine& m, const Method& mm) {
    // DrawPrimitiveUP(this, PrimitiveType, PrimitiveCount, pVertexData, Stride).
    if (m.arg(1) != 4 /*D3DPT_TRIANGLELIST*/)
        throw MachineError{"DrawPrimitiveUP: only D3DPT_TRIANGLELIST supported"};
    uint32_t data = m.arg(3), stride = m.arg(4);
    draw_core(m, m.arg(2), [=](uint32_t j) { return data + j * stride; });
    m.ret(mm.nargs, 0);
}
void dev_draw_primitive(Machine& m, const Method& mm) {
    // DrawPrimitive(this, PrimitiveType, StartVertex, PrimitiveCount) — draws
    // from the vertex buffer bound via SetStreamSource.
    if (m.arg(1) != 4 /*D3DPT_TRIANGLELIST*/)
        throw MachineError{"DrawPrimitive: only D3DPT_TRIANGLELIST supported"};
    if (!g_state.stream_vb)
        throw MachineError{"DrawPrimitive: no vertex buffer bound"};
    uint32_t base = m.read32(g_state.stream_vb + VB_BACKING) + g_state.stream_offset;
    uint32_t start = m.arg(2), stride = g_state.stream_stride;
    draw_core(m, m.arg(3), [=](uint32_t j) { return base + (start + j) * stride; });
    m.ret(mm.nargs, 0);
}
void dev_draw_indexed_primitive(Machine& m, const Method& mm) {
    // DrawIndexedPrimitive(this, PrimType, BaseVertexIndex, MinIndex,
    //   NumVertices, StartIndex, PrimitiveCount). Vertices come from the bound
    //   stream, indexed through the bound index buffer.
    if (m.arg(1) != 4 /*D3DPT_TRIANGLELIST*/)
        throw MachineError{"DrawIndexedPrimitive: only D3DPT_TRIANGLELIST supported"};
    if (!g_state.stream_vb || !g_state.stream_ib)
        throw MachineError{"DrawIndexedPrimitive: no vertex/index buffer bound"};
    uint32_t vb_base = m.read32(g_state.stream_vb + VB_BACKING) + g_state.stream_offset;
    uint32_t ib_base = m.read32(g_state.stream_ib + VB_BACKING);
    uint32_t isz = m.read32(g_state.stream_ib + IB_FORMAT) == 101 /*INDEX16*/ ? 2 : 4;
    int32_t base_vertex = (int32_t)m.arg(2);
    uint32_t start_index = m.arg(5), stride = g_state.stream_stride;
    auto vaddr = [&](uint32_t j) {
        uint32_t ia = ib_base + (start_index + j) * isz;
        uint32_t idx = isz == 2 ? (m.read32(ia) & 0xFFFF) : m.read32(ia);
        return vb_base + (uint32_t)(base_vertex + (int32_t)idx) * stride;
    };
    draw_core(m, m.arg(6), vaddr);
    m.ret(mm.nargs, 0);
}
void dev_set_stream_source(Machine& m, const Method& mm) {
    // SetStreamSource(this, StreamNumber, pStreamData, OffsetInBytes, Stride).
    g_state.stream_vb = m.arg(2);
    g_state.stream_offset = m.arg(3);
    g_state.stream_stride = m.arg(4);
    m.ret(mm.nargs, 0);
}
void dev_set_indices(Machine& m, const Method& mm) {
    g_state.stream_ib = m.arg(1); // SetIndices(this, pIndexData)
    m.ret(mm.nargs, 0);
}
void dev_create_vertex_buffer(Machine& m, const Method& mm) {
    // CreateVertexBuffer(this, Length, Usage, FVF, Pool, ppVB, pSharedHandle).
    uint32_t length = m.arg(1), ppVB = m.arg(5);
    uint32_t backing = m.alloc(length ? length : 4); // identity-mapped store
    uint32_t vb = m.create_com_instance(g_state.vbuffer_vtbl, VB_STATE_BYTES);
    m.write32(vb + VB_REFCOUNT, 1);
    m.write32(vb + VB_BACKING, backing);
    m.write32(vb + VB_LENGTH, length);
    m.write32(ppVB, vb);
    m.ret(mm.nargs, 0);
}
void dev_create_index_buffer(Machine& m, const Method& mm) {
    // CreateIndexBuffer(this, Length, Usage, Format, Pool, ppIB, pSharedHandle).
    // Index buffers share the vertex-buffer vtable (identical IUnknown+resource+
    // Lock/Unlock shape); the extra dword is the index format (D3DFMT_INDEX16/32).
    uint32_t length = m.arg(1), fmt = m.arg(3), ppIB = m.arg(5);
    uint32_t backing = m.alloc(length ? length : 4);
    uint32_t ib = m.create_com_instance(g_state.vbuffer_vtbl, IB_STATE_BYTES);
    m.write32(ib + VB_REFCOUNT, 1);
    m.write32(ib + VB_BACKING, backing);
    m.write32(ib + VB_LENGTH, length);
    m.write32(ib + IB_FORMAT, fmt);
    m.write32(ppIB, ib);
    m.ret(mm.nargs, 0);
}
void vb_lock(Machine& m, const Method& mm) {
    // Lock(this, OffsetToLock, SizeToLock, ppbData, Flags). Hand back a direct
    // guest pointer into the backing store — no copy (identity-mapped memory).
    // Validate the range so the returned pointer stays inside the buffer (a
    // guest that then memcpys past it can only corrupt its own sandbox, but
    // this catches the mistake honestly). SizeToLock 0 = to end of buffer.
    uint32_t vb = m.arg(0), offset = m.arg(1), size = m.arg(2), ppb = m.arg(3);
    uint32_t length = m.read32(vb + VB_LENGTH);
    if (offset > length || (size && (uint64_t)offset + size > length))
        throw MachineError{"IDirect3DVertexBuffer9::Lock: range out of bounds"};
    m.write32(ppb, m.read32(vb + VB_BACKING) + offset);
    m.ret(mm.nargs, 0);
}

// --- IDirect3DTexture9 ---
void dev_create_texture(Machine& m, const Method& mm) {
    // CreateTexture(this, Width, Height, Levels, Usage, Format, Pool, ppTexture,
    //   pSharedHandle). Backing store is Width*Height A8R8G8B8 texels.
    uint32_t w = m.arg(1), h = m.arg(2), ppTex = m.arg(7);
    if (w == 0 || h == 0 || w > 4096 || h > 4096)
        throw MachineError{"CreateTexture: unsupported dimensions"};
    uint32_t backing = m.alloc(w * h * 4);
    uint32_t tex = m.create_com_instance(g_state.texture_vtbl, TEX_STATE_BYTES);
    m.write32(tex + TEX_REFCOUNT, 1);
    m.write32(tex + TEX_BACKING, backing);
    m.write32(tex + TEX_W, w);
    m.write32(tex + TEX_H, h);
    m.write32(ppTex, tex);
    m.ret(mm.nargs, 0);
}
void tex_lock_rect(Machine& m, const Method& mm) {
    // LockRect(this, Level, D3DLOCKED_RECT* {INT Pitch; void* pBits}, RECT*,
    //   Flags). Level 0 only; hand back a guest pointer to the whole surface.
    uint32_t tex = m.arg(0), plr = m.arg(2);
    uint32_t w = m.read32(tex + TEX_W);
    m.write32(plr + 0, w * 4);                     // Pitch
    m.write32(plr + 4, m.read32(tex + TEX_BACKING)); // pBits
    m.ret(mm.nargs, 0);
}
void dev_set_texture(Machine& m, const Method& mm) {
    // SetTexture(this, Stage, pTexture). Stage 0 honored; others recorded-noop.
    if (m.arg(1) == 0) g_state.tex_stage0 = m.arg(2);
    m.ret(mm.nargs, 0);
}
void dev_get_device_caps(Machine& m, const Method& mm) {
    fill_caps(m, m.arg(1), 1); // GetDeviceCaps(this, D3DCAPS9*) — HAL persona
    m.ret(mm.nargs, 0);
}
void dev_get_display_mode(Machine& m, const Method& mm) {
    // GetDisplayMode(this, iSwapChain, D3DDISPLAYMODE*). 22 = D3DFMT_X8R8G8B8.
    fill_display_mode(m, m.arg(2), g_state.width, g_state.height, 60, 22);
    m.ret(mm.nargs, 0);
}
void dev_reset(Machine& m, const Method& mm) {
    // Reset(this, D3DPRESENT_PARAMETERS*): re-apply the backbuffer size and
    // reseed default render states, exactly as a fresh CreateDevice. Real D3D
    // also releases all default-pool resources; ours are immortal HLE objects,
    // so the observable duty here is restoring default state so post-Reset
    // drawing behaves (Codex noted stale rs[] would otherwise persist).
    uint32_t pp = m.arg(1);
    uint32_t w = m.read32(pp + 0), h = m.read32(pp + 4);
    if (w && w <= 4096) g_state.width = w;
    if (h && h <= 4096) g_state.height = h;
    g_state.ensure_bb();
    g_state.vp_x = g_state.vp_y = 0;
    g_state.vp_w = (float)g_state.width;
    g_state.vp_h = (float)g_state.height;
    seed_render_states(g_state.rs);
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
    {"GetDirect3D", 0, nullptr}, {"GetDeviceCaps", 2, dev_get_device_caps},
    {"GetDisplayMode", 3, dev_get_display_mode}, {"GetCreationParameters", 0, nullptr},
    {"SetCursorProperties", 0, nullptr}, {"SetCursorPosition", 0, nullptr},
    {"ShowCursor", 0, nullptr}, {"CreateAdditionalSwapChain", 0, nullptr},
    {"GetSwapChain", 0, nullptr}, {"GetNumberOfSwapChains", 0, nullptr},
    {"Reset", 2, dev_reset}, {"Present", 5, dev_present},
    {"GetBackBuffer", 0, nullptr}, {"GetRasterStatus", 0, nullptr},
    {"SetDialogBoxMode", 0, nullptr}, {"SetGammaRamp", 0, nullptr},
    {"GetGammaRamp", 0, nullptr}, {"CreateTexture", 9, dev_create_texture},
    {"CreateVolumeTexture", 0, nullptr}, {"CreateCubeTexture", 0, nullptr},
    {"CreateVertexBuffer", 7, dev_create_vertex_buffer},
    {"CreateIndexBuffer", 7, dev_create_index_buffer},
    {"CreateRenderTarget", 0, nullptr}, {"CreateDepthStencilSurface", 0, nullptr},
    {"UpdateSurface", 0, nullptr}, {"UpdateTexture", 0, nullptr},
    {"GetRenderTargetData", 0, nullptr}, {"GetFrontBufferData", 0, nullptr},
    {"StretchRect", 0, nullptr}, {"ColorFill", 0, nullptr},
    {"CreateOffscreenPlainSurface", 0, nullptr}, {"SetRenderTarget", 0, nullptr},
    {"GetRenderTarget", 0, nullptr}, {"SetDepthStencilSurface", 0, nullptr},
    {"GetDepthStencilSurface", 0, nullptr}, {"BeginScene", 1, ret_ok},
    {"EndScene", 1, ret_ok}, {"Clear", 7, dev_clear},
    {"SetTransform", 3, dev_set_transform}, {"GetTransform", 0, nullptr},
    {"MultiplyTransform", 0, nullptr}, {"SetViewport", 2, dev_set_viewport},
    {"GetViewport", 0, nullptr}, {"SetMaterial", 0, nullptr},
    {"GetMaterial", 0, nullptr}, {"SetLight", 0, nullptr},
    {"GetLight", 0, nullptr}, {"LightEnable", 0, nullptr},
    {"GetLightEnable", 0, nullptr}, {"SetClipPlane", 0, nullptr},
    {"GetClipPlane", 0, nullptr}, {"SetRenderState", 3, dev_set_render_state},
    {"GetRenderState", 0, nullptr}, {"CreateStateBlock", 0, nullptr},
    {"BeginStateBlock", 0, nullptr}, {"EndStateBlock", 0, nullptr},
    {"SetClipStatus", 0, nullptr}, {"GetClipStatus", 0, nullptr},
    {"GetTexture", 0, nullptr}, {"SetTexture", 3, dev_set_texture},
    {"GetTextureStageState", 0, nullptr}, {"SetTextureStageState", 0, nullptr},
    {"GetSamplerState", 0, nullptr}, {"SetSamplerState", 0, nullptr},
    {"ValidateDevice", 0, nullptr}, {"SetPaletteEntries", 0, nullptr},
    {"GetPaletteEntries", 0, nullptr}, {"SetCurrentTexturePalette", 0, nullptr},
    {"GetCurrentTexturePalette", 0, nullptr}, {"SetScissorRect", 0, nullptr},
    {"GetScissorRect", 0, nullptr}, {"SetSoftwareVertexProcessing", 0, nullptr},
    {"GetSoftwareVertexProcessing", 0, nullptr}, {"SetNPatchMode", 0, nullptr},
    {"GetNPatchMode", 0, nullptr}, {"DrawPrimitive", 4, dev_draw_primitive},
    {"DrawIndexedPrimitive", 7, dev_draw_indexed_primitive},
    {"DrawPrimitiveUP", 5, dev_draw_primitive_up},
    {"DrawIndexedPrimitiveUP", 0, nullptr}, {"ProcessVertices", 0, nullptr},
    {"CreateVertexDeclaration", 0, nullptr}, {"SetVertexDeclaration", 0, nullptr},
    {"GetVertexDeclaration", 0, nullptr}, {"SetFVF", 2, dev_set_fvf},
    {"GetFVF", 0, nullptr}, {"CreateVertexShader", 0, nullptr},
    {"SetVertexShader", 0, nullptr}, {"GetVertexShader", 0, nullptr},
    {"SetVertexShaderConstantF", 0, nullptr}, {"GetVertexShaderConstantF", 0, nullptr},
    {"SetVertexShaderConstantI", 0, nullptr}, {"GetVertexShaderConstantI", 0, nullptr},
    {"SetVertexShaderConstantB", 0, nullptr}, {"GetVertexShaderConstantB", 0, nullptr},
    {"SetStreamSource", 5, dev_set_stream_source}, {"GetStreamSource", 0, nullptr},
    {"SetStreamSourceFreq", 0, nullptr}, {"GetStreamSourceFreq", 0, nullptr},
    {"SetIndices", 2, dev_set_indices}, {"GetIndices", 0, nullptr},
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
    g_state.vp_x = g_state.vp_y = 0; // default viewport = full backbuffer
    g_state.vp_w = (float)g_state.width;
    g_state.vp_h = (float)g_state.height;
    seed_render_states(g_state.rs); // D3D default render states
    uint32_t dev = m.create_com_instance(g_state.device_vtbl, OBJ_STATE_BYTES);
    m.write32(dev + OBJ_REFCOUNT, 1);
    m.write32(ppOut, dev);
    m.ret(mm.nargs, 0);
}
// A single-adapter software enumeration surface: one adapter, one 1024x768 mode,
// every format/device check reported supported (S_OK). Enough for a game's D3D
// init to pick a device and reach CreateDevice.
void d3d9_get_adapter_count(Machine& m, const Method& mm) { m.ret(mm.nargs, 1); }
void d3d9_get_adapter_identifier(Machine& m, const Method& mm) {
    // GetAdapterIdentifier(this, Adapter, Flags, D3DADAPTER_IDENTIFIER9*).
    // sizeof(D3DADAPTER_IDENTIFIER9) = 1104 (8-aligned). Zeroed = empty driver/
    // description strings + null ids; WHQLLevel 1 = validated.
    uint32_t p = m.arg(3);
    if (p) {
        zero_guest(m, p, 1100);
        m.write32(p + 1096, 1); // WHQLLevel
    }
    m.ret(mm.nargs, 0);
}
void d3d9_get_adapter_mode_count(Machine& m, const Method& mm) { m.ret(mm.nargs, 1); }
void d3d9_enum_adapter_modes(Machine& m, const Method& mm) {
    // EnumAdapterModes(this, Adapter, Format, Mode, D3DDISPLAYMODE*).
    fill_display_mode(m, m.arg(4), 1024, 768, 60, m.arg(2));
    m.ret(mm.nargs, 0);
}
void d3d9_get_adapter_display_mode(Machine& m, const Method& mm) {
    // GetAdapterDisplayMode(this, Adapter, D3DDISPLAYMODE*). 22 = D3DFMT_X8R8G8B8.
    fill_display_mode(m, m.arg(2), 1024, 768, 60, 22);
    m.ret(mm.nargs, 0);
}
void d3d9_check_multisample(Machine& m, const Method& mm) {
    // CheckDeviceMultiSampleType(..., DWORD* pQualityLevels): one quality level.
    uint32_t pq = m.arg(6);
    if (pq) m.write32(pq, 1);
    m.ret(mm.nargs, 0);
}
void d3d9_get_adapter_monitor(Machine& m, const Method& mm) {
    m.ret(mm.nargs, 1); // a non-null fake HMONITOR
}
void d3d9_get_device_caps(Machine& m, const Method& mm) {
    // GetDeviceCaps(this, Adapter, DeviceType, D3DCAPS9*).
    fill_caps(m, m.arg(3), m.arg(2));
    m.ret(mm.nargs, 0);
}

constexpr Method kD3d9Vtbl[] = {
    {"QueryInterface", 3, com_query_interface}, {"AddRef", 1, com_addref},
    {"Release", 1, com_release}, {"RegisterSoftwareDevice", 0, nullptr},
    {"GetAdapterCount", 1, d3d9_get_adapter_count},
    {"GetAdapterIdentifier", 4, d3d9_get_adapter_identifier},
    {"GetAdapterModeCount", 3, d3d9_get_adapter_mode_count},
    {"EnumAdapterModes", 5, d3d9_enum_adapter_modes},
    {"GetAdapterDisplayMode", 3, d3d9_get_adapter_display_mode},
    {"CheckDeviceType", 6, ret_ok},
    {"CheckDeviceFormat", 7, ret_ok},
    {"CheckDeviceMultiSampleType", 7, d3d9_check_multisample},
    {"CheckDepthStencilMatch", 6, ret_ok},
    {"CheckDeviceFormatConversion", 5, ret_ok},
    {"GetDeviceCaps", 4, d3d9_get_device_caps},
    {"GetAdapterMonitor", 2, d3d9_get_adapter_monitor},
    {"CreateDevice", 7, d3d9_create_device},
};
static_assert(sizeof(kD3d9Vtbl) / sizeof(Method) == 17,
              "IDirect3D9 has 17 vtable entries");

// IDirect3DVertexBuffer9 (14 methods). Lock hands back a guest pointer into the
// backing store; Unlock is a no-op (identity-mapped, data already in memory).
constexpr Method kVBufferVtbl[] = {
    {"QueryInterface", 3, com_query_interface}, {"AddRef", 1, com_addref},
    {"Release", 1, com_release}, {"GetDevice", 0, nullptr},
    {"SetPrivateData", 0, nullptr}, {"GetPrivateData", 0, nullptr},
    {"FreePrivateData", 0, nullptr}, {"SetPriority", 0, nullptr},
    {"GetPriority", 0, nullptr}, {"PreLoad", 0, nullptr}, {"GetType", 0, nullptr},
    {"Lock", 5, vb_lock}, {"Unlock", 1, ret_ok}, {"GetDesc", 0, nullptr},
};
static_assert(sizeof(kVBufferVtbl) / sizeof(Method) == 14,
              "IDirect3DVertexBuffer9 has 14 vtable entries");

// IDirect3DTexture9 (22 methods; IDirect3DResource9 + BaseTexture prefix, then
// texture methods). LockRect hands back a guest pointer to the surface texels.
constexpr Method kTextureVtbl[] = {
    {"QueryInterface", 3, com_query_interface}, {"AddRef", 1, com_addref},
    {"Release", 1, com_release}, {"GetDevice", 0, nullptr},
    {"SetPrivateData", 0, nullptr}, {"GetPrivateData", 0, nullptr},
    {"FreePrivateData", 0, nullptr}, {"SetPriority", 0, nullptr},
    {"GetPriority", 0, nullptr}, {"PreLoad", 0, nullptr}, {"GetType", 0, nullptr},
    {"SetLOD", 0, nullptr}, {"GetLOD", 0, nullptr}, {"GetLevelCount", 0, nullptr},
    {"SetAutoGenFilterType", 0, nullptr}, {"GetAutoGenFilterType", 0, nullptr},
    {"GenerateMipSubLevels", 0, nullptr}, {"GetLevelDesc", 0, nullptr},
    {"GetSurfaceLevel", 0, nullptr}, {"LockRect", 5, tex_lock_rect},
    {"UnlockRect", 2, ret_ok}, {"AddDirtyRect", 0, nullptr},
};
static_assert(sizeof(kTextureVtbl) / sizeof(Method) == 22,
              "IDirect3DTexture9 has 22 vtable entries");

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
void vbuffer_method(Machine& m, unsigned i) {
    dispatch<kVBufferVtbl, 14>(m, i, "IDirect3DVertexBuffer9");
}
void texture_method(Machine& m, unsigned i) {
    dispatch<kTextureVtbl, 22>(m, i, "IDirect3DTexture9");
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
                g_state.vbuffer_vtbl = m.create_com_vtable(14, vbuffer_method);
                g_state.texture_vtbl = m.create_com_vtable(22, texture_method);
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
