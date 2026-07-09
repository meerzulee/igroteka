// IA-32 decoder: prefixes, opcode tables, ModRM/SIB, immediates.
// Scope: the 32-bit user-mode subset a 2004 game binary exercises.
// Unknown encodings decode as UD and fault precisely at execution.
#include "insn.h"

namespace zhelezo {

namespace {

// Per-opcode decode metadata.
enum : uint8_t {
    M = 1 << 0,   // ModRM byte follows
    I8 = 1 << 1,  // 8-bit immediate
    IW = 1 << 2,  // 16/32-bit immediate by operand size
    I16 = 1 << 3, // 16-bit immediate always (RET imm16)
    R8 = 1 << 4,  // rel8 branch displacement
    RW = 1 << 5,  // rel16/32 branch displacement by operand size
    IA = 1 << 6,  // 32-bit moffs absolute address (A0–A3)
    X = 1 << 7,   // invalid / out of tier-0 scope -> #UD at exec
};

// clang-format off
constexpr uint8_t kOne[256] = {
    /*00*/ M, M, M, M, I8, IW, X, X,       /*08*/ M, M, M, M, I8, IW, X, 0,
    /*10*/ M, M, M, M, I8, IW, X, X,       /*18*/ M, M, M, M, I8, IW, X, X,
    /*20*/ M, M, M, M, I8, IW, 0, X,       /*28*/ M, M, M, M, I8, IW, 0, X,
    /*30*/ M, M, M, M, I8, IW, 0, X,       /*38*/ M, M, M, M, I8, IW, 0, X,
    /*40*/ 0, 0, 0, 0, 0, 0, 0, 0,         /*48*/ 0, 0, 0, 0, 0, 0, 0, 0,
    /*50*/ 0, 0, 0, 0, 0, 0, 0, 0,         /*58*/ 0, 0, 0, 0, 0, 0, 0, 0,
    /*60*/ 0, 0, X, X, 0, 0, 0, 0,         /*68*/ IW, M|IW, I8, M|I8, X, X, X, X,
    /*70*/ R8, R8, R8, R8, R8, R8, R8, R8, /*78*/ R8, R8, R8, R8, R8, R8, R8, R8,
    /*80*/ M|I8, M|IW, M|I8, M|I8,         /*84*/ M, M, M, M,
    /*88*/ M, M, M, M, M, M, M, M,
    /*90*/ 0, 0, 0, 0, 0, 0, 0, 0,         /*98*/ 0, 0, X, 0, 0, 0, 0, 0,
    /*A0*/ IA, IA, IA, IA, 0, 0, 0, 0,     /*A8*/ I8, IW, 0, 0, 0, 0, 0, 0,
    /*B0*/ I8, I8, I8, I8, I8, I8, I8, I8, /*B8*/ IW, IW, IW, IW, IW, IW, IW, IW,
    /*C0*/ M|I8, M|I8, I16, 0, X, X, M|I8, M|IW,
    /*C8*/ X, 0, X, X, 0, I8, 0, X,
    /*D0*/ M, M, M, M, X, X, 0, 0,
    /*D8*/ M, M, M, M, M, M, M, M,          // x87 escapes: length-correct, UD at exec
    /*E0*/ R8, R8, R8, R8, X, X, X, X,     /*E8*/ RW, RW, X, R8, X, X, X, X,
    /*F0*/ 0, X, 0, 0, 0, 0, M, M,         /*F8*/ 0, 0, 0, 0, 0, 0, M, M,
};

constexpr uint8_t kTwo[256] = {
    /*00*/ X, X, X, X, X, X, X, X,         /*08*/ X, X, X, 0 /*UD2*/, X, X, X, X,
    /*10*/ X, X, X, X, X, X, X, X,         /*18*/ M, X, X, X, X, X, X, M, // 18=hint-nop, 1F=long nop
    /*20*/ X, X, X, X, X, X, X, X,         /*28*/ X, X, X, X, X, X, X, X,
    /*30*/ X, 0 /*rdtsc*/, X, X, X, X, X, X, /*38*/ X, X, X, X, X, X, X, X,
    /*40*/ M, M, M, M, M, M, M, M,         /*48*/ M, M, M, M, M, M, M, M, // cmovcc
    /*50*/ X, X, X, X, X, X, X, X,         /*58*/ X, X, X, X, X, X, X, X,
    /*60*/ X, X, X, X, X, X, X, X,         /*68*/ X, X, X, X, X, X, X, X,
    /*70*/ X, X, X, X, X, X, X, X,         /*78*/ X, X, X, X, X, X, X, X,
    /*80*/ RW, RW, RW, RW, RW, RW, RW, RW, /*88*/ RW, RW, RW, RW, RW, RW, RW, RW,
    /*90*/ M, M, M, M, M, M, M, M,         /*98*/ M, M, M, M, M, M, M, M, // setcc
    /*A0*/ 0, 0, 0, M, M|I8, M, X, X,      /*A8*/ 0, 0, X, M, M|I8, M, X, M,
    /*B0*/ M, M, X, M, X, X, M, M,         /*B8*/ X, X, M|I8, M, M, M, M, M,
    /*C0*/ M, M, X, X, X, X, X, M,         /*C8*/ 0, 0, 0, 0, 0, 0, 0, 0, // bswap
    /*D0*/ X, X, X, X, X, X, X, X,         /*D8*/ X, X, X, X, X, X, X, X,
    /*E0*/ X, X, X, X, X, X, X, X,         /*E8*/ X, X, X, X, X, X, X, X,
    /*F0*/ X, X, X, X, X, X, X, X,         /*F8*/ X, X, X, X, X, X, X, X,
};
// clang-format on

struct Fetch {
    const Bus& bus;
    uint32_t pos;
    uint32_t start;

    uint8_t u8() {
        if (pos >= bus.size) throw MemFault{FaultKind::MemExec, pos};
        if (pos - start >= 15) throw UdFault{}; // x86 hard length limit
        return bus.base[pos++];
    }
    uint16_t u16() { return u8() | (uint16_t)(u8() << 8); }
    uint32_t u32() {
        uint32_t v = u16();
        return v | ((uint32_t)u16() << 16);
    }
};

} // namespace

void decode(const Bus& bus, uint32_t eip, Inst& out) {
    out = Inst{};
    Fetch f{bus, eip, eip};

    // Prefix loop.
    uint8_t b;
    for (;;) {
        b = f.u8();
        switch (b) {
            case 0x26: out.seg = SEG_ES; continue;
            case 0x2E: out.seg = SEG_CS; continue;
            case 0x36: out.seg = SEG_SS; continue;
            case 0x3E: out.seg = SEG_DS; continue;
            case 0x64: out.seg = SEG_FS; continue;
            case 0x65: out.seg = SEG_GS; continue;
            case 0x66: out.opsize = 2; continue;
            case 0x67: throw UdFault{}; // 16-bit addressing: era code never
            case 0xF0: out.lock = true; continue;
            case 0xF2: out.repne = true; continue;
            case 0xF3: out.rep = true; continue;
            default: break;
        }
        break;
    }

    uint8_t meta;
    if (b == 0x0F) {
        out.twobyte = true;
        out.op = f.u8();
        meta = kTwo[out.op];
    } else {
        out.op = b;
        meta = kOne[out.op];
    }
    // X still decodes ModRM-free as length 1; exec raises UD at a precise EIP.

    if (meta & M) {
        uint8_t m = f.u8();
        out.has_modrm = true;
        out.mod = m >> 6;
        out.reg = (m >> 3) & 7;
        out.rm = m & 7;
        if (out.mod != 3) {
            if (out.rm == 4) {
                uint8_t s = f.u8();
                out.has_sib = true;
                out.sib_scale = s >> 6;
                out.sib_index = (s >> 3) & 7;
                out.sib_base = s & 7;
            }
            if (out.mod == 1) {
                out.disp = (int8_t)f.u8();
            } else if (out.mod == 2 || (out.mod == 0 && out.rm == 5) ||
                       (out.mod == 0 && out.has_sib && out.sib_base == 5)) {
                out.disp = (int32_t)f.u32();
            }
        }
    }

    // Group 3 (F6/F7): TEST /0 and /1 carry an immediate, the rest do not.
    if (!out.twobyte && (out.op == 0xF6 || out.op == 0xF7) && out.reg >= 2)
        meta &= ~(I8 | IW);

    if (meta & I8) out.imm = f.u8();
    if (meta & I16) out.imm = f.u16();
    if (meta & IW) out.imm = out.opsize == 2 ? f.u16() : f.u32();
    if (meta & IA) out.imm = f.u32();
    if (meta & R8) out.imm = (uint32_t)(int32_t)(int8_t)f.u8();
    if (meta & RW)
        out.imm = out.opsize == 2 ? (uint32_t)(int32_t)(int16_t)f.u16() : f.u32();

    out.len = (uint8_t)(f.pos - eip);
}

} // namespace zhelezo
