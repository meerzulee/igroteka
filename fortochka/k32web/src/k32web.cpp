#include "k32web/k32web.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <dirent.h>   // host-mount directory enumeration (native boot only)
#include <sys/stat.h>

namespace k32web {

using runtime::Machine;

namespace {
// Windows std-handle pseudo-values; any distinct nonzero constants work.
constexpr uint32_t H_STDOUT = 0x12, H_STDERR = 0x13, H_STDIN = 0x11;

// Host-side process state for the CRT-startup kernel32 surface (single guest
// thread in tier 0). Reset per install().
struct K32 {
    uint32_t last_error = 0;
    uint32_t tls[64] = {};   // Tls{Alloc,Get,Set,Free} slots
    bool tls_used[64] = {};
    uint32_t cmdline = 0;    // cached GetCommandLineA guest string
    uint32_t env_a = 0, env_w = 0; // cached GetEnvironmentStrings(A/W) blocks
    uint32_t heap_handle = 0x00E70001; // one process heap pseudo-handle
    // Interned DLL names for LoadLibrary/GetModuleHandle; the handle is
    // MODULE_TAG + table index, so GetProcAddress can scope a lookup to the
    // module the guest actually asked about.
    std::vector<std::string> modules;
    // In-memory VFS: files a game reads/writes. In the browser this is backed
    // by OPFS; the native harness keeps it in host memory and a guest can
    // populate it itself (write then read back). Paths normalized lowercase.
    struct VFile {
        std::string name;
        std::vector<uint8_t> data;
    };
    std::vector<VFile> vfs;
    // Open file handles: handle = FILE_TAG + index; index into open_files.
    // vfs_index < 0 marks a closed slot (never index-reused in tier 0).
    struct OpenFile {
        int vfs_index = -1;
        uint64_t pos = 0;
    };
    std::vector<OpenFile> open_files;
    // Current directory (normalized, no trailing slash) and directory-search
    // handles. FindFirstFile snapshots the matching names; FindNext walks them.
    std::string cwd = "c:/rtw";
    struct FindEntry {
        std::string name; // basename only
        bool is_dir;
        uint64_t size;
    };
    struct FindState {
        std::vector<FindEntry> entries;
        size_t pos = 0;
    };
    std::vector<FindState> finds; // handle = FIND_TAG + index
    // Registry shim (advapi32). Values are exact-match (key path + value name,
    // both normalized lowercase); everything else is "not found", which pushes
    // an era game onto its relative-path fallback. Misses are logged so a boot
    // log says exactly which keys to seed.
    struct RegValue {
        std::string key, name;   // normalized
        uint32_t type = 1;       // REG_SZ=1, REG_DWORD=4
        std::vector<uint8_t> data;
    };
    std::vector<RegValue> reg;
    std::vector<std::string> reg_open;    // open-key handles: KEY_TAG + index
    std::vector<std::string> reg_created; // keys made by RegCreateKeyExA
    // File mappings (CreateFileMappingA): the source VFS file (-1 = anonymous)
    // and the mapping size. A view is a guest alloc holding a COPY of the file
    // bytes (identity-mapped, read-mostly; no writeback in tier 0).
    struct Mapping {
        int vfs_index = -1;
        uint32_t size = 0;
    };
    std::vector<Mapping> mappings; // handle = MAP_TAG + index
    // Host-directory mount (native boot): a guest path under `mount_prefix`
    // that misses the in-memory VFS is lazily read from the host filesystem.
    std::string mount_prefix, mount_root;
};
K32 g_k;

// Handle tag for open files, distinct from the module tag and every pseudo-
// handle the runtime hands out. Index stays small, so no range overlap.
constexpr uint32_t FILE_TAG = 0x50000000u;
constexpr uint32_t INVALID_HANDLE = 0xFFFFFFFFu;
// Cap a single file's size and a single read/write span so a guest can't force
// an unbounded host allocation or copy. Also cap the file and open-handle counts
// so a guest looping CreateFileA over distinct names can't grow host memory
// without bound (tier-0 never reclaims slots).
constexpr uint32_t MAX_FILE_BYTES = 512u << 20; // real game packs run large
constexpr size_t MAX_FILES = 1u << 16;          // a game touches thousands
constexpr size_t MAX_OPEN_FILES = 1u << 16;
// Cap string-conversion element counts so a huge guest-supplied length can't
// drive a multi-billion-iteration host loop (the HLE runs outside the CPU step
// budget). No real string exceeds the arena; 64 Mi chars is far above any.
constexpr uint32_t MAX_CONV_CHARS = 64u << 20;
// Directory-search handle tag, distinct from FILE_TAG and MODULE_TAG.
constexpr uint32_t FIND_TAG = 0x60000000u;
constexpr size_t MAX_FINDS = 4096;
// Registry open-key handle tag + growth caps (values/handles never reclaimed
// in tier 0, so bound them). Note KEY_TAG stays below the predefined roots
// (0x80000000+) — a root HKEY must never index reg_open.
constexpr uint32_t KEY_TAG = 0x70000000u;
constexpr size_t MAX_REG_VALUES = 4096, MAX_REG_OPEN = 4096, MAX_REG_KEYS = 4096;
constexpr uint32_t MAX_REG_DATA = 64u << 10; // one value's data cap
// File-mapping handle tag: between FILE_TAG and FIND_TAG, own index space.
constexpr uint32_t MAP_TAG = 0x58000000u;
constexpr size_t MAX_MAPPINGS = 4096;

// Civil-date → days since the FILETIME epoch (1601-01-01). Standard
// days-from-civil (Howard Hinnant), rebased from 0000-03-01 to 1601-01-01.
int64_t days_from_1601(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days_epoch = era * 146097 + (int64_t)doe - 719468; // days since 1970-01-01
    return days_epoch + 134774;                                // 1601→1970 offset
}
// Inverse: days since 1601 → civil date.
void civil_from_1601(int64_t z, int& y, int& m, int& d) {
    z -= 134774; // to days since 1970
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp + (mp < 10 ? 3 : -9));
    y = (int)(yy + (m <= 2));
}

// Normalize a VFS path: lowercase, backslashes → forward slashes.
std::string norm_path(std::string s) {
    for (auto& c : s) {
        c = (char)std::tolower((unsigned char)c);
        if (c == '\\') c = '/';
    }
    return s;
}
// Strip a trailing '/' (except a lone root) so "data/" and "data" compare equal.
std::string strip_slash(std::string s) {
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}
// Match a filename against a DOS wildcard pattern (already normalized lowercase).
// Supports '*' (any run) and '?' (any one char). Iterative backtracking.
bool glob_match(const std::string& pat, const std::string& name) {
    size_t p = 0, n = 0, star = std::string::npos, mark = 0;
    while (n < name.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == name[n])) { p++; n++; }
        else if (p < pat.size() && pat[p] == '*') { star = p++; mark = n; }
        else if (star != std::string::npos) { p = star + 1; n = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') p++;
    return p == pat.size();
}
int vfs_find(const std::string& name) {
    std::string n = norm_path(name);
    for (size_t i = 0; i < g_k.vfs.size(); i++)
        if (g_k.vfs[i].name == n) return (int)i;
    return -1;
}
int vfs_create(const std::string& name) {
    if (g_k.vfs.size() >= MAX_FILES) return -1; // cap reached
    g_k.vfs.push_back({norm_path(name), {}});
    return (int)g_k.vfs.size() - 1;
}
// Map a normalized guest path to its host path if it lies under the mount.
std::string host_path_for(const std::string& norm) {
    if (g_k.mount_root.empty()) return {};
    const std::string& pre = g_k.mount_prefix;
    if (norm.size() < pre.size() || norm.compare(0, pre.size(), pre) != 0) return {};
    std::string rest = norm.substr(pre.size());
    while (!rest.empty() && rest[0] == '/') rest.erase(0, 1);
    return rest.empty() ? g_k.mount_root : g_k.mount_root + "/" + rest;
}
// Lazily load a host-backed file into the VFS on first access; return its index
// (or -1 if not a regular file under the mount). One log line per real load.
int host_load(const std::string& norm) {
    std::string hp = host_path_for(norm);
    if (hp.empty()) return -1;
    struct stat st;
    if (stat(hp.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    if (g_k.vfs.size() >= MAX_FILES) return -1;
    FILE* f = std::fopen(hp.c_str(), "rb");
    if (!f) return -1;
    g_k.vfs.push_back({norm, {}});
    int idx = (int)g_k.vfs.size() - 1;
    auto& data = g_k.vfs[idx].data;
    data.resize((size_t)st.st_size);
    if (st.st_size) {
        size_t got = std::fread(data.data(), 1, (size_t)st.st_size, f);
        data.resize(got);
    }
    std::fclose(f);
    std::fprintf(stderr, "vfs: host-load %s (%zu bytes)\n", norm.c_str(),
                 data.size());
    return idx;
}
// vfs_find, then a host-mount fallback (open / attribute / read paths).
int vfs_find_host(const std::string& name) {
    std::string n = norm_path(name);
    int i = vfs_find(n);
    return i >= 0 ? i : host_load(n);
}
// Open a VFS file index as a new handle, or INVALID_HANDLE at the handle cap.
uint32_t open_file_handle(int vfs_index) {
    if (vfs_index < 0 || g_k.open_files.size() >= MAX_OPEN_FILES)
        return INVALID_HANDLE;
    g_k.open_files.push_back({vfs_index, 0});
    return FILE_TAG + (uint32_t)(g_k.open_files.size() - 1);
}
// If `h` is a live file handle, return its OpenFile*, else nullptr.
K32::OpenFile* as_open_file(uint32_t h) {
    if (h < FILE_TAG || (h - FILE_TAG) >= g_k.open_files.size()) return nullptr;
    K32::OpenFile* of = &g_k.open_files[h - FILE_TAG];
    return of->vfs_index >= 0 ? of : nullptr;
}
uint32_t guest_write_bytes(Machine& m, uint32_t dst, const uint8_t* src, uint32_t n); // fwd

// Resolve a guest path against the cwd: a drive-qualified ("c:/...") or
// root-anchored ("/...") path stays as-is; a relative one is joined to the cwd.
// "." and ".." segments are normalized out ('..' never pops past the first
// segment, i.e. the drive), so SetCurrentDirectory("..") and "..\\data" paths
// canonicalize to the same VFS keys as their absolute forms (Codex finding).
std::string resolve_path(const std::string& raw) {
    std::string p = norm_path(raw);
    if (!p.empty() && p[0] == '/') {
        // Root-anchored: anchor to the cwd's drive ("\\data" → "c:/data"),
        // not the cwd itself — and keep the drive segment through the rejoin
        // below (a bare leading '/' would be dropped as an empty segment).
        std::string drive = g_k.cwd.substr(0, g_k.cwd.find('/'));
        p = drive + p;
    } else if (!(p.size() >= 2 && p[1] == ':')) {
        p = g_k.cwd + "/" + p; // relative: join to the cwd
    }
    std::vector<std::string> segs;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string seg = p.substr(i, j - i);
        if (seg == "..") { if (segs.size() > 1) segs.pop_back(); }
        else if (!seg.empty() && seg != ".") segs.push_back(seg);
        i = j + 1;
    }
    std::string out;
    for (size_t k = 0; k < segs.size(); k++) {
        if (k) out += '/';
        out += segs[k];
    }
    return out;
}
// Populate a FindState with the immediate children of the directory named by a
// wildcard pattern (e.g. "c:/rtw/data/*"): direct file children matching the
// last-segment glob, plus each distinct first-level subdirectory (synthesized
// from the flat VFS paths). Bounded by the VFS size.
void build_find(K32::FindState& fs, const std::string& pattern_raw) {
    std::string pat = resolve_path(pattern_raw);
    size_t slash = pat.find_last_of('/');
    std::string dir = slash == std::string::npos ? std::string() : pat.substr(0, slash);
    std::string glob = slash == std::string::npos ? pat : pat.substr(slash + 1);
    std::string prefix = dir + "/";
    std::vector<std::string> dirs_seen;
    for (const auto& vf : g_k.vfs) {
        if (vf.name.size() <= prefix.size() ||
            vf.name.compare(0, prefix.size(), prefix) != 0)
            continue;
        std::string rest = vf.name.substr(prefix.size());
        size_t sub = rest.find('/');
        if (sub == std::string::npos) { // a direct file child
            if (glob_match(glob, rest))
                fs.entries.push_back({rest, false, vf.data.size()});
        } else { // a subdirectory child (dedup)
            std::string d = rest.substr(0, sub);
            bool seen = false;
            for (const auto& s : dirs_seen)
                if (s == d) { seen = true; break; }
            if (!seen && glob_match(glob, d)) {
                dirs_seen.push_back(d);
                fs.entries.push_back({d, true, 0});
            }
        }
    }
    // Host mount: also enumerate the real directory, skipping names already
    // surfaced from the in-memory VFS (dedup by basename).
    std::string hdir = host_path_for(dir);
    if (hdir.empty()) return;
    DIR* dp = opendir(hdir.c_str());
    if (!dp) return;
    auto already = [&](const std::string& nm) {
        for (const auto& e : fs.entries)
            if (e.name == nm) return true;
        return false;
    };
    while (struct dirent* de = readdir(dp)) {
        std::string nm = de->d_name;
        if (nm == "." || nm == "..") continue;
        std::string lower = norm_path(nm);
        if (!glob_match(glob, lower) || already(lower)) continue;
        struct stat st;
        std::string full = hdir + "/" + nm;
        bool is_dir = stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        uint64_t sz = is_dir ? 0 : (uint64_t)st.st_size;
        fs.entries.push_back({lower, is_dir, sz});
    }
    closedir(dp);
}
// Fill a WIN32_FIND_DATAA (320 bytes) from a FindEntry. All writes bounds-checked.
void fill_find_data_a(Machine& m, uint32_t p, const K32::FindEntry& e) {
    for (uint32_t off = 0; off < 320; off += 4) m.write32(p + off, 0);
    m.write32(p + 0, e.is_dir ? 0x10 : 0x80); // FILE_ATTRIBUTE_DIRECTORY : NORMAL
    m.write32(p + 28, (uint32_t)(e.size >> 32)); // nFileSizeHigh
    m.write32(p + 32, (uint32_t)e.size);         // nFileSizeLow
    uint32_t n = e.name.size() < 259 ? (uint32_t)e.name.size() : 259;
    guest_write_bytes(m, p + 44, (const uint8_t*)e.name.data(), n); // cFileName[260]
}

// Open/create a VFS file per a CreateFile disposition (shared by CreateFileA/W).
// Path is resolved against the cwd so relative and absolute forms canonicalize
// to the same VFS key. Returns the guest HANDLE or INVALID_HANDLE.
uint32_t open_vfs(const std::string& path_raw, uint32_t disp) {
    std::string path = resolve_path(path_raw);
    // For dispositions that read an existing file, fall back to the host mount.
    int idx = (disp == 3 || disp == 5) ? vfs_find_host(path) : vfs_find(path);
    bool exists = idx >= 0;
    bool open_it = false, create = false, trunc = false;
    switch (disp) {
        case 1: /*CREATE_NEW*/        open_it = create = !exists; break;
        case 2: /*CREATE_ALWAYS*/     open_it = true; create = !exists; trunc = true; break;
        case 3: /*OPEN_EXISTING*/     open_it = exists; break;
        case 4: /*OPEN_ALWAYS*/       open_it = true; create = !exists; break;
        case 5: /*TRUNCATE_EXISTING*/ open_it = trunc = exists; break;
        default:                      open_it = exists; break;
    }
    uint32_t handle = INVALID_HANDLE;
    if (open_it) {
        if (create) idx = vfs_create(path);
        if (idx >= 0) {
            if (trunc) g_k.vfs[idx].data.clear();
            handle = open_file_handle(idx);
        }
    }
    if (handle == INVALID_HANDLE) g_k.last_error = 2; // ERROR_FILE_NOT_FOUND
    return handle;
}
// ---- registry helpers ----
// Map an HKEY (predefined root or an open-key handle) to its normalized path.
// Empty string = unknown/invalid handle.
std::string reg_key_path(uint32_t h) {
    switch (h) {
        case 0x80000000u: return "hkcr";
        case 0x80000001u: return "hkcu";
        case 0x80000002u: return "hklm";
        case 0x80000003u: return "hku";
        case 0x80000005u: return "hkcc";
        default: break;
    }
    if (h >= KEY_TAG && h < 0x80000000u && (h - KEY_TAG) < g_k.reg_open.size())
        return g_k.reg_open[h - KEY_TAG];
    return std::string();
}
// Join an HKEY base with a subkey path (normalized, empty subkey = the base).
std::string reg_join(uint32_t h, const std::string& sub_raw) {
    std::string base = reg_key_path(h);
    if (base.empty()) return base;
    std::string sub = strip_slash(norm_path(sub_raw));
    return sub.empty() ? base : base + "/" + sub;
}
// A key "exists" if any stored value or created key sits at it or under it
// (intermediate keys exist implicitly, as on Windows).
bool reg_key_exists(const std::string& path) {
    if (path.empty()) return false;
    auto covers = [&](const std::string& k) {
        return k == path ||
               (k.size() > path.size() && k.compare(0, path.size(), path) == 0 &&
                k[path.size()] == '/');
    };
    for (const auto& v : g_k.reg) if (covers(v.key)) return true;
    for (const auto& k : g_k.reg_created) if (covers(k)) return true;
    // The predefined roots themselves always exist.
    return path == "hkcr" || path == "hkcu" || path == "hklm" ||
           path == "hku" || path == "hkcc";
}
// Open-key handle for a path, or 0 at the cap (caller maps to an error).
uint32_t reg_open_handle(const std::string& path) {
    if (g_k.reg_open.size() >= MAX_REG_OPEN) return 0;
    g_k.reg_open.push_back(path);
    return KEY_TAG + (uint32_t)(g_k.reg_open.size() - 1);
}
K32::RegValue* reg_find(const std::string& key, const std::string& name) {
    for (auto& v : g_k.reg)
        if (v.key == key && v.name == name) return &v;
    return nullptr;
}

// File attributes for a resolved path: NORMAL(0x80) if a file exists, DIRECTORY
// (0x10) if any VFS entry lives under it, else INVALID_FILE_ATTRIBUTES.
uint32_t vfs_attrs(const std::string& path_raw) {
    std::string path = resolve_path(path_raw);
    if (vfs_find(path) >= 0) return 0x80;
    std::string prefix = path + "/";
    for (const auto& vf : g_k.vfs)
        if (vf.name.size() > prefix.size() &&
            vf.name.compare(0, prefix.size(), prefix) == 0)
            return 0x10;
    // Host mount: is it a real file or directory on disk?
    std::string hp = host_path_for(path);
    if (!hp.empty()) {
        struct stat st;
        if (stat(hp.c_str(), &st) == 0)
            return S_ISDIR(st.st_mode) ? 0x10 : 0x80;
    }
    return INVALID_HANDLE;
}
// Copy `n` bytes from host `src` into guest memory at `dst`, bounds-checked per
// byte against the arena (like write32) so a bogus dst is a no-op, not a host
// out-of-bounds write. Returns bytes actually written in-range.
uint32_t guest_write_bytes(Machine& m, uint32_t dst, const uint8_t* src, uint32_t n) {
    uint8_t* base = m.mem();
    uint32_t sz = m.mem_size();
    uint32_t wrote = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t d = (uint64_t)dst + i;
        if (d >= sz) break;
        base[d] = src[i];
        wrote++;
    }
    return wrote;
}

// Synthetic module-handle base. Distinct from the image base (0x400000); index
// stays small so no overlap with the FILE_TAG range or high pseudo-handles.
constexpr uint32_t MODULE_TAG = 0x40000000u;
// Cap the intern table so a guest looping LoadLibrary over distinct names can't
// grow host memory without bound; past the cap we hand back the image base,
// which scopes GetProcAddress to name-only (safe, just unscoped).
constexpr size_t MAX_MODULES = 4096;

// Read a guest UTF-16LE string as narrow ASCII (DLL names/paths are ASCII).
// read32 is bounds-checked (0 out of range), so a bogus pointer just terminates.
std::string read_wstr_narrow(Machine& m, uint32_t p, uint32_t max = 4096) {
    std::string s;
    for (uint32_t i = 0; i < max; i++) {
        uint32_t w = m.read32(p + i * 2) & 0xFFFF; // low 16 bits = the wchar
        if (w == 0) break;
        s.push_back((char)(w & 0xFF));
    }
    return s;
}
// Write a 16-bit value into guest memory (bounds-checked per byte via the copy
// helper above), used to emit UTF-16LE.
inline void guest_write16(Machine& m, uint32_t dst, uint16_t v) {
    uint8_t b[2] = {(uint8_t)(v & 0xFF), (uint8_t)(v >> 8)};
    guest_write_bytes(m, dst, b, 2);
}

// Intern a DLL name (normalized: lowercase, no trailing ".dll") → module handle.
uint32_t module_handle(const std::string& raw) {
    std::string name = raw;
    for (auto& c : name) c = (char)std::tolower((unsigned char)c);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".dll") == 0)
        name.resize(name.size() - 4);
    for (size_t i = 0; i < g_k.modules.size(); i++)
        if (g_k.modules[i] == name) return MODULE_TAG + (uint32_t)i;
    if (g_k.modules.size() >= MAX_MODULES) return 0x00400000; // cap: name-only scope
    g_k.modules.push_back(name);
    return MODULE_TAG + (uint32_t)(g_k.modules.size() - 1);
}

// Write a NUL-terminated string into freshly-allocated guest memory, return VA.
uint32_t guest_strdup(Machine& m, const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    uint32_t p = m.alloc(n + 1);
    for (uint32_t i = 0; i <= n; i++) m.mem()[p + i] = (uint8_t)s[i];
    return p;
}

// EXCEPTION_DISPOSITION
constexpr uint32_t ExceptionContinueExecution = 0;
constexpr uint32_t ExceptionContinueSearch = 1;
constexpr uint32_t EXCEPTION_NONCONTINUABLE = 0x1;

constexpr uint32_t EXC_RECORD_SIZE = 0x50;  // { code,flags,*rec,addr,n,info[15] }
constexpr uint32_t CONTEXT_SIZE = 0x2CC;    // i386 CONTEXT
constexpr uint32_t CTX_FLAGS = 0x00010007;  // CONTEXT_i386|INTEGER|CONTROL

// Fill an i386 CONTEXT at guest address `ctx` from the current CPU state — so a
// handler reading ContextRecord->Esp/Ebp/Eip/registers sees sane values. This
// is the state at the exception point (for RaiseException, the call site).
void write_context(Machine& m, uint32_t ctx, uint32_t except_addr) {
    for (uint32_t i = 0; i < CONTEXT_SIZE; i += 4) m.write32(ctx + i, 0);
    const auto& c = m.cpu();
    m.write32(ctx + 0x00, CTX_FLAGS);
    m.write32(ctx + 0x9C, c.gpr[zhelezo::EDI]);
    m.write32(ctx + 0xA0, c.gpr[zhelezo::ESI]);
    m.write32(ctx + 0xA4, c.gpr[zhelezo::EBX]);
    m.write32(ctx + 0xA8, c.gpr[zhelezo::EDX]);
    m.write32(ctx + 0xAC, c.gpr[zhelezo::ECX]);
    m.write32(ctx + 0xB0, c.gpr[zhelezo::EAX]);
    m.write32(ctx + 0xB4, c.gpr[zhelezo::EBP]);
    m.write32(ctx + 0xB8, except_addr);      // Eip
    m.write32(ctx + 0xC0, c.eflags);         // EFlags
    m.write32(ctx + 0xC4, c.gpr[zhelezo::ESP]); // Esp
}

// Dispatch a synthetic exception through the guest's fs:[0] SEH chain, reverse-
// thunking each registered handler with (record, establisher, context,
// dispatcher). The EXCEPTION_RECORD and CONTEXT live on the guest stack below
// the current ESP — one frame per dispatch, so a handler that raises a nested
// exception cannot clobber the outer record. Honors dispositions:
// ContinueExecution stops the walk (caller resumes), ContinueSearch advances;
// anything else is an invalid disposition. The chain must climb strictly toward
// higher stack addresses (real SEH's frame-order rule), which also rejects
// cyclic/corrupt chains. Returns true if handled, false if the chain was
// exhausted unhandled.
//
// Reference: ReactOS/Wine RtlDispatchException — walk, ordering, and disposition
// semantics; read, not copied.
//
// KNOWN GAPS (need feature work, not fixes here):
//  - Fault→SEH: dispatching a real CPU fault must first snapshot ALL guest
//    registers into CONTEXT and, on ContinueExecution, restore the (possibly
//    handler-edited) CONTEXT — call_guest only preserves ESP/EIP, so the fault
//    path will need its own register save/apply around this call.
//  - RtlUnwind / __except transfer: a handler that unwinds sets guest ESP/EIP
//    to an __except block instead of resuming at the exception point. That is
//    incompatible with call_guest's unconditional ESP/EIP restore; it needs a
//    distinct "transfer, do not return to dispatcher" path.
//  - CONTEXT here reflects the RaiseException call site only (integer+control),
//    not FP/debug/extended state.
bool dispatch_seh(Machine& m, uint32_t code, uint32_t flags, uint32_t except_addr,
                  uint32_t nparams, uint32_t params_ptr) {
    const uint32_t saved_esp = m.cpu().gpr[zhelezo::ESP];
    // Reserve record + context on the guest stack; run handlers below them.
    const uint32_t rec = (saved_esp - EXC_RECORD_SIZE) & ~0xFu;
    const uint32_t ctx = (rec - CONTEXT_SIZE) & ~0xFu;
    if (ctx < 0x2000 || saved_esp >= m.mem_size()) // sanity: stack sane?
        throw runtime::MachineError{"SEH dispatch: stack pointer out of range"};

    // EXCEPTION_RECORD { code, flags, *nested, address, nparams, info[15] }.
    m.write32(rec + 0, code);
    m.write32(rec + 4, flags);
    m.write32(rec + 8, 0);
    m.write32(rec + 12, except_addr);
    uint32_t n = nparams > 15 ? 15 : nparams;
    m.write32(rec + 16, n);
    for (uint32_t i = 0; i < n; i++)
        m.write32(rec + 20 + 4 * i, params_ptr ? m.read32(params_ptr + 4 * i) : 0);
    write_context(m, ctx, except_addr);

    // Handlers push below the reserved region; restore ESP when done.
    m.cpu().gpr[zhelezo::ESP] = ctx - 0x20;

    const uint32_t stack_base = m.tib_addr(); // records live below the TIB/stack top
    uint32_t node = m.read32(m.cpu().fs_base); // fs:[0] = ExceptionList head
    uint32_t prev = 0; // enforce strictly increasing addresses (no cycles)
    bool handled = false;
    for (unsigned steps = 0; node != Machine::SEH_CHAIN_END && node != 0; steps++) {
        // Validate: in bounds, aligned, room for the record, strictly above the
        // previous frame. A bad chain is corruption — stop rather than spin.
        if (steps > 4096 || node <= prev || (node & 3) || node + 8 > stack_base ||
            node + 8 > m.mem_size())
            break;
        prev = node;
        uint32_t handler = m.read32(node + 4); // record: { *next, handler }
        // cdecl handler(ExceptionRecord*, EstablisherFrame, Context*, Dispatcher).
        uint32_t disp = m.call_guest(handler, {rec, node, ctx, 0});
        if (disp == ExceptionContinueExecution) {
            if (flags & EXCEPTION_NONCONTINUABLE)
                throw runtime::MachineError{
                    "handler continued a noncontinuable exception"};
            handled = true;
            break;
        }
        if (disp != ExceptionContinueSearch)
            throw runtime::MachineError{"invalid SEH disposition " +
                                        std::to_string(disp)};
        node = m.read32(node); // advance to *next
    }

    m.cpu().gpr[zhelezo::ESP] = saved_esp;
    return handled;
}
} // namespace

void install(Machine& m) {
    g_k = K32{};
    m.add_handler([](Machine& m, const std::string& dll,
                     const std::string& name) -> bool {
        if (dll != "kernel32.dll") return false;

        if (name == "GetStdHandle") {
            uint32_t n = m.arg(0); // -10 in, -11 out, -12 err
            uint32_t h = 0xFFFFFFFFu;
            if (n == (uint32_t)-10) h = H_STDIN;
            else if (n == (uint32_t)-11) h = H_STDOUT;
            else if (n == (uint32_t)-12) h = H_STDERR;
            m.ret(1, h);
            return true;
        }
        if (name == "WriteFile") {
            uint32_t h = m.arg(0), buf = m.arg(1), len = m.arg(2),
                     p_written = m.arg(3);
            if (buf >= m.mem_size() || m.mem_size() - buf < len) {
                m.ret(5, 0); // buffer outside guest memory: fail the call
                return true;
            }
            if (K32::OpenFile* of = as_open_file(h)) {
                // Write into the VFS file at the current position (extending it),
                // capping total size so a guest can't force an unbounded alloc.
                auto& data = g_k.vfs[of->vfs_index].data;
                uint64_t end = of->pos + len;
                if (end > MAX_FILE_BYTES) { m.ret(5, 0); return true; }
                if (data.size() < end) data.resize((size_t)end, 0);
                for (uint32_t i = 0; i < len; i++)
                    data[(size_t)of->pos + i] = m.mem()[buf + i];
                of->pos = end;
                if (p_written) m.write32(p_written, len);
                m.ret(5, 1);
                return true;
            }
            FILE* out = h == H_STDERR ? stderr : stdout;
            std::fwrite(m.mem() + buf, 1, len, out); // exact bytes, NULs included
            std::fflush(out);
            if (p_written) m.write32(p_written, len);
            m.ret(5, 1);
            return true;
        }
        if (name == "CreateFileA") {
            // CreateFileA(name, access, share, sec, disposition, flags, template).
            m.ret(7, open_vfs(m.read_cstr(m.arg(0)), m.arg(4)));
            return true;
        }
        if (name == "CreateFileW") {
            m.ret(7, open_vfs(read_wstr_narrow(m, m.arg(0)), m.arg(4)));
            return true;
        }
        if (name == "ReadFile") {
            // ReadFile(hFile, lpBuffer, nBytes, lpBytesRead, lpOverlapped).
            uint32_t h = m.arg(0), buf = m.arg(1), len = m.arg(2), p_read = m.arg(3);
            K32::OpenFile* of = as_open_file(h);
            if (!of) { // console/unknown handle: 0 bytes read, still success
                if (p_read) m.write32(p_read, 0);
                m.ret(5, 1);
                return true;
            }
            const auto& data = g_k.vfs[of->vfs_index].data;
            // Clamp the read cursor to the data end BEFORE forming the source
            // pointer: data.data()+pos with pos>size (a seek past EOF) is UB even
            // when the copy length is 0.
            uint64_t p = of->pos < data.size() ? of->pos : data.size();
            uint32_t avail = (uint32_t)(data.size() - p);
            uint32_t want = len < avail ? len : avail;
            uint32_t got = guest_write_bytes(m, buf, data.data() + p, want);
            of->pos += got;
            if (p_read) m.write32(p_read, got);
            m.ret(5, 1);
            return true;
        }
        if (name == "SetFilePointer") {
            // SetFilePointer(hFile, lDistanceToMove, lpDistanceToMoveHigh,
            // dwMoveMethod). FILE_BEGIN=0, FILE_CURRENT=1, FILE_END=2.
            uint32_t h = m.arg(0);
            int32_t dist = (int32_t)m.arg(1);
            uint32_t method = m.arg(3);
            K32::OpenFile* of = as_open_file(h);
            if (!of) { m.ret(4, INVALID_HANDLE); return true; }
            int64_t base = method == 1 ? (int64_t)of->pos
                         : method == 2 ? (int64_t)g_k.vfs[of->vfs_index].data.size()
                                       : 0; // FILE_BEGIN
            int64_t np = base + dist; // base is capped ≤ MAX_FILE_BYTES, so no i64 overflow
            // Reject out-of-range targets. Capping pos at MAX_FILE_BYTES keeps
            // base bounded for the next call (no eventual signed overflow) and
            // matches WriteFile, which rejects any write past that size anyway.
            if (np < 0 || np > (int64_t)MAX_FILE_BYTES) {
                g_k.last_error = 131; // ERROR_NEGATIVE_SEEK
                m.ret(4, INVALID_HANDLE);
                return true;
            }
            of->pos = (uint64_t)np;
            m.ret(4, (uint32_t)of->pos);
            return true;
        }
        if (name == "GetFileSize") {
            // GetFileSize(hFile, lpFileSizeHigh) → low 32 bits (high via ptr).
            uint32_t h = m.arg(0), p_high = m.arg(1);
            K32::OpenFile* of = as_open_file(h);
            if (!of) { m.ret(2, INVALID_HANDLE); return true; }
            uint64_t sz = g_k.vfs[of->vfs_index].data.size();
            if (p_high) m.write32(p_high, (uint32_t)(sz >> 32));
            m.ret(2, (uint32_t)sz);
            return true;
        }
        if (name == "GetFileAttributesA") {
            m.ret(1, vfs_attrs(m.read_cstr(m.arg(0))));
            return true;
        }
        if (name == "GetFileAttributesW") {
            m.ret(1, vfs_attrs(read_wstr_narrow(m, m.arg(0))));
            return true;
        }
        if (name == "SetFileAttributesA" || name == "SetFileAttributesW") {
            m.ret(2, 1); // attributes not modeled — accept
            return true;
        }
        if (name == "GetDriveTypeA" || name == "GetDriveTypeW") {
            m.ret(1, 3); // DRIVE_FIXED
            return true;
        }
        if (name == "CloseHandle") {
            // Free a file handle if that's what this is; other handle kinds
            // (heap, pseudo-handles) just succeed.
            if (K32::OpenFile* of = as_open_file(m.arg(0))) of->vfs_index = -1;
            m.ret(1, 1);
            return true;
        }

        // ---- current directory + full-path resolution ----
        if (name == "GetCurrentDirectoryA") {
            // (nBufferLength, lpBuffer). Report the cwd in Windows form (drive +
            // backslashes). Return length excl. NUL, or the required size incl.
            // NUL if the buffer is too small.
            uint32_t size = m.arg(0), buf = m.arg(1);
            std::string w = g_k.cwd;
            for (auto& c : w) if (c == '/') c = '\\';
            uint32_t need = (uint32_t)w.size();
            if (size < need + 1) { m.ret(2, need + 1); return true; }
            guest_write_bytes(m, buf, (const uint8_t*)w.data(), need);
            uint8_t z = 0; guest_write_bytes(m, buf + need, &z, 1);
            m.ret(2, need);
            return true;
        }
        if (name == "SetCurrentDirectoryA") {
            g_k.cwd = resolve_path(m.read_cstr(m.arg(0)));
            m.ret(1, 1);
            return true;
        }
        if (name == "SetCurrentDirectoryW") {
            g_k.cwd = resolve_path(read_wstr_narrow(m, m.arg(0)));
            m.ret(1, 1);
            return true;
        }
        if (name == "GetCurrentDirectoryW") {
            uint32_t size = m.arg(0), buf = m.arg(1);
            std::string w = g_k.cwd;
            for (auto& c : w) if (c == '/') c = '\\';
            uint32_t need = (uint32_t)w.size();
            if (size < need + 1) { m.ret(2, need + 1); return true; }
            for (uint32_t i = 0; i < need; i++) guest_write16(m, buf + i * 2, (uint8_t)w[i]);
            guest_write16(m, buf + need * 2, 0);
            m.ret(2, need);
            return true;
        }
        if (name == "GetFullPathNameA") {
            // (lpFileName, nBufferLength, lpBuffer, lpFilePart). Resolve against
            // the cwd, emit Windows form, and point lpFilePart at the basename.
            std::string full = resolve_path(m.read_cstr(m.arg(0)));
            for (auto& c : full) if (c == '/') c = '\\';
            uint32_t size = m.arg(1), buf = m.arg(2), pFilePart = m.arg(3);
            uint32_t need = (uint32_t)full.size();
            if (size < need + 1) { m.ret(4, need + 1); return true; }
            guest_write_bytes(m, buf, (const uint8_t*)full.data(), need);
            uint8_t z = 0; guest_write_bytes(m, buf + need, &z, 1);
            if (pFilePart) {
                // A directory-form input (trailing slash) has no filename part →
                // NULL, per Win32 (Codex finding). Otherwise point at the basename.
                std::string raw = norm_path(m.read_cstr(m.arg(0)));
                if (!raw.empty() && raw.back() == '/') m.write32(pFilePart, 0);
                else {
                    size_t bs = full.find_last_of('\\');
                    m.write32(pFilePart, bs == std::string::npos ? buf : buf + (uint32_t)bs + 1);
                }
            }
            m.ret(4, need);
            return true;
        }

        // ---- directory enumeration over the VFS ----
        if (name == "FindFirstFileA" || name == "FindFirstFileW") {
            bool w = name == "FindFirstFileW";
            std::string pat = w ? read_wstr_narrow(m, m.arg(0)) : m.read_cstr(m.arg(0));
            if (g_k.finds.size() >= MAX_FINDS) { m.ret(2, INVALID_HANDLE); return true; }
            K32::FindState fs;
            build_find(fs, pat);
            if (fs.entries.empty()) {
                g_k.last_error = 2; // ERROR_FILE_NOT_FOUND
                m.ret(2, INVALID_HANDLE);
                return true;
            }
            fill_find_data_a(m, m.arg(1), fs.entries[0]);
            fs.pos = 1;
            g_k.finds.push_back(std::move(fs));
            m.ret(2, FIND_TAG + (uint32_t)(g_k.finds.size() - 1));
            return true;
        }
        if (name == "FindNextFileA" || name == "FindNextFileW") {
            uint32_t h = m.arg(0);
            if (h < FIND_TAG || (h - FIND_TAG) >= g_k.finds.size()) { m.ret(2, 0); return true; }
            K32::FindState& fs = g_k.finds[h - FIND_TAG];
            if (fs.pos >= fs.entries.size()) {
                g_k.last_error = 18; // ERROR_NO_MORE_FILES
                m.ret(2, 0);
                return true;
            }
            fill_find_data_a(m, m.arg(1), fs.entries[fs.pos++]);
            m.ret(2, 1);
            return true;
        }
        if (name == "FindClose") {
            uint32_t h = m.arg(0);
            if (h >= FIND_TAG && (h - FIND_TAG) < g_k.finds.size())
                g_k.finds[h - FIND_TAG].entries.clear();
            m.ret(1, 1);
            return true;
        }

        // ---- file / directory mutation ----
        if (name == "CreateDirectoryA" || name == "CreateDirectoryW") {
            m.ret(2, 1); // directories are implicit in the flat VFS
            return true;
        }
        if (name == "RemoveDirectoryA" || name == "RemoveDirectoryW") {
            m.ret(1, 1);
            return true;
        }
        if (name == "DeleteFileA" || name == "DeleteFileW") {
            bool w = name == "DeleteFileW";
            std::string path = resolve_path(w ? read_wstr_narrow(m, m.arg(0))
                                              : m.read_cstr(m.arg(0)));
            int idx = vfs_find(path);
            if (idx >= 0) { g_k.vfs[idx].name.clear(); g_k.vfs[idx].data.clear(); } // tombstone
            m.ret(1, idx >= 0 ? 1 : 0);
            return true;
        }
        if (name == "MoveFileA" || name == "MoveFileW") {
            bool w = name == "MoveFileW";
            std::string from = resolve_path(w ? read_wstr_narrow(m, m.arg(0))
                                              : m.read_cstr(m.arg(0)));
            std::string to = resolve_path(w ? read_wstr_narrow(m, m.arg(1))
                                            : m.read_cstr(m.arg(1)));
            int idx = vfs_find(from);
            if (idx >= 0 && vfs_find(to) < 0) { g_k.vfs[idx].name = to; m.ret(2, 1); }
            else m.ret(2, 0);
            return true;
        }
        if (name == "FlushFileBuffers") { m.ret(1, 1); return true; }
        if (name == "SetEndOfFile") {
            // Truncate the file to the current position.
            if (K32::OpenFile* of = as_open_file(m.arg(0)))
                g_k.vfs[of->vfs_index].data.resize((size_t)of->pos, 0);
            m.ret(1, 1);
            return true;
        }
        if (name == "RaiseException") {
            // void RaiseException(code, flags, nNumberOfArguments, lpArguments).
            uint32_t code = m.arg(0), flags = m.arg(1), nargs = m.arg(2),
                     lpargs = m.arg(3);
            uint32_t except_addr = m.read32(m.cpu().gpr[zhelezo::ESP]); // call site
            bool handled = dispatch_seh(m, code, flags, except_addr, nargs, lpargs);
            if (!handled)
                throw runtime::MachineError{"unhandled SEH exception code=" +
                                            std::to_string(code)};
            // Handler chose ContinueExecution: RaiseException returns normally
            // and the guest resumes after the call site.
            m.ret(4, 0);
            return true;
        }
        if (name == "ExitProcess") {
            m.exited = true;
            m.exit_code = m.arg(0);
            return true; // no ret: nothing resumes
        }

        // ---- guest heap ----
        if (name == "GetProcessHeap") { m.ret(0, g_k.heap_handle); return true; }
        if (name == "HeapCreate") { m.ret(3, g_k.heap_handle); return true; }
        if (name == "HeapDestroy") { m.ret(1, 1); return true; }
        if (name == "HeapAlloc") {
            // HeapAlloc(hHeap, dwFlags, dwBytes). HEAP_ZERO_MEMORY = 0x8.
            uint32_t flags = m.arg(1), bytes = m.arg(2);
            uint32_t p = m.alloc(bytes ? bytes : 1);
            if (flags & 0x8)
                for (uint32_t i = 0; i < bytes; i++) m.mem()[p + i] = 0;
            m.ret(3, p);
            return true;
        }
        if (name == "HeapReAlloc") {
            // HeapReAlloc(hHeap, dwFlags, lpMem, dwBytes) — bump-alloc a fresh
            // block and copy dwBytes across (no old-size tracking; over-copy is
            // bounds-clamped). Leaks the old block until the free path lands.
            uint32_t old = m.arg(2), bytes = m.arg(3);
            uint32_t p = m.alloc(bytes ? bytes : 1);
            if (old) // copy old contents (dword steps); no old-size metadata yet
                for (uint32_t i = 0; i + 4 <= bytes; i += 4)
                    m.write32(p + i, m.read32(old + i));
            m.ret(4, p);
            return true;
        }
        if (name == "HeapFree" || name == "HeapSize" ||
            name == "HeapValidate") {
            m.ret(3, name == "HeapFree" ? 1 : 0); // no free (bump allocator)
            return true;
        }

        // ---- virtual memory ----
        if (name == "VirtualAlloc") {
            // VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect).
            uint32_t addr = m.arg(0), size = m.arg(1);
            m.ret(4, addr ? addr : m.alloc(size ? size : 1));
            return true;
        }
        if (name == "VirtualFree") { m.ret(3, 1); return true; }
        if (name == "VirtualProtect") {
            // VirtualProtect(addr, size, newProtect, lpflOldProtect).
            if (m.arg(3)) m.write32(m.arg(3), 4 /*PAGE_READWRITE*/);
            m.ret(4, 1);
            return true;
        }

        // ---- TLS (single thread) ----
        if (name == "TlsAlloc") {
            for (uint32_t i = 0; i < 64; i++)
                if (!g_k.tls_used[i]) {
                    g_k.tls_used[i] = true;
                    g_k.tls[i] = 0;
                    m.ret(0, i);
                    return true;
                }
            m.ret(0, 0xFFFFFFFFu); // TLS_OUT_OF_INDEXES
            return true;
        }
        if (name == "TlsFree") {
            uint32_t i = m.arg(0);
            if (i < 64) g_k.tls_used[i] = false;
            m.ret(1, 1);
            return true;
        }
        if (name == "TlsGetValue") {
            // Values are PER-THREAD (Machine); the alloc bitmap stays global.
            uint32_t i = m.arg(0);
            m.ret(1, i < 64 ? m.tls_get(i) : 0);
            return true;
        }
        if (name == "TlsSetValue") {
            uint32_t i = m.arg(0);
            if (i < 64) m.tls_set(i, m.arg(1));
            m.ret(2, 1);
            return true;
        }

        // ---- critical sections (single-threaded no-ops) ----
        if (name == "InitializeCriticalSection" ||
            name == "EnterCriticalSection" || name == "LeaveCriticalSection" ||
            name == "DeleteCriticalSection") {
            m.ret(1, 0);
            return true;
        }
        if (name == "InitializeCriticalSectionAndSpinCount") {
            m.ret(2, 1);
            return true;
        }

        // ---- timing (deterministic: the shared retired-instruction clock) ----
        if (name == "GetTickCount") {
            m.ret(0, (uint32_t)(m.proc_ticks() / 1000)); // ~1 tick per 1k insns
            return true;
        }
        if (name == "QueryPerformanceCounter") {
            uint32_t p = m.arg(0);
            m.write32(p, (uint32_t)m.proc_ticks());
            m.write32(p + 4, (uint32_t)(m.proc_ticks() >> 32));
            m.ret(1, 1);
            return true;
        }
        if (name == "QueryPerformanceFrequency") {
            uint32_t p = m.arg(0);
            m.write32(p, 1000000); // 1 MHz virtual timer
            m.write32(p + 4, 0);
            m.ret(1, 1);
            return true;
        }
        if (name == "GetSystemTimeAsFileTime") {
            uint32_t p = m.arg(0); // 64-bit FILETIME; a fixed 2004-ish stamp
            m.write32(p, 0xD0000000);
            m.write32(p + 4, 0x01C40000);
            m.ret(1, 0);
            return true;
        }
        if (name == "Sleep") {
            // No-op yield: preemptive slicing already lets other threads run, so
            // a spin-Sleep loop makes progress without parking (and this stays
            // safe to call from inside a reverse thunk, unlike a real wait).
            m.ret(1, 0);
            return true;
        }

        // ---- threads + synchronization (the cooperative scheduler) ----
        if (name == "CreateThread") {
            // (lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter,
            //  dwCreationFlags, lpThreadId). CREATE_SUSPENDED = 0x4.
            uint32_t entry = m.arg(2), param = m.arg(3), flags = m.arg(4),
                     pTid = m.arg(5);
            uint32_t h = m.create_thread(entry, param, flags & 0x4);
            if (pTid) m.write32(pTid, h); // thread id token = the handle
            m.ret(6, h);
            return true;
        }
        if (name == "ResumeThread") { m.ret(1, m.resume_thread(m.arg(0))); return true; }
        if (name == "SuspendThread") { m.ret(1, m.suspend_thread(m.arg(0))); return true; }
        if (name == "SetThreadPriority") { m.ret(2, 1); return true; }
        if (name == "GetThreadPriority") { m.ret(1, 0); return true; } // NORMAL
        if (name == "GetExitCodeThread") {
            uint32_t code = 0x103; // STILL_ACTIVE
            m.thread_exit_code(m.arg(0), code);
            if (m.arg(1)) m.write32(m.arg(1), code);
            m.ret(2, 1);
            return true;
        }
        if (name == "CreateEventA" || name == "CreateEventW") {
            // (lpEventAttributes, bManualReset, bInitialState, lpName).
            m.ret(4, m.create_event(m.arg(1) != 0, m.arg(2) != 0));
            return true;
        }
        if (name == "SetEvent") { m.set_event(m.arg(0)); m.ret(1, 1); return true; }
        if (name == "ResetEvent") { m.reset_event(m.arg(0)); m.ret(1, 1); return true; }
        if (name == "PulseEvent") { // signal, wake waiters, then clear
            m.set_event(m.arg(0));
            m.reset_event(m.arg(0));
            m.ret(1, 1);
            return true;
        }
        if (name == "CreateMutexA" || name == "CreateMutexW") {
            // (lpMutexAttributes, bInitialOwner, lpName).
            m.ret(3, m.create_mutex(m.arg(1) != 0));
            return true;
        }
        if (name == "ReleaseMutex") { m.ret(1, m.release_mutex(m.arg(0)) ? 1 : 0); return true; }
        if (name == "WaitForSingleObject") {
            // (hHandle, dwMilliseconds). Parks the thread; the SCHEDULER writes
            // the ret when the object signals or the timeout elapses — do NOT
            // call m.ret here.
            m.begin_wait({m.arg(0)}, false, m.arg(1), 2);
            return true;
        }
        if (name == "WaitForMultipleObjects") {
            // (nCount, lpHandles, bWaitAll, dwMilliseconds).
            uint32_t n = m.arg(0), ph = m.arg(1);
            if (n > 64) n = 64; // MAXIMUM_WAIT_OBJECTS
            std::vector<uint32_t> handles;
            for (uint32_t i = 0; i < n; i++) handles.push_back(m.read32(ph + i * 4));
            m.begin_wait(handles, m.arg(2) != 0, m.arg(3), 4);
            return true;
        }
        if (name == "RtlUnwind") {
            // SEH collapse-unwind: the happy boot path never unwinds. Return so
            // the caller continues; a real unwind lands with the __except work.
            m.ret(4, 0);
            return true;
        }

        // ---- module / process info ----
        if (name == "GetModuleHandleA") {
            // NULL name = the process image itself (base 0x400000); a named
            // module interns to a distinct handle carrying its DLL identity.
            uint32_t p = m.arg(0);
            m.ret(1, p == 0 ? 0x00400000 : module_handle(m.read_cstr(p)));
            return true;
        }
        if (name == "GetModuleHandleW") {
            // Decode the wide name so W lookups intern and scope like A lookups.
            uint32_t p = m.arg(0);
            m.ret(1, p == 0 ? 0x00400000 : module_handle(read_wstr_narrow(m, p)));
            return true;
        }
        if (name == "GetProcAddress") {
            // GetProcAddress(hModule, lpProcName). An ordinal request (name ptr
            // with a zero high word) is unsupported → 0. With a real interned
            // module handle, scope the name to that DLL so a cross-DLL name
            // collision can't resolve to the wrong ABI's thunk; the image base
            // or an unknown handle falls back to a name-only match. 0 = "not
            // found" so the guest gracefully skips the optional API.
            uint32_t hmod = m.arg(0), proc = m.arg(1), addr = 0;
            if ((proc >> 16) != 0) {
                std::string pname = m.read_cstr(proc);
                if (hmod >= MODULE_TAG && (hmod - MODULE_TAG) < g_k.modules.size())
                    addr = m.resolve_proc(g_k.modules[hmod - MODULE_TAG], pname);
                else
                    addr = m.resolve_proc(pname);
            }
            m.ret(2, addr);
            return true;
        }
        if (name == "LoadLibraryA" || name == "LoadLibraryExA") {
            // Return an interned handle for the named DLL (arg0 for both; ExA's
            // extra args are hFile/flags). GetProcAddress then scopes to it.
            uint32_t p = m.arg(0);
            uint32_t h = p ? module_handle(m.read_cstr(p)) : 0x00400000;
            m.ret(name == "LoadLibraryExA" ? 3 : 1, h);
            return true;
        }
        if (name == "LoadLibraryW" || name == "LoadLibraryExW") {
            // Decode + intern the wide name so W loads scope like A loads.
            uint32_t p = m.arg(0);
            uint32_t h = p ? module_handle(read_wstr_narrow(m, p)) : 0x00400000;
            m.ret(name == "LoadLibraryExW" ? 3 : 1, h);
            return true;
        }
        if (name == "FreeLibrary") { m.ret(1, 1); return true; }

        // ---- CRT pre-main surface (MSVC __tmainCRTStartup) ----
        // Atomics (single guest thread → a plain read-modify-write is atomic).
        if (name == "InterlockedIncrement") {
            uint32_t p = m.arg(0), v = m.read32(p) + 1;
            m.write32(p, v);
            m.ret(1, v);
            return true;
        }
        if (name == "InterlockedDecrement") {
            uint32_t p = m.arg(0), v = m.read32(p) - 1;
            m.write32(p, v);
            m.ret(1, v);
            return true;
        }
        if (name == "InterlockedExchange") {
            uint32_t p = m.arg(0), old = m.read32(p);
            m.write32(p, m.arg(1));
            m.ret(2, old);
            return true;
        }
        // Code-page / locale: RTW is ANSI, so a CP-agnostic passthrough
        // (low-byte narrow, zero-extend wide, ASCII classification) suffices.
        if (name == "GetCPInfo") {
            // GetCPInfo(cp, LPCPINFO): {UINT MaxCharSize; BYTE DefaultChar[2];
            // BYTE LeadByte[12]} = 20 bytes. Single-byte codepage.
            uint32_t p = m.arg(1);
            m.write32(p + 0, 1);       // MaxCharSize
            m.write32(p + 4, 0x003F);  // DefaultChar = '?', 0
            m.write32(p + 8, 0);       // LeadByte ranges: none
            m.write32(p + 12, 0);
            m.write32(p + 16, 0);
            m.ret(2, 1);
            return true;
        }
        if (name == "GetOEMCP") { m.ret(0, 437); return true; }
        if (name == "IsValidCodePage") { m.ret(1, 1); return true; }
        if (name == "IsValidLocale") { m.ret(2, 1); return true; }
        if (name == "GetUserDefaultLCID") { m.ret(0, 0x0409); return true; } // en-US
        if (name == "WideCharToMultiByte") {
            // (cp, flags, wstr, wlen, mbstr, mblen, defchar, usedflag). wlen=-1
            // → NUL-terminated (count includes the NUL). mblen=0 → return the
            // required byte count. Narrowing = take the low byte of each wchar.
            uint32_t wstr = m.arg(2);
            int32_t wlen = (int32_t)m.arg(3);
            uint32_t mbstr = m.arg(4), mblen = m.arg(5);
            uint32_t n = 0; // source wchar count
            if (wlen < 0) { while (n < MAX_CONV_CHARS && (m.read32(wstr + n * 2) & 0xFFFF)) n++; n++; }
            else n = (uint32_t)wlen;
            if (n > MAX_CONV_CHARS) n = MAX_CONV_CHARS; // bound the host loop
            if (mblen == 0) { m.ret(8, n); return true; } // query length
            uint32_t out = n < mblen ? n : mblen;
            for (uint32_t i = 0; i < out; i++) {
                uint8_t b = (uint8_t)(m.read32(wstr + i * 2) & 0xFF); // low byte
                guest_write_bytes(m, mbstr + i, &b, 1);
            }
            m.ret(8, out);
            return true;
        }
        if (name == "MultiByteToWideChar") {
            // (cp, flags, mbstr, mblen, wstr, wlen). mblen=-1 → NUL-terminated.
            // wlen=0 → return required wchar count. Widening = zero-extend byte.
            uint32_t mbstr = m.arg(2);
            int32_t mblen = (int32_t)m.arg(3);
            uint32_t wstr = m.arg(4), wlen = m.arg(5);
            uint32_t n = 0;
            if (mblen < 0) { while (n < MAX_CONV_CHARS && (m.read32(mbstr + n) & 0xFF)) n++; n++; }
            else n = (uint32_t)mblen;
            if (n > MAX_CONV_CHARS) n = MAX_CONV_CHARS;
            if (wlen == 0) { m.ret(6, n); return true; }
            uint32_t out = n < wlen ? n : wlen;
            for (uint32_t i = 0; i < out; i++)
                guest_write16(m, wstr + i * 2, (uint16_t)(m.read32(mbstr + i) & 0xFF));
            m.ret(6, out);
            return true;
        }
        if (name == "GetStringTypeA" || name == "GetStringTypeW") {
            // A: (lcid, dwInfoType, src, cch, lpCharType). W: (dwInfoType, src,
            // cch, lpCharType). Only CT_CTYPE1 supported (ASCII classification).
            bool w = name == "GetStringTypeW";
            uint32_t src = m.arg(w ? 1 : 2);
            int32_t cch = (int32_t)m.arg(w ? 2 : 3);
            uint32_t out = m.arg(w ? 3 : 4);
            uint32_t step = w ? 2 : 1;
            uint32_t n = 0;
            if (cch < 0) { while (n < MAX_CONV_CHARS && (m.read32(src + n * step) & (w ? 0xFFFF : 0xFF))) n++; n++; }
            else n = (uint32_t)cch;
            if (n > MAX_CONV_CHARS) n = MAX_CONV_CHARS;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t c = m.read32(src + i * step) & (w ? 0xFFFF : 0xFF);
                uint16_t t = 0;
                if (c >= 'A' && c <= 'Z') t |= 0x0001 | 0x0100;        // UPPER|ALPHA
                else if (c >= 'a' && c <= 'z') t |= 0x0002 | 0x0100;   // LOWER|ALPHA
                else if (c >= '0' && c <= '9') t |= 0x0004;            // DIGIT
                else if (c == ' ' || (c >= 9 && c <= 13)) t |= 0x0008; // SPACE
                if (c < 0x20) t |= 0x0020;                             // CNTRL
                guest_write16(m, out + i * 2, t);
            }
            m.ret(w ? 4 : 5, 1);
            return true;
        }
        if (name == "LCMapStringA" || name == "LCMapStringW") {
            // (lcid, flags, src, srclen, dst, dstlen). LCMAP_LOWERCASE=0x100,
            // LCMAP_UPPERCASE=0x200. Copy applying the case transform.
            bool w = name == "LCMapStringW";
            uint32_t flags = m.arg(1), src = m.arg(2);
            int32_t srclen = (int32_t)m.arg(3);
            uint32_t dst = m.arg(4), dstlen = m.arg(5);
            uint32_t step = w ? 2 : 1, mask = w ? 0xFFFF : 0xFF;
            uint32_t n = 0;
            if (srclen < 0) { while (n < MAX_CONV_CHARS && (m.read32(src + n * step) & mask)) n++; n++; }
            else n = (uint32_t)srclen;
            if (n > MAX_CONV_CHARS) n = MAX_CONV_CHARS;
            if (dstlen == 0) { m.ret(6, n); return true; } // query length
            uint32_t out = n < dstlen ? n : dstlen;
            for (uint32_t i = 0; i < out; i++) {
                uint32_t c = m.read32(src + i * step) & mask;
                if ((flags & 0x200) && c >= 'a' && c <= 'z') c -= 32;      // UPPER
                else if ((flags & 0x100) && c >= 'A' && c <= 'Z') c += 32; // LOWER
                if (w) guest_write16(m, dst + i * 2, (uint16_t)c);
                else { uint8_t b = (uint8_t)c; guest_write_bytes(m, dst + i, &b, 1); }
            }
            m.ret(6, out);
            return true;
        }
        if (name == "GetLocaleInfoA" || name == "GetLocaleInfoW") {
            // (lcid, lctype, buf, len). Minimal: an empty string, length 1.
            // Refine specific LCTYPEs from stub logs if RTW needs them.
            bool w = name == "GetLocaleInfoW";
            uint32_t buf = m.arg(2), len = m.arg(3);
            if (len > 0) { if (w) guest_write16(m, buf, 0); else { uint8_t z = 0; guest_write_bytes(m, buf, &z, 1); } }
            m.ret(4, 1);
            return true;
        }
        if (name == "EnumSystemLocalesA") {
            // (lpLocaleEnumProc, dwFlags): reverse-thunk the callback once with
            // the default locale id string "0409", then stop.
            uint32_t proc = m.arg(0);
            if (proc) {
                uint32_t s = guest_strdup(m, "0409");
                m.call_guest(proc, {s});
            }
            m.ret(2, 1);
            return true;
        }
        // Environment block: a minimal double-NUL-terminated empty block.
        if (name == "GetEnvironmentStrings" || name == "GetEnvironmentStringsA") {
            if (!g_k.env_a) {
                g_k.env_a = m.alloc(2);
                m.mem()[g_k.env_a] = 0;
                m.mem()[g_k.env_a + 1] = 0;
            }
            m.ret(0, g_k.env_a);
            return true;
        }
        if (name == "GetEnvironmentStringsW") {
            if (!g_k.env_w) {
                g_k.env_w = m.alloc(4);
                for (int i = 0; i < 4; i++) m.mem()[g_k.env_w + i] = 0;
            }
            m.ret(0, g_k.env_w);
            return true;
        }
        if (name == "FreeEnvironmentStringsA" || name == "FreeEnvironmentStringsW") {
            m.ret(1, 1); // blocks are HLE-owned/immortal
            return true;
        }
        if (name == "GetEnvironmentVariableA") {
            g_k.last_error = 203; // ERROR_ENVVAR_NOT_FOUND
            m.ret(3, 0); // not found
            return true;
        }
        if (name == "SetEnvironmentVariableA" || name == "SetEnvironmentVariableW") {
            m.ret(2, 1);
            return true;
        }
        // Handles / std streams.
        if (name == "GetFileType") {
            uint32_t h = m.arg(0);
            uint32_t t = (h == H_STDIN || h == H_STDOUT || h == H_STDERR) ? 2 /*CHAR*/
                       : 1 /*DISK*/;
            m.ret(1, t);
            return true;
        }
        if (name == "SetHandleCount") { m.ret(1, m.arg(0)); return true; }
        if (name == "SetStdHandle") { m.ret(2, 1); return true; }
        if (name == "GetModuleFileNameA") {
            // (hModule, lpFilename, nSize). Report a plausible install path.
            const char* path = "C:\\RTW\\RomeTW.exe";
            uint32_t buf = m.arg(1), size = m.arg(2);
            uint32_t n = 0;
            while (path[n]) n++;
            uint32_t out = n < size ? n : size;
            guest_write_bytes(m, buf, (const uint8_t*)path, out);
            if (out < size) { uint8_t z = 0; guest_write_bytes(m, buf + out, &z, 1); }
            m.ret(3, out);
            return true;
        }
        if (name == "UnhandledExceptionFilter") {
            m.ret(1, 1); // EXCEPTION_EXECUTE_HANDLER (never hit on the happy path)
            return true;
        }
        if (name == "TerminateProcess") {
            uint32_t h = m.arg(0);
            if (h == 0xFFFFFFFFu) { // the current process → end the run
                m.exited = true;
                m.exit_code = m.arg(1);
                return true;
            }
            m.ret(2, 1);
            return true;
        }

        // ---- CRT tail: compare, probes, arithmetic, memory status ----
        if (name == "CompareStringA" || name == "CompareStringW") {
            // (lcid, flags, str1, cch1, str2, cch2) → 1 LESS / 2 EQUAL / 3
            // GREATER. cch -1 = NUL-terminated. NORM_IGNORECASE = 1.
            bool w = name == "CompareStringW";
            bool nocase = m.arg(1) & 1;
            uint32_t s1 = m.arg(2), s2 = m.arg(4);
            int32_t c1 = (int32_t)m.arg(3), c2 = (int32_t)m.arg(5);
            uint32_t step = w ? 2 : 1, mask = w ? 0xFFFF : 0xFF;
            uint32_t n1 = 0, n2 = 0;
            if (c1 < 0) { while (n1 < MAX_CONV_CHARS && (m.read32(s1 + n1 * step) & mask)) n1++; }
            else n1 = (uint32_t)c1 > MAX_CONV_CHARS ? MAX_CONV_CHARS : (uint32_t)c1;
            if (c2 < 0) { while (n2 < MAX_CONV_CHARS && (m.read32(s2 + n2 * step) & mask)) n2++; }
            else n2 = (uint32_t)c2 > MAX_CONV_CHARS ? MAX_CONV_CHARS : (uint32_t)c2;
            uint32_t lim = n1 < n2 ? n1 : n2, r = 2;
            for (uint32_t i = 0; i < lim; i++) {
                uint32_t a = m.read32(s1 + i * step) & mask;
                uint32_t b = m.read32(s2 + i * step) & mask;
                if (nocase) {
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (b >= 'A' && b <= 'Z') b += 32;
                }
                if (a != b) { r = a < b ? 1 : 3; break; }
            }
            if (r == 2 && n1 != n2) r = n1 < n2 ? 1 : 3;
            m.ret(6, r);
            return true;
        }
        if (name == "IsBadReadPtr" || name == "IsBadWritePtr") {
            // (lp, ucb) → 0 = ok, nonzero = bad. Bad if null or outside arena.
            uint32_t lp = m.arg(0), n = m.arg(1);
            bool bad = lp == 0 || lp >= m.mem_size() || m.mem_size() - lp < n;
            m.ret(2, bad ? 1 : 0);
            return true;
        }
        if (name == "IsBadCodePtr") {
            uint32_t lp = m.arg(0);
            m.ret(1, (lp == 0 || lp >= m.mem_size()) ? 1 : 0);
            return true;
        }
        if (name == "MulDiv") {
            // (int a, int b, int c) → (a*b + c/2)/c, 64-bit intermediate,
            // rounded to nearest; c == 0 → -1.
            int32_t a = (int32_t)m.arg(0), b = (int32_t)m.arg(1),
                    c = (int32_t)m.arg(2);
            if (c == 0) { m.ret(3, (uint32_t)-1); return true; }
            int64_t v = (int64_t)a * b;
            v += (v >= 0) == (c >= 0) ? c / 2 : -(c / 2);
            m.ret(3, (uint32_t)(int32_t)(v / c));
            return true;
        }
        if (name == "GlobalMemoryStatus") {
            // (LPMEMORYSTATUS): 32 bytes of dwords. A roomy 512 MB machine.
            uint32_t p = m.arg(0);
            m.write32(p + 0, 32);           // dwLength
            m.write32(p + 4, 25);           // dwMemoryLoad %
            m.write32(p + 8, 512u << 20);   // dwTotalPhys
            m.write32(p + 12, 384u << 20);  // dwAvailPhys
            m.write32(p + 16, 1024u << 20); // dwTotalPageFile
            m.write32(p + 20, 896u << 20);  // dwAvailPageFile
            m.write32(p + 24, 0x7FFE0000);  // dwTotalVirtual (~2GB user space)
            m.write32(p + 28, 0x70000000);  // dwAvailVirtual
            m.ret(1, 1);
            return true;
        }

        // ---- wall-clock family (deterministic fixed date: 2004-06-15 12:00) ----
        if (name == "GetSystemTime" || name == "GetLocalTime") {
            uint32_t p = m.arg(0); // SYSTEMTIME: 8 WORDs
            int64_t days = days_from_1601(2004, 6, 15);
            guest_write16(m, p + 0, 2004);                    // wYear
            guest_write16(m, p + 2, 6);                       // wMonth
            guest_write16(m, p + 4, (uint16_t)((days + 1) % 7)); // wDayOfWeek
            guest_write16(m, p + 6, 15);                      // wDay
            guest_write16(m, p + 8, 12);                      // wHour
            guest_write16(m, p + 10, 0);                      // wMinute
            guest_write16(m, p + 12, 0);                      // wSecond
            guest_write16(m, p + 14, 0);                      // wMilliseconds
            m.ret(1, 0);
            return true;
        }
        if (name == "GetTimeZoneInformation") {
            // (LPTIME_ZONE_INFORMATION): 172 bytes, zeroed = UTC, Bias 0.
            uint32_t p = m.arg(0);
            for (uint32_t off = 0; off < 172; off += 4) m.write32(p + off, 0);
            m.ret(1, 0); // TIME_ZONE_ID_UNKNOWN
            return true;
        }
        if (name == "SystemTimeToFileTime") {
            // (const SYSTEMTIME*, LPFILETIME): real calendar math so the
            // SystemTime→FileTime→SystemTime round trip is exact.
            uint32_t st = m.arg(0), ft = m.arg(1);
            int y = (int)(m.read32(st + 0) & 0xFFFF);
            int mo = (int)(m.read32(st + 2) & 0xFFFF);
            int d = (int)(m.read32(st + 6) & 0xFFFF);
            uint32_t hh = m.read32(st + 8) & 0xFFFF, mi = m.read32(st + 10) & 0xFFFF,
                     ss = m.read32(st + 12) & 0xFFFF, ms = m.read32(st + 14) & 0xFFFF;
            if (y < 1601 || mo < 1 || mo > 12 || d < 1 || d > 31) {
                m.ret(2, 0);
                return true;
            }
            uint64_t t = (uint64_t)days_from_1601(y, mo, d) * 86400ull;
            t = ((t + hh * 3600ull + mi * 60ull + ss) * 1000ull + ms) * 10000ull;
            m.write32(ft + 0, (uint32_t)t);
            m.write32(ft + 4, (uint32_t)(t >> 32));
            m.ret(2, 1);
            return true;
        }
        if (name == "FileTimeToSystemTime") {
            uint32_t ft = m.arg(0), st = m.arg(1);
            uint64_t t = m.read32(ft) | ((uint64_t)m.read32(ft + 4) << 32);
            uint64_t ms_total = t / 10000ull;
            uint64_t days = ms_total / 86400000ull;
            uint32_t ms_day = (uint32_t)(ms_total % 86400000ull);
            int y, mo, d;
            civil_from_1601((int64_t)days, y, mo, d);
            guest_write16(m, st + 0, (uint16_t)y);
            guest_write16(m, st + 2, (uint16_t)mo);
            guest_write16(m, st + 4, (uint16_t)((days + 1) % 7));
            guest_write16(m, st + 6, (uint16_t)d);
            guest_write16(m, st + 8, (uint16_t)(ms_day / 3600000u));
            guest_write16(m, st + 10, (uint16_t)(ms_day / 60000u % 60u));
            guest_write16(m, st + 12, (uint16_t)(ms_day / 1000u % 60u));
            guest_write16(m, st + 14, (uint16_t)(ms_day % 1000u));
            m.ret(2, 1);
            return true;
        }
        if (name == "LocalFileTimeToFileTime" || name == "FileTimeToLocalFileTime") {
            // Bias 0 (UTC): the identity copy.
            uint32_t src = m.arg(0), dst = m.arg(1);
            m.write32(dst + 0, m.read32(src + 0));
            m.write32(dst + 4, m.read32(src + 4));
            m.ret(2, 1);
            return true;
        }
        if (name == "SetFileTime") { m.ret(4, 1); return true; }

        // ---- file mapping (views are copies; no writeback in tier 0) ----
        if (name == "CreateFileMappingA") {
            // (hFile, lpAttributes, flProtect, dwMaxSizeHigh, dwMaxSizeLow,
            //  lpName). hFile == INVALID_HANDLE_VALUE → anonymous (pagefile).
            uint32_t hFile = m.arg(0), size_lo = m.arg(4);
            if (g_k.mappings.size() >= MAX_MAPPINGS) { m.ret(6, 0); return true; }
            K32::Mapping map;
            if (hFile == INVALID_HANDLE) { // anonymous: needs an explicit size
                if (size_lo == 0 || size_lo > MAX_FILE_BYTES) { m.ret(6, 0); return true; }
                map.size = size_lo;
            } else {
                K32::OpenFile* of = as_open_file(hFile);
                if (!of) { m.ret(6, 0); return true; }
                map.vfs_index = of->vfs_index;
                uint32_t fsz = (uint32_t)g_k.vfs[of->vfs_index].data.size();
                map.size = size_lo ? (size_lo < fsz ? size_lo : fsz) : fsz;
                if (map.size == 0) { m.ret(6, 0); return true; } // empty file
            }
            g_k.mappings.push_back(map);
            m.ret(6, MAP_TAG + (uint32_t)(g_k.mappings.size() - 1));
            return true;
        }
        if (name == "MapViewOfFile") {
            // (hMapping, dwDesiredAccess, dwOffsetHigh, dwOffsetLow, dwBytes).
            // Alloc guest memory and copy the file bytes in (identity-mapped:
            // the returned VA is directly readable by guest code).
            uint32_t h = m.arg(0), off_lo = m.arg(3), want = m.arg(4);
            if (h < MAP_TAG || (h - MAP_TAG) >= g_k.mappings.size()) {
                m.ret(5, 0);
                return true;
            }
            const K32::Mapping& map = g_k.mappings[h - MAP_TAG];
            if (off_lo >= map.size) { m.ret(5, 0); return true; }
            uint32_t span = map.size - off_lo;
            if (want && want < span) span = want;
            uint32_t va = m.alloc(span);
            if (map.vfs_index >= 0) {
                const auto& data = g_k.vfs[map.vfs_index].data;
                uint32_t avail = off_lo < data.size() ? (uint32_t)data.size() - off_lo : 0;
                uint32_t n = span < avail ? span : avail;
                if (n) guest_write_bytes(m, va, data.data() + off_lo, n);
            } // anonymous: m.alloc arena is pre-zeroed
            m.ret(5, va);
            return true;
        }
        if (name == "UnmapViewOfFile") {
            m.ret(1, 1); // view copies are immortal in tier 0 (no writeback)
            return true;
        }
        if (name == "GetFullPathNameW") {
            // Like the A form, but emits UTF-16.
            std::string full = resolve_path(read_wstr_narrow(m, m.arg(0)));
            for (auto& c : full) if (c == '/') c = '\\';
            uint32_t size = m.arg(1), buf = m.arg(2), pFilePart = m.arg(3);
            uint32_t need = (uint32_t)full.size();
            if (size < need + 1) { m.ret(4, need + 1); return true; }
            for (uint32_t i = 0; i < need; i++)
                guest_write16(m, buf + i * 2, (uint8_t)full[i]);
            guest_write16(m, buf + need * 2, 0);
            if (pFilePart) {
                std::string raw = read_wstr_narrow(m, m.arg(0));
                if (!raw.empty() && (raw.back() == '\\' || raw.back() == '/'))
                    m.write32(pFilePart, 0);
                else {
                    size_t bs = full.find_last_of('\\');
                    m.write32(pFilePart,
                              bs == std::string::npos ? buf : buf + ((uint32_t)bs + 1) * 2);
                }
            }
            m.ret(4, need);
            return true;
        }

        if (name == "GetCommandLineA") {
            if (!g_k.cmdline) g_k.cmdline = guest_strdup(m, "fortochka.exe");
            m.ret(0, g_k.cmdline);
            return true;
        }
        if (name == "GetCurrentThreadId") {
            m.ret(0, m.current_tid() + 1); // distinct nonzero id per thread
            return true;
        }
        if (name == "GetCurrentProcessId") { m.ret(0, 1); return true; }
        if (name == "GetCurrentProcess") { m.ret(0, 0xFFFFFFFFu); return true; }
        if (name == "GetCurrentThread") { m.ret(0, 0xFFFFFFFEu); return true; } // (HANDLE)-2
        if (name == "GetVersion") {
            m.ret(0, 0x0A280105); // build 0x0A28, Windows 5.1 (XP)
            return true;
        }
        if (name == "GetVersionExA" || name == "GetVersionExW") {
            uint32_t p = m.arg(0); // OSVERSIONINFO: keep dwOSVersionInfoSize
            m.write32(p + 4, 5);   // dwMajorVersion
            m.write32(p + 8, 1);   // dwMinorVersion
            m.write32(p + 12, 2600); // dwBuildNumber
            m.write32(p + 16, 2);  // dwPlatformId = VER_PLATFORM_WIN32_NT
            for (uint32_t i = 20; i < 20 + 128; i += 4) m.write32(p + i, 0); // szCSDVersion
            m.ret(1, 1);
            return true;
        }
        if (name == "GetStartupInfoA" || name == "GetStartupInfoW") {
            uint32_t p = m.arg(0); // zero STARTUPINFO (68 bytes), set cb
            for (uint32_t i = 0; i < 68; i += 4) m.write32(p + i, 0);
            m.write32(p, 68);
            m.ret(1, 0);
            return true;
        }

        // ---- error / misc ----
        if (name == "SetLastError") { g_k.last_error = m.arg(0); m.ret(1, 0); return true; }
        if (name == "GetLastError") { m.ret(0, g_k.last_error); return true; }
        if (name == "SetUnhandledExceptionFilter") { m.ret(1, 0); return true; }
        if (name == "OutputDebugStringA") {
            fprintf(stderr, "[guest] %s", m.read_cstr(m.arg(0)).c_str());
            m.ret(1, 0);
            return true;
        }
        if (name == "IsProcessorFeaturePresent") {
            // Report ONLY what zhelezo implements, so a game doesn't select a
            // path we can't run (e.g. SSE2). 2=CMPXCHG_DOUBLE, 6=XMMI(SSE1),
            // 8=RDTSC, 23=CMPXCHG16B(no). SSE2(10)+ report false.
            uint32_t f = m.arg(0);
            m.ret(1, (f == 2 || f == 6 || f == 8) ? 1 : 0);
            return true;
        }
        if (name == "GetACP") { m.ret(0, 1252); return true; }
        if (name == "IsDebuggerPresent") { m.ret(0, 0); return true; }
        if (name == "GetSystemInfo") {
            uint32_t p = m.arg(0); // zero SYSTEM_INFO, set a plausible page size
            for (uint32_t i = 0; i < 36; i += 4) m.write32(p + i, 0);
            m.write32(p + 4, 0x1000);    // dwPageSize
            m.write32(p + 16, 1);        // dwActiveProcessorMask
            m.write32(p + 20, 1);        // dwNumberOfProcessors
            m.write32(p + 28, 0x10000);  // dwAllocationGranularity (64 KB)
            m.ret(1, 0);
            return true;
        }
        if (name == "lstrlenA") {
            m.ret(1, (uint32_t)m.read_cstr(m.arg(0)).size());
            return true;
        }
        return false;
    });

    // ---- advapi32: the registry shim ----
    // Exact-match store; everything else "not found" (LSTATUS in EAX, not
    // GetLastError), which pushes an era game onto its relative-path fallback.
    // Misses are logged to stderr so a boot log names the keys worth seeding.
    m.add_handler([](Machine& m, const std::string& dll,
                     const std::string& name) -> bool {
        if (dll != "advapi32.dll") return false;
        constexpr uint32_t ERROR_SUCCESS = 0, ERROR_FILE_NOT_FOUND = 2,
                           ERROR_MORE_DATA = 234, ERROR_INVALID_HANDLE_ = 6,
                           ERROR_NO_SYSTEM_RESOURCES = 1450;

        if (name == "RegOpenKeyA" || name == "RegOpenKeyExA") {
            // RegOpenKeyA(hKey, lpSubKey, phkResult) = 3
            // RegOpenKeyExA(hKey, lpSubKey, ulOptions, samDesired, phkResult) = 5
            bool ex = name == "RegOpenKeyExA";
            uint32_t nargs = ex ? 5 : 3;
            uint32_t phk = m.arg(ex ? 4 : 2);
            std::string path = reg_join(m.arg(0), m.read_cstr(m.arg(1)));
            if (path.empty()) { m.ret(nargs, ERROR_INVALID_HANDLE_); return true; }
            if (!reg_key_exists(path)) {
                std::fprintf(stderr, "regweb: open miss %s\n", path.c_str());
                m.ret(nargs, ERROR_FILE_NOT_FOUND);
                return true;
            }
            uint32_t h = reg_open_handle(path);
            if (!h) { m.ret(nargs, ERROR_NO_SYSTEM_RESOURCES); return true; }
            m.write32(phk, h);
            m.ret(nargs, ERROR_SUCCESS);
            return true;
        }
        if (name == "RegCreateKeyExA") {
            // (hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired,
            //  lpSecurityAttributes, phkResult, lpdwDisposition) = 9
            std::string path = reg_join(m.arg(0), m.read_cstr(m.arg(1)));
            if (path.empty()) { m.ret(9, ERROR_INVALID_HANDLE_); return true; }
            bool existed = reg_key_exists(path);
            if (!existed) {
                if (g_k.reg_created.size() >= MAX_REG_KEYS) {
                    m.ret(9, ERROR_NO_SYSTEM_RESOURCES);
                    return true;
                }
                g_k.reg_created.push_back(path);
            }
            uint32_t h = reg_open_handle(path);
            if (!h) { m.ret(9, ERROR_NO_SYSTEM_RESOURCES); return true; }
            m.write32(m.arg(7), h);
            uint32_t pdisp = m.arg(8); // 1 = created new, 2 = opened existing
            if (pdisp) m.write32(pdisp, existed ? 2 : 1);
            m.ret(9, ERROR_SUCCESS);
            return true;
        }
        if (name == "RegQueryValueExA") {
            // (hKey, lpValueName, lpReserved, lpType, lpData, lpcbData) = 6
            std::string key = reg_key_path(m.arg(0));
            std::string vname = norm_path(m.read_cstr(m.arg(1)));
            uint32_t pType = m.arg(3), pData = m.arg(4), pcb = m.arg(5);
            K32::RegValue* v = key.empty() ? nullptr : reg_find(key, vname);
            if (!v) {
                std::fprintf(stderr, "regweb: query miss %s [%s]\n", key.c_str(),
                             vname.c_str());
                m.ret(6, ERROR_FILE_NOT_FOUND);
                return true;
            }
            if (pData && !pcb) { // Win32: data without a size is invalid
                m.ret(6, 87 /*ERROR_INVALID_PARAMETER*/);
                return true;
            }
            if (pType) m.write32(pType, v->type);
            uint32_t need = (uint32_t)v->data.size();
            uint32_t have = pcb ? m.read32(pcb) : 0;
            if (pcb) m.write32(pcb, need);
            if (!pData) { m.ret(6, ERROR_SUCCESS); return true; } // size probe
            if (have < need) { m.ret(6, ERROR_MORE_DATA); return true; }
            guest_write_bytes(m, pData, v->data.data(), need);
            m.ret(6, ERROR_SUCCESS);
            return true;
        }
        if (name == "RegSetValueExA") {
            // (hKey, lpValueName, Reserved, dwType, lpData, cbData) = 6
            std::string key = reg_key_path(m.arg(0));
            if (key.empty()) { m.ret(6, ERROR_INVALID_HANDLE_); return true; }
            uint32_t type = m.arg(3), pData = m.arg(4), cb = m.arg(5);
            if (cb > MAX_REG_DATA) { m.ret(6, ERROR_NO_SYSTEM_RESOURCES); return true; }
            std::string vname = norm_path(m.read_cstr(m.arg(1)));
            K32::RegValue* v = reg_find(key, vname);
            if (!v) {
                if (g_k.reg.size() >= MAX_REG_VALUES) {
                    m.ret(6, ERROR_NO_SYSTEM_RESOURCES);
                    return true;
                }
                g_k.reg.push_back({key, vname, type, {}});
                v = &g_k.reg.back();
            }
            v->type = type;
            v->data.assign(cb, 0);
            for (uint32_t i = 0; i < cb; i++) // bounds-checked guest read
                v->data[i] = (uint8_t)(m.read32(pData + i) & 0xFF);
            m.ret(6, ERROR_SUCCESS);
            return true;
        }
        if (name == "RegCloseKey") {
            m.ret(1, ERROR_SUCCESS); // open-key slots are immortal in tier 0
            return true;
        }
        return false;
    });
}

void mount_host_dir(const std::string& guest_prefix, const std::string& host_root) {
    g_k.mount_prefix = strip_slash(norm_path(guest_prefix));
    g_k.mount_root = host_root;
    while (!g_k.mount_root.empty() && g_k.mount_root.back() == '/')
        g_k.mount_root.pop_back();
    g_k.cwd = g_k.mount_prefix; // run the guest from the mount root
}

} // namespace k32web
