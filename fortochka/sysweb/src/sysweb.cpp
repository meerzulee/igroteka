// sysweb — init-gate stub DLLs. See sysweb.h. Behavioral stubs only; no code
// from any real DLL is copied. Growth is stub-log-driven.
#include "sysweb/sysweb.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace sysweb {

using runtime::Machine;

namespace {

// wsock32 is imported by ORDINAL, so a real game's imports arrive as
// "ordinal#N"; a corpus binary linked against the mingw import lib arrives by
// NAME. Map the ordinals RTW uses to canonical names so both forms dispatch to
// one handler.
const char* wsock_ordinal(const std::string& name) {
    if (name.rfind("ordinal#", 0) != 0) return nullptr;
    int n = std::atoi(name.c_str() + 8);
    switch (n) {
        case 2: return "bind";        case 3: return "closesocket";
        case 8: return "htonl";       case 9: return "htons";
        case 10: return "inet_addr";  case 13: return "listen";
        case 14: return "ntohl";      case 15: return "ntohs";
        case 16: return "recv";       case 17: return "recvfrom";
        case 18: return "select";     case 20: return "sendto";
        case 21: return "setsockopt"; case 23: return "socket";
        case 52: return "gethostbyname"; case 57: return "gethostname";
        case 115: return "WSAStartup";   case 116: return "WSACleanup";
        default: return nullptr;
    }
}

// Per-interface fake Steam objects. Each getter (SteamClient/User/...) returns
// its own COM object; the vtable is 256 __thiscall thunks whose handler knows
// the interface, so a method whose semantics matter (a struct-return, a bool)
// is served correctly while everything else defaults to a safe 0. Method args
// arrive on the stack; `this` is in ECX (thiscall). Created lazily.
enum SteamIface { SC_CLIENT, SC_USER, SC_FRIENDS, SC_MATCHMAKING, SC_NETWORKING,
                  SC_GAMESERVER, SC_GSNETWORKING, SC_COUNT };
uint32_t g_steam_iface[SC_COUNT] = {};

void steam_method(Machine& m, int iface, unsigned method) {
    uint32_t sp = m.cpu().gpr[zhelezo::ESP];
    // ISteamUser::GetSteamID() — MSVC x86 struct return: the caller pushed a
    // hidden pointer to a CSteamID buffer (at [esp+4]); write a plausible
    // offline id there, return EAX = that buffer, and pop the hidden pointer.
    if (iface == SC_USER && method == 2) {
        uint32_t buf = m.read32(sp + 4);
        m.write32(buf, 0x00000001);     // account id = 1
        m.write32(buf + 4, 0x01100001); // universe=public, type=individual, inst=1
        m.ret(1, buf);
        return;
    }
    // ISteamClient slot 20 takes a callback pointer arg and its result is
    // ignored by RTW — pop the one arg, return 0.
    if (iface == SC_CLIENT && method == 20) { m.ret(1, 0); return; }
    // Unknown method: return 0, pop nothing (an ebp-framed caller heals any
    // stack drift via `leave`). Logged so the next boot wall names it.
    std::fprintf(stderr, "steam iface=%d vtbl[%u] -> 0 (default)\n", iface, method);
    m.ret(0, 0);
}

uint32_t steam_obj(Machine& m, int iface) {
    if (!g_steam_iface[iface]) {
        uint32_t vt = m.create_com_vtable(
            256, [iface](Machine& m, unsigned method) { steam_method(m, iface, method); });
        g_steam_iface[iface] = m.create_com_instance(vt, 8);
    }
    return g_steam_iface[iface];
}
// Map a flat-API getter name to its interface id.
int steam_iface_of(const std::string& name) {
    if (name == "SteamClient") return SC_CLIENT;
    if (name == "SteamUser") return SC_USER;
    if (name == "SteamFriends") return SC_FRIENDS;
    if (name == "SteamMatchmaking") return SC_MATCHMAKING;
    if (name == "SteamNetworking") return SC_NETWORKING;
    if (name == "SteamGameServer") return SC_GAMESERVER;
    if (name == "SteamGameServerNetworking") return SC_GSNETWORKING;
    return -1;
}

bool steam_api(Machine& m, const std::string& name) {
    // The Steamworks flat API is __cdecl: the CALLER cleans the stack, so every
    // handler pops NOTHING (m.ret(0, ...)). Getting this wrong corrupts the
    // guest stack. GOG's build runs without Steam, so Init/Restart both decline.
    if (name == "SteamAPI_RestartAppIfNecessary") { m.ret(0, 0); return true; } // don't relaunch
    if (name == "SteamAPI_Init" || name == "SteamGameServer_Init") {
        // The GOG steam_api.dll stub reports SUCCESS so the game runs without a
        // real Steam client (returning false triggers "Steam must be running").
        m.ret(0, 1); // true
        return true;
    }
    // Interface getters return a per-interface fake object (see steam_obj).
    if (int iface = steam_iface_of(name); iface >= 0) {
        // A 0-arg accessor: return the interface object, pop nothing (leave any
        // hidden struct-return buffer the caller pushed for the next method).
        m.ret(0, steam_obj(m, iface));
        return true;
    }
    // Shutdown / callbacks / (un)register: all no-op void or ignorable.
    static const char* kVoids[] = {
        "SteamAPI_Shutdown", "SteamAPI_RunCallbacks", "SteamAPI_RegisterCallback",
        "SteamAPI_UnregisterCallback", "SteamAPI_RegisterCallResult",
        "SteamAPI_UnregisterCallResult", "SteamGameServer_Shutdown",
        "SteamGameServer_RunCallbacks"};
    for (auto v : kVoids)
        if (name == v) { m.ret(0, 0); return true; }
    return false;
}

bool ole32(Machine& m, const std::string& name) {
    if (name == "CoInitialize") { m.ret(1, 0); return true; }   // S_OK
    if (name == "CoUninitialize") { m.ret(0, 0); return true; }
    if (name == "CoFreeUnusedLibraries") { m.ret(0, 0); return true; }
    if (name == "CoCreateInstance") {
        // (rclsid, pUnkOuter, dwClsContext, riid, ppv): decline — no class is
        // registered, so the game skips the optional COM object. *ppv = NULL.
        uint32_t ppv = m.arg(4);
        if (ppv) m.write32(ppv, 0);
        m.ret(5, 0x80040154); // REGDB_E_CLASSNOTREG
        return true;
    }
    if (name == "CoTaskMemAlloc") {
        uint32_t cb = m.arg(0);
        m.ret(1, cb ? m.alloc(cb) : 0);
        return true;
    }
    if (name == "CoTaskMemFree") { m.ret(1, 0); return true; } // immortal, no-op
    return false;
}

// Fake DirectInput8. COM (__stdcall, `this` is the first STACK arg, so
// m.arg(0)=this, m.arg(1..)=args, and m.ret(nargs,...) must pop the exact count
// incl. this). IDirectInput8::CreateDevice hands back a fake device; the device
// reports no input (zeroed state, no buffered data) so RTW's input init and
// polling succeed with a quiet keyboard/mouse.
const uint8_t kDI8Nargs[]  = {3,1,1,4,5,2,3,3,4,6,5};                    // IDirectInput8A
const uint8_t kDIDevNargs[] = {3,1,1,2,4,3,3,1,1,3,5,2,2,3,4,2,3,5,5,4,  // IDirectInputDevice8A
                               3,2,2,4,2,1,5,5,5,4,4,2};
uint32_t g_di8 = 0, g_didev = 0;

void di_device_method(Machine& m, unsigned method) {
    unsigned nargs = method < sizeof(kDIDevNargs) ? kDIDevNargs[method] : 1;
    if (method == 0) { // QueryInterface(this, riid, ppv) → same object
        if (m.arg(2)) m.write32(m.arg(2), m.arg(0));
        m.ret(nargs, 0);
        return;
    }
    if (method == 9) { // GetDeviceState(this, cbData, lpvData): all-zero state
        uint32_t cb = m.arg(1), buf = m.arg(2);
        for (uint32_t i = 0; i < cb && i < 4096; i += 4) m.write32(buf + i, 0);
        m.ret(nargs, 0);
        return;
    }
    if (method == 10) { // GetDeviceData(this, cb, rgdod, pdwInOut, flags): 0 events
        if (m.arg(3)) m.write32(m.arg(3), 0);
        m.ret(nargs, 0);
        return;
    }
    m.ret(nargs, 0); // DI_OK for everything else
}
uint32_t di_device(Machine& m) {
    if (!g_didev) {
        uint32_t vt = m.create_com_vtable(32, di_device_method);
        g_didev = m.create_com_instance(vt, 8);
    }
    return g_didev;
}
void di8_method(Machine& m, unsigned method) {
    unsigned nargs = method < sizeof(kDI8Nargs) ? kDI8Nargs[method] : 1;
    if (method == 0) { // QueryInterface
        if (m.arg(2)) m.write32(m.arg(2), m.arg(0));
        m.ret(nargs, 0);
        return;
    }
    if (method == 3) { // CreateDevice(this, rguid, lplpDevice, pUnkOuter)
        if (m.arg(2)) m.write32(m.arg(2), di_device(m));
        m.ret(nargs, 0); // DI_OK
        return;
    }
    m.ret(nargs, 0); // EnumDevices etc.: succeed, enumerate nothing
}
uint32_t di8(Machine& m) {
    if (!g_di8) {
        uint32_t vt = m.create_com_vtable(11, di8_method);
        g_di8 = m.create_com_instance(vt, 8);
    }
    return g_di8;
}

bool graphics_input(Machine& m, const std::string& dll, const std::string& name) {
    // d3d8.dll (Direct3DCreate8) is handled by d9web — RTW renders through D3D8
    // and d9web owns the D3D backend. Decline the remaining alternate stacks so
    // RTW uses the paths we implement; these return failure with a NULL out-ptr.
    if (dll == "ddraw.dll") {
        if (name == "DirectDrawCreateEx") { // (guid, lplpDD, iid, pUnkOuter)
            uint32_t pp = m.arg(1);
            if (pp) m.write32(pp, 0);
            m.ret(4, 0x8004005); // E_FAIL
            return true;
        }
        if (name == "DirectDrawEnumerateExA") { m.ret(3, 0); return true; } // no devices
    }
    if (dll == "dinput8.dll" && name == "DirectInput8Create") {
        // (hinst, version, riidltf, ppvOut, punkOuter) → a fake IDirectInput8 so
        // RTW's input init succeeds; the devices report no input.
        if (m.arg(3)) m.write32(m.arg(3), di8(m));
        m.ret(5, 0); // DI_OK
        return true;
    }
    if (dll == "msvfw32.dll") { // video-for-windows drawing: pretend success
        if (name == "DrawDibOpen") { m.ret(0, 0x0DD00001); return true; } // fake HDRAWDIB
        if (name == "DrawDibClose") { m.ret(1, 1); return true; }
        if (name == "DrawDibDraw") { m.ret(13, 1); return true; }
    }
    return false;
}

bool wsock32(Machine& m, const std::string& canon) {
    // Winsock is __stdcall (WSAAPI). No real networking in tier 0: byte-order
    // helpers do real work; everything else declines cleanly so a game's net
    // init completes but no connection is made.
    constexpr uint32_t INVALID_SOCKET = 0xFFFFFFFFu, SOCKET_ERROR = 0xFFFFFFFFu;
    if (canon == "htonl" || canon == "ntohl") {
        uint32_t v = m.arg(0);
        m.ret(1, __builtin_bswap32(v)); // guest is little-endian → network order
        return true;
    }
    if (canon == "htons" || canon == "ntohs") {
        uint32_t v = m.arg(0) & 0xFFFF;
        m.ret(1, (uint32_t)(uint16_t)__builtin_bswap16((uint16_t)v));
        return true;
    }
    if (canon == "inet_addr") { m.ret(1, 0xFFFFFFFFu); return true; } // INADDR_NONE
    if (canon == "WSAStartup") {
        // (wVersionRequested, lpWSAData): succeed with a WSADATA claiming 2.2.
        uint32_t p = m.arg(1);
        if (p) {
            m.write32(p + 0, 0x00020202); // wVersion=2.2, wHighVersion=2.2
            for (uint32_t off = 4; off < 400; off += 4) m.write32(p + off, 0);
        }
        m.ret(2, 0); // success
        return true;
    }
    if (canon == "WSACleanup") { m.ret(0, 0); return true; }
    if (canon == "gethostname") { // (name, namelen): a fixed host name
        uint32_t buf = m.arg(0), len = m.arg(1);
        const char* h = "fortochka";
        for (uint32_t i = 0; h[i] && i + 1 < len; i++) m.write32(buf + i, h[i]);
        if (len) m.write32(buf, 'f'); // ensure first byte if room
        m.ret(2, 0);
        return true;
    }
    if (canon == "gethostbyname") { m.ret(1, 0); return true; } // null: not found
    if (canon == "socket") { m.ret(3, INVALID_SOCKET); return true; }
    if (canon == "closesocket") { m.ret(1, 0); return true; }
    if (canon == "bind") { m.ret(3, SOCKET_ERROR); return true; }
    if (canon == "listen") { m.ret(2, SOCKET_ERROR); return true; }
    if (canon == "recv" || canon == "setsockopt" || canon == "select") {
        m.ret(canon == "recv" ? 4 : 5, SOCKET_ERROR);
        return true;
    }
    if (canon == "recvfrom" || canon == "sendto") { m.ret(6, SOCKET_ERROR); return true; }
    return false;
}

// mss32 (Miles Sound System) — audio-off stubs. Every export is __stdcall with
// the decorated name _AIL_xxx@N, where N is the argument byte count, so
// nargs = N/4 comes straight off the @N decoration. Opens return a fixed fake
// handle so RTW believes audio initialized; status queries report "done" so
// the game never waits on a sound finishing; everything else returns 0.
int decorated_nargs(const std::string& name) {
    size_t at = name.rfind('@');
    if (at == std::string::npos) return 0;
    return std::atoi(name.c_str() + at + 1) / 4;
}

bool mss32(Machine& m, const std::string& name) {
    if (name.rfind("_AIL_", 0) != 0) return false;
    // _AIL_enumerate_3D_providers(HPROENUM* next, HPROVIDER* dest, C8** name):
    // RTW treats 3D-audio-init failure as FATAL (main returns after "Failed to
    // initialise 3D audio!"). It enumerates 3D providers, then requires the one
    // named "Miles Fast 2D Positional Audio" (the Miles software positional
    // provider, present on every real install). Yield exactly that one provider
    // so the init finds and selects it. The iterator convention: *next starts at
    // AIL_3D_PROVIDER_START (0), each nonzero return fills *dest/*name and
    // advances *next; a 0 return ends enumeration.
    if (name == "_AIL_enumerate_3D_providers@12") {
        static uint32_t name_va = 0;
        if (!name_va) {
            const char* pn = "Miles Fast 2D Positional Audio";
            uint32_t n = (uint32_t)std::strlen(pn) + 1;
            name_va = m.alloc(n);
            std::memcpy(m.mem() + name_va, pn, n);
        }
        uint32_t next_p = m.arg(0), dest_p = m.arg(1), name_p = m.arg(2);
        if (m.read32(next_p) == 0) {
            m.write32(dest_p, 0x0A1D3D01u); // fake HPROVIDER
            m.write32(name_p, name_va);     // provider name string
            m.write32(next_p, 1);           // advance cursor
            m.ret(3, 1);                    // one provider available
        } else {
            m.ret(3, 0);                    // enumeration complete
        }
        return true;
    }
    // _AIL_init_3D_sample_handle(provider, ...): RTW allocates 3D voices in a
    // "while (handle != 0) alloc" loop — it keeps requesting voices until the
    // provider is exhausted. A stub that always returns a nonzero handle makes
    // that loop run forever (each iteration also grows a voice tree, so it spins
    // billions of instructions). Hand out a finite pool (a real Miles software
    // provider offers a few dozen 3D voices), then 0 to end the loop.
    if (name == "_AIL_init_3D_sample_handle@12") {
        static uint32_t voices = 0;
        m.ret(3, voices < 32 ? 0x0A1D3D00u + (++voices) : 0);
        return true;
    }
    // Handle-returning opens (nonzero = valid handle). NOTE: _AIL_open_3D_provider
    // is NOT here — it returns an M3DRESULT status where 0 == M3D_NOERR (success),
    // so it falls through to the default 0 return. Returning a fake "handle" there
    // reads as an error code and makes RTW abort 3D-audio init (fatal).
    static const char* kHandles[] = {
        "_AIL_allocate_sample_handle@4",
        "_AIL_init_sample@4", "_AIL_init_stream@16", "_AIL_open_3D_listener@4",
        "_AIL_open_digital_driver@16",
        "_AIL_open_filter@8", "_AIL_startup@0"};
    static const char* kStatus[] = {
        "_AIL_3D_sample_status@4", "_AIL_sample_status@4", "_AIL_stream_status@4"};
    uint32_t retval = 0;
    for (auto h : kHandles)
        if (name == h) { retval = 0x0A1D0001u; break; }
    if (retval == 0)
        for (auto s : kStatus)
            if (name == s) { retval = 2; break; }
    m.ret(decorated_nargs(name), retval);
    return true;
}

} // namespace

void install(Machine& m) {
    m.add_handler([](Machine& m, const std::string& dll,
                     const std::string& name) -> bool {
        if (dll == "steam_api.dll") return steam_api(m, name);
        if (dll == "ole32.dll") return ole32(m, name);
        if (dll == "d3d8.dll" || dll == "ddraw.dll" || dll == "dinput8.dll" ||
            dll == "msvfw32.dll")
            return graphics_input(m, dll, name);
        if (dll == "wsock32.dll" || dll == "ws2_32.dll") {
            const char* ord = wsock_ordinal(name);
            return wsock32(m, ord ? ord : name);
        }
        if (dll == "mss32.dll") return mss32(m, name);
        return false;
    });
}

} // namespace sysweb
