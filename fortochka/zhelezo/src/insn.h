// Decoded-instruction record + fault carriers. Internal to zhelezo.
#pragma once

#include <cstdint>

#include "zhelezo/zhelezo.h"

namespace zhelezo {

// Thrown by memory/fetch helpers; caught in run() for precise faults.
struct MemFault {
    FaultKind kind;
    uint32_t addr;
};
struct UdFault {};
struct DeFault {};

enum Seg : int8_t { SEG_NONE = -1, SEG_ES, SEG_CS, SEG_SS, SEG_DS, SEG_FS, SEG_GS };

struct Inst {
    uint32_t imm = 0;    // immediate, zero-extended as encoded; branch rel
                         // displacements are stored sign-extended
    uint32_t imm2 = 0;   // second immediate (ENTER, far forms) — rare
    int32_t disp = 0;    // ModRM displacement, sign-extended
    uint8_t op = 0;      // primary opcode byte
    bool twobyte = false;

    bool has_modrm = false;
    uint8_t mod = 0, reg = 0, rm = 0;
    bool has_sib = false;
    uint8_t sib_scale = 0, sib_index = 4, sib_base = 0; // index 4 = none

    uint8_t opsize = 4;  // 4, or 2 under 0x66
    int8_t seg = SEG_NONE;
    bool rep = false;    // F3
    bool repne = false;  // F2
    bool lock = false;   // F0 (single-threaded tier 0: decoded, ignored)
    uint8_t len = 0;     // total encoded length
};

// Decode one instruction at `eip`. Throws MemFault{MemExec} past arena end,
// UdFault on invalid encodings the table rejects outright.
void decode(const Bus& bus, uint32_t eip, Inst& out);

} // namespace zhelezo
