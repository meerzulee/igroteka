#include "peload/peload.h"

#include <cstring>

namespace peload {

namespace {

struct Reader {
    const uint8_t* p;
    size_t n;

    uint16_t u16(size_t off) const {
        if (off + 2 > n) throw LoadError("truncated PE (u16 read)");
        uint16_t v;
        std::memcpy(&v, p + off, 2);
        return v;
    }
    uint32_t u32(size_t off) const {
        if (off + 4 > n) throw LoadError("truncated PE (u32 read)");
        uint32_t v;
        std::memcpy(&v, p + off, 4);
        return v;
    }
};

struct Section {
    uint32_t vaddr, vsize, rawptr, rawsize;
};

// Guest-side accessors: the image is already mapped, so RVAs resolve through
// the arena directly.
struct GuestView {
    uint8_t* arena;
    uint32_t arena_size;
    uint32_t base;

    void checkVa(uint32_t va, uint32_t len) const {
        if (va >= arena_size || arena_size - va < len)
            throw LoadError("image reference outside arena");
    }
    uint32_t u32(uint32_t va) const {
        checkVa(va, 4);
        uint32_t v;
        std::memcpy(&v, arena + va, 4);
        return v;
    }
    void put32(uint32_t va, uint32_t v) {
        checkVa(va, 4);
        std::memcpy(arena + va, &v, 4);
    }
    std::string cstr(uint32_t va) const {
        std::string s;
        while (true) {
            checkVa(va, 1);
            char c = (char)arena[va++];
            if (!c) break;
            s.push_back(c);
            if (s.size() > 4096) throw LoadError("unterminated string in image");
        }
        return s;
    }
};

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

} // namespace

Image load(uint8_t* arena, uint32_t arena_size, const uint8_t* file,
           size_t file_len, uint32_t hostcall_base, uint32_t& next_slot,
           uint32_t force_base) {
    Reader f{file, file_len};

    if (f.u16(0) != 0x5A4D) throw LoadError("no MZ header");
    const uint32_t pe = f.u32(0x3C);
    if (f.u32(pe) != 0x00004550) throw LoadError("no PE signature");

    const uint32_t coff = pe + 4;
    const uint16_t machine = f.u16(coff);
    if (machine != 0x014C) throw LoadError("not i386");
    const uint16_t nsects = f.u16(coff + 2);
    const uint16_t opt_size = f.u16(coff + 16);

    const uint32_t opt = coff + 20;
    if (f.u16(opt) != 0x10B) throw LoadError("not PE32");
    const uint32_t entry_rva = f.u32(opt + 16);
    const uint32_t pref_base = f.u32(opt + 28);
    const uint32_t size_of_image = f.u32(opt + 56);
    const uint32_t size_of_headers = f.u32(opt + 60);
    const uint32_t ndirs = f.u32(opt + 92);
    auto dir = [&](uint32_t i) -> std::pair<uint32_t, uint32_t> {
        if (i >= ndirs) return {0, 0};
        return {f.u32(opt + 96 + 8 * i), f.u32(opt + 96 + 8 * i + 4)};
    };

    Image img;
    img.base = force_base ? force_base : pref_base;
    img.size = size_of_image;
    img.entry = entry_rva ? img.base + entry_rva : 0;

    if (img.base >= arena_size || arena_size - img.base < size_of_image)
        throw LoadError("image does not fit in arena at chosen base");

    // Headers + sections. The arena is caller-zeroed; vsize > rawsize tails
    // (.bss) need no explicit clearing.
    if (size_of_headers > file_len) throw LoadError("SizeOfHeaders > file");
    std::memcpy(arena + img.base, file, size_of_headers);

    const uint32_t sect0 = opt + opt_size;
    std::vector<Section> sections;
    for (uint32_t i = 0; i < nsects; i++) {
        const uint32_t s = sect0 + 40 * i;
        Section sec{f.u32(s + 12), f.u32(s + 8), f.u32(s + 20), f.u32(s + 16)};
        const uint32_t copy = sec.rawsize < sec.vsize ? sec.rawsize : sec.vsize;
        if (copy) {
            if (sec.rawptr + (uint64_t)copy > file_len)
                throw LoadError("section raw data outside file");
            if (sec.vaddr + (uint64_t)copy > size_of_image)
                throw LoadError("section extends past SizeOfImage");
            std::memcpy(arena + img.base + sec.vaddr, file + sec.rawptr, copy);
        }
        sections.push_back(sec);
    }

    GuestView g{arena, arena_size, img.base};

    // Base relocations — only when we moved the image.
    const int32_t delta = (int32_t)(img.base - pref_base);
    if (delta != 0) {
        auto [rva, size] = dir(5);
        if (!rva) throw LoadError("image must relocate but has no .reloc");
        uint32_t p = img.base + rva, end = p + size;
        while (p + 8 <= end) {
            const uint32_t page = g.u32(p), block = g.u32(p + 4);
            if (block < 8) throw LoadError("bad reloc block");
            for (uint32_t e = p + 8; e + 2 <= p + block; e += 2) {
                g.checkVa(e, 2);
                uint16_t ent;
                std::memcpy(&ent, arena + e, 2);
                const uint16_t type = ent >> 12, off = ent & 0xFFF;
                if (type == 3) { // IMAGE_REL_BASED_HIGHLOW
                    const uint32_t va = img.base + page + off;
                    g.put32(va, g.u32(va) + (uint32_t)delta);
                } else if (type != 0) {
                    throw LoadError("unsupported reloc type");
                }
            }
            p += block;
        }
    }

    // Imports: patch every IAT slot with a hostcall address.
    if (auto [rva, size] = dir(1); rva) {
        (void)size;
        uint32_t d = img.base + rva;
        while (true) {
            const uint32_t ilt = g.u32(d), name_rva = g.u32(d + 12),
                           iat = g.u32(d + 16);
            if (!name_rva) break;
            const std::string dll = lower(g.cstr(img.base + name_rva));
            uint32_t src = img.base + (ilt ? ilt : iat);
            uint32_t dst = img.base + iat;
            while (true) {
                const uint32_t ent = g.u32(src);
                if (!ent) break;
                Import imp;
                imp.dll = dll;
                if (ent & 0x80000000u) imp.ordinal = (uint16_t)(ent & 0xFFFF);
                else imp.name = g.cstr(img.base + ent + 2);
                imp.iat_va = dst;
                imp.slot = next_slot++;
                g.put32(dst, hostcall_base + imp.slot * 16);
                img.imports.push_back(std::move(imp));
                src += 4;
                dst += 4;
            }
            d += 20;
        }
    }

    // TLS callbacks (rare in era exes, but they run before the entry point).
    if (auto [rva, size] = dir(9); rva) {
        (void)size;
        // AddressOfCallBacks is a VA at preferred base; adjust if relocated.
        uint32_t cb_va = g.u32(img.base + rva + 12);
        if (cb_va) {
            cb_va += (uint32_t)delta;
            while (uint32_t cb = g.u32(cb_va)) {
                img.tls_callbacks.push_back(cb + (uint32_t)delta);
                cb_va += 4;
            }
        }
    }

    return img;
}

} // namespace peload
