// peload — PE32 loader: map sections, apply relocations, bind imports to
// hostcall slots. Knows nothing about x86 or Windows semantics; it moves
// bytes and reports what the image asked for. Part of Fortochka (MIT).
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace peload {

struct Import {
    std::string dll;      // lowercase, e.g. "kernel32.dll"
    std::string name;     // empty when imported by ordinal
    uint16_t ordinal = 0; // meaningful only when name is empty
    uint32_t iat_va = 0;  // guest VA of the IAT slot we patched
    uint32_t slot = 0;    // hostcall slot index assigned to this import
};

struct Image {
    uint32_t base = 0;  // where the image actually landed
    uint32_t entry = 0; // guest VA of AddressOfEntryPoint (0 for DLL w/o entry)
    uint32_t size = 0;  // SizeOfImage
    std::vector<Import> imports;
    std::vector<uint32_t> tls_callbacks; // guest VAs; host runs them before entry
};

struct LoadError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Map a PE32 file into the guest arena at its preferred base (relocating via
// .reloc only if `force_base` is nonzero). Each import's IAT slot is patched
// to hostcall_base + slot*16; `next_slot` is shared across modules and
// advances as slots are handed out. Throws LoadError.
Image load(uint8_t* arena, uint32_t arena_size, const uint8_t* file,
           size_t file_len, uint32_t hostcall_base, uint32_t& next_slot,
           uint32_t force_base = 0);

} // namespace peload
