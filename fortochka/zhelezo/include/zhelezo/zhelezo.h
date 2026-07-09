// zhelezo — IA-32 user-mode interpreter, tier 0.
// Boring, correct, fully instrumentable. Part of Fortochka (MIT).
//
// Design contract (FORTOCHKA.md):
//  - Guest memory is identity-mapped: guest virtual address == offset into
//    the arena at Bus::base. No paging, no protection; every access is
//    bounds-checked against Bus::size and faults precisely.
//  - EIP entering [HOSTCALL_BASE, HOSTCALL_END) exits the run loop; peload
//    binds each import's IAT slot to a unique address in that window and the
//    host performs the call (arg lift from guest stack, EAX return, resume).
//  - zhelezo knows nothing about PE, DLLs, or Windows. CPU only.
#pragma once

#include <cstdint>

namespace zhelezo {

struct Bus {
    uint8_t* base = nullptr;
    uint32_t size = 0; // accesses at [addr, addr+width) outside [0,size) fault
};

enum GprIndex { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

// EFLAGS bits
inline constexpr uint32_t FLAG_CF = 1u << 0;
inline constexpr uint32_t FLAG_PF = 1u << 2;
inline constexpr uint32_t FLAG_AF = 1u << 4;
inline constexpr uint32_t FLAG_ZF = 1u << 6;
inline constexpr uint32_t FLAG_SF = 1u << 7;
inline constexpr uint32_t FLAG_TF = 1u << 8;
inline constexpr uint32_t FLAG_IF = 1u << 9;
inline constexpr uint32_t FLAG_DF = 1u << 10;
inline constexpr uint32_t FLAG_OF = 1u << 11;
// Reserved bit 1 always reads 1; IF stays set (user mode, no interrupts).
inline constexpr uint32_t EFLAGS_FIXED = 0x2u | FLAG_IF;
inline constexpr uint32_t EFLAGS_WRITABLE =
    FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_DF | FLAG_OF;

inline constexpr uint32_t HOSTCALL_BASE = 0xF0000000u;
inline constexpr uint32_t HOSTCALL_END  = 0xF1000000u;

// x87 FPU state. Tier 0 models the register stack as f64, not 80-bit extended —
// results can differ from real x87 in the last mantissa bits. Single-player has
// no desync partner, so this is acceptable; `used` flags any run that touched
// x87 for the determinism ledger (matters only if lockstep MP ever wants it).
struct X87 {
    double st[8] = {};      // physical registers; st(i) = st[(top + i) & 7]
    uint16_t control = 0x037F; // default: round-nearest, 64-bit precision, masked
    uint16_t status = 0;    // C0/C1/C2/C3 condition flags + TOP field
    uint8_t top = 0;        // top-of-stack pointer (0..7)
    uint8_t tag_empty = 0xFF; // bit i set → physical reg i is empty
    bool used = false;      // any x87 op executed this run
};

struct Cpu {
    uint32_t gpr[8] = {};
    uint32_t eip = 0;
    uint32_t eflags = EFLAGS_FIXED;
    uint32_t fs_base = 0; // TIB: SEH chain lives at fs:[0]
    uint32_t gs_base = 0;
    uint64_t icount = 0;  // instructions retired; also the RDTSC source
    X87 x87;
};

enum class Exit : uint8_t {
    Steps,    // step budget exhausted — normal timeslice end
    Hostcall, // EIP landed in the hostcall window (eip = slot address)
    Hlt,      // HLT — corpus binaries use it as "test finished"
    Int3,     // CC breakpoint (eip after the instruction)
    IntN,     // INT imm8 / INTO — vector in RunResult (eip after)
    Fault,    // precise CPU exception — eip points AT the faulting instruction
};

enum class FaultKind : uint8_t {
    None,
    Ud,       // undefined/unimplemented opcode
    De,       // divide error
    MemRead,  // out-of-arena load
    MemWrite, // out-of-arena store
    MemExec,  // out-of-arena fetch
};

struct RunResult {
    Exit exit = Exit::Steps;
    FaultKind fault = FaultKind::None;
    uint8_t vector = 0;      // IntN vector
    uint32_t fault_addr = 0; // faulting guest address for Mem* faults
    uint64_t steps = 0;      // instructions retired during this call
};

// Opaque per-CPU decode cache: memoizes decoded instructions by EIP, validated
// against the live bytes so self-modifying code stays correct. Passing one to
// run() skips re-decoding hot blocks. Owned by the caller; one per guest whose
// memory is independent.
class DecodeCache;
DecodeCache* decode_cache_new();
void decode_cache_free(DecodeCache*);
// Drop all entries. INVARIANT (Codex-reviewed): byte-validation makes the cache
// self-correct under self-modifying code, but NOT under unmap/permission change
// — a page that is unmapped while its backing bytes survive would still hit.
// Once k32web adds VirtualFree/unmap or execute-permission changes, call this on
// every such transition (and teach fresh decode to honor map state).
void decode_cache_clear(DecodeCache*);

// Execute until an exit condition or max_steps instructions. Passing a cache is
// a pure speedup; nullptr decodes every instruction fresh (identical results).
RunResult run(Cpu& cpu, const Bus& bus, uint64_t max_steps,
              DecodeCache* cache = nullptr);

// Execute exactly one instruction (the permanent debugger primitive).
RunResult step(Cpu& cpu, const Bus& bus);

} // namespace zhelezo
