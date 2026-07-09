// IA-32 tier-0 interpreter: eager flags, precise faults, no surprises.
//
// Known tier-0 relaxations (each is a documented divergence, not an accident):
//  - Multi-write instructions (PUSHA, string ops) that fault mid-way leave
//    partial side effects; EIP is still precise. Era games fault on loads,
//    not stack ops, and SEH handlers don't resume into these.
//  - Undefined-by-spec flag results are computed deterministically (noted
//    per site) rather than left stale, so runs are reproducible.
//  - LOCK is decoded and ignored: tier 0 is single-threaded by design.
#include <array>
#include <cstring>
#include <vector>

#include "insn.h"

namespace zhelezo {
namespace {

// ---------------- memory ----------------

inline void check(const Bus& b, uint32_t a, uint32_t n, FaultKind k) {
    if (a >= b.size || b.size - a < n) throw MemFault{k, a};
}
inline uint32_t rdW(const Bus& b, uint32_t a, int w) {
    check(b, a, (uint32_t)w, FaultKind::MemRead);
    uint32_t v = 0;
    std::memcpy(&v, b.base + a, (size_t)w);
    return v;
}
inline void wrW(const Bus& b, uint32_t a, int w, uint32_t v) {
    check(b, a, (uint32_t)w, FaultKind::MemWrite);
    std::memcpy(b.base + a, &v, (size_t)w);
}

// ---------------- registers ----------------

inline uint32_t getReg(const Cpu& c, unsigned r, int w) {
    if (w == 4) return c.gpr[r];
    if (w == 2) return c.gpr[r] & 0xFFFFu;
    return r < 4 ? (c.gpr[r] & 0xFFu) : ((c.gpr[r - 4] >> 8) & 0xFFu);
}
inline void setReg(Cpu& c, unsigned r, int w, uint32_t v) {
    if (w == 4) {
        c.gpr[r] = v;
    } else if (w == 2) {
        c.gpr[r] = (c.gpr[r] & 0xFFFF0000u) | (v & 0xFFFFu);
    } else if (r < 4) {
        c.gpr[r] = (c.gpr[r] & ~0xFFu) | (v & 0xFFu);
    } else {
        c.gpr[r - 4] = (c.gpr[r - 4] & ~0xFF00u) | ((v & 0xFFu) << 8);
    }
}

// ---------------- flags ----------------

constexpr std::array<uint8_t, 256> kParity = [] {
    std::array<uint8_t, 256> t{};
    for (int i = 0; i < 256; i++) {
        int bits = 0;
        for (int j = 0; j < 8; j++) bits += (i >> j) & 1;
        t[(size_t)i] = (bits & 1) ? 0 : 1; // PF = even parity of low byte
    }
    return t;
}();

inline uint32_t maskW(int w) { return w == 4 ? 0xFFFFFFFFu : (w == 2 ? 0xFFFFu : 0xFFu); }
inline unsigned msbW(int w) { return (unsigned)(w * 8 - 1); }

inline void setZSP(Cpu& c, uint32_t res, int w) {
    res &= maskW(w);
    c.eflags &= ~(FLAG_ZF | FLAG_SF | FLAG_PF);
    if (res == 0) c.eflags |= FLAG_ZF;
    if ((res >> msbW(w)) & 1) c.eflags |= FLAG_SF;
    if (kParity[res & 0xFF]) c.eflags |= FLAG_PF;
}
inline uint32_t setLogic(Cpu& c, uint32_t res, int w) {
    res &= maskW(w);
    c.eflags &= ~(FLAG_CF | FLAG_OF | FLAG_AF); // AF undefined by spec: cleared
    setZSP(c, res, w);
    return res;
}
inline uint32_t doAdd(Cpu& c, uint32_t a, uint32_t b, uint32_t cin, int w) {
    a &= maskW(w);
    b &= maskW(w);
    uint64_t wide = (uint64_t)a + b + cin;
    uint32_t res = (uint32_t)wide & maskW(w);
    c.eflags &= ~(FLAG_CF | FLAG_OF | FLAG_AF);
    if (wide > maskW(w)) c.eflags |= FLAG_CF;
    if ((((~(a ^ b)) & (a ^ res)) >> msbW(w)) & 1) c.eflags |= FLAG_OF;
    if ((a ^ b ^ res) & 0x10) c.eflags |= FLAG_AF;
    setZSP(c, res, w);
    return res;
}
inline uint32_t doSub(Cpu& c, uint32_t a, uint32_t b, uint32_t bin, int w) {
    a &= maskW(w);
    b &= maskW(w);
    uint32_t res = (a - b - bin) & maskW(w);
    c.eflags &= ~(FLAG_CF | FLAG_OF | FLAG_AF);
    if ((uint64_t)b + bin > a) c.eflags |= FLAG_CF;
    if ((((a ^ b) & (a ^ res)) >> msbW(w)) & 1) c.eflags |= FLAG_OF;
    if ((a ^ b ^ res) & 0x10) c.eflags |= FLAG_AF;
    setZSP(c, res, w);
    return res;
}

inline bool cond(const Cpu& c, unsigned cc) {
    const uint32_t f = c.eflags;
    bool r;
    switch (cc >> 1) {
        case 0: r = f & FLAG_OF; break;
        case 1: r = f & FLAG_CF; break;
        case 2: r = f & FLAG_ZF; break;
        case 3: r = (f & FLAG_CF) || (f & FLAG_ZF); break;
        case 4: r = f & FLAG_SF; break;
        case 5: r = f & FLAG_PF; break;
        case 6: r = !!(f & FLAG_SF) != !!(f & FLAG_OF); break;
        default: r = (f & FLAG_ZF) || (!!(f & FLAG_SF) != !!(f & FLAG_OF)); break;
    }
    return (cc & 1) ? !r : r;
}

// ---------------- operands ----------------

struct RM {
    bool mem;
    uint32_t addr;
    uint8_t reg;
};
inline RM resolveRM(const Cpu& c, const Inst& in) {
    if (in.mod == 3) return {false, 0, in.rm};
    uint32_t a = (uint32_t)in.disp;
    if (in.has_sib) {
        if (!(in.sib_base == 5 && in.mod == 0)) a += c.gpr[in.sib_base];
        if (in.sib_index != 4) a += c.gpr[in.sib_index] << in.sib_scale;
    } else if (!(in.mod == 0 && in.rm == 5)) {
        a += c.gpr[in.rm];
    }
    if (in.seg == SEG_FS) a += c.fs_base;
    else if (in.seg == SEG_GS) a += c.gs_base;
    return {true, a, 0};
}
inline uint32_t rmRead(const Cpu& c, const Bus& b, const RM& rm, int w) {
    return rm.mem ? rdW(b, rm.addr, w) : getReg(c, rm.reg, w);
}
inline void rmWrite(Cpu& c, const Bus& b, const RM& rm, int w, uint32_t v) {
    if (rm.mem) wrW(b, rm.addr, w, v);
    else setReg(c, rm.reg, w, v);
}

inline void push(Cpu& c, const Bus& b, uint32_t v, int w = 4) {
    uint32_t nsp = c.gpr[ESP] - (uint32_t)w;
    wrW(b, nsp, w, v); // write before commit: a faulting push leaves ESP intact
    c.gpr[ESP] = nsp;
}
inline uint32_t pop(Cpu& c, const Bus& b, int w = 4) {
    uint32_t v = rdW(b, c.gpr[ESP], w);
    c.gpr[ESP] += (uint32_t)w;
    return v;
}

// ---------------- ALU / shift bodies ----------------

// idx: 0 add, 1 or, 2 adc, 3 sbb, 4 and, 5 sub, 6 xor, 7 cmp
inline uint32_t aluOp(Cpu& c, unsigned idx, uint32_t a, uint32_t b, int w) {
    const uint32_t cf = (c.eflags & FLAG_CF) ? 1 : 0;
    switch (idx) {
        case 0: return doAdd(c, a, b, 0, w);
        case 1: return setLogic(c, a | b, w);
        case 2: return doAdd(c, a, b, cf, w);
        case 3: return doSub(c, a, b, cf, w);
        case 4: return setLogic(c, a & b, w);
        case 5: return doSub(c, a, b, 0, w);
        case 6: return setLogic(c, a ^ b, w);
        default: return doSub(c, a, b, 0, w); // cmp: caller skips writeback
    }
}

// op: modrm.reg of C0/C1/D0-D3 — 0 rol, 1 ror, 2 rcl, 3 rcr, 4/6 shl, 5 shr, 7 sar
inline uint32_t doShift(Cpu& c, unsigned op, uint32_t v, unsigned count, int w) {
    count &= 31;
    const unsigned bits = (unsigned)w * 8;
    const uint32_t mask = maskW(w);
    v &= mask;
    if (count == 0) return v; // masked count 0: no flags touched
    uint32_t res = v, cf = 0;
    auto setCFOF = [&](uint32_t of) {
        c.eflags &= ~(FLAG_CF | FLAG_OF);
        if (cf) c.eflags |= FLAG_CF;
        if (of) c.eflags |= FLAG_OF; // OF: spec-defined for count==1; we
                                     // compute the count==1 formula always
    };
    switch (op) {
        case 0: { // rol — CF/OF only
            unsigned e = count % bits;
            res = e ? ((v << e) | (v >> (bits - e))) & mask : v;
            cf = res & 1;
            setCFOF(((res >> msbW(w)) & 1) ^ cf);
            return res;
        }
        case 1: { // ror
            unsigned e = count % bits;
            res = e ? ((v >> e) | (v << (bits - e))) & mask : v;
            cf = (res >> msbW(w)) & 1;
            setCFOF(((res >> msbW(w)) ^ (res >> (msbW(w) - 1))) & 1);
            return res;
        }
        case 2: { // rcl — width+1 bit rotate through CF
            cf = (c.eflags & FLAG_CF) ? 1 : 0;
            for (unsigned i = 0; i < count % (bits + 1); i++) {
                uint32_t ncf = (res >> msbW(w)) & 1;
                res = ((res << 1) | cf) & mask;
                cf = ncf;
            }
            setCFOF(((res >> msbW(w)) & 1) ^ cf);
            return res;
        }
        case 3: { // rcr
            cf = (c.eflags & FLAG_CF) ? 1 : 0;
            for (unsigned i = 0; i < count % (bits + 1); i++) {
                uint32_t ncf = res & 1;
                res = ((res >> 1) | (cf << msbW(w))) & mask;
                cf = ncf;
            }
            setCFOF(((res >> msbW(w)) ^ (res >> (msbW(w) - 1))) & 1);
            return res;
        }
        case 4:
        case 6: { // shl
            uint64_t wide = (uint64_t)v << count;
            res = (uint32_t)wide & mask;
            cf = count <= bits ? (uint32_t)(wide >> bits) & 1 : 0;
            setCFOF(((res >> msbW(w)) & 1) ^ cf);
            c.eflags &= ~FLAG_AF;
            setZSP(c, res, w);
            return res;
        }
        case 5: { // shr
            cf = count <= bits ? (v >> (count - 1)) & 1 : 0;
            res = count < bits ? v >> count : 0;
            setCFOF((v >> msbW(w)) & 1);
            c.eflags &= ~FLAG_AF;
            setZSP(c, res, w);
            return res;
        }
        default: { // sar
            int32_t sv = (int32_t)(v << (32 - bits)) >> (32 - bits); // sign-extend
            cf = (uint32_t)(count < bits ? (sv >> (count - 1)) : (sv >> (bits - 1))) & 1;
            res = (uint32_t)(count < bits ? sv >> count : sv >> (bits - 1)) & mask;
            setCFOF(0);
            c.eflags &= ~FLAG_AF;
            setZSP(c, res, w);
            return res;
        }
    }
}

// ---------------- string ops ----------------

inline uint32_t strDelta(const Cpu& c, int w) {
    return (c.eflags & FLAG_DF) ? (uint32_t)-w : (uint32_t)w;
}

// Executes one string instruction (with REP/REPE/REPNE if present).
// op is the primary opcode (A4..AF family).
inline void doString(Cpu& c, const Bus& b, const Inst& in) {
    const int w = (in.op & 1) ? in.opsize : 1;
    const uint32_t d = strDelta(c, w);
    const bool repAny = in.rep || in.repne;
    const uint8_t kind = (in.op >> 1) & 7; // 2=movs 3=cmps 5=stos 6=lods 7=scas
    // A6/A7 cmps, AE/AF scas terminate on ZF when REPE/REPNE present.
    for (;;) {
        if (repAny) {
            if (c.gpr[ECX] == 0) break;
        }
        switch (kind) {
            case 2: { // movs: [edi] <- [esi]
                wrW(b, c.gpr[EDI], w, rdW(b, c.gpr[ESI], w));
                c.gpr[ESI] += d;
                c.gpr[EDI] += d;
                break;
            }
            case 3: { // cmps: flags(esi - edi operands)
                uint32_t a = rdW(b, c.gpr[ESI], w), v = rdW(b, c.gpr[EDI], w);
                doSub(c, a, v, 0, w);
                c.gpr[ESI] += d;
                c.gpr[EDI] += d;
                break;
            }
            case 5: { // stos
                wrW(b, c.gpr[EDI], w, getReg(c, EAX, w));
                c.gpr[EDI] += d;
                break;
            }
            case 6: { // lods
                setReg(c, EAX, w, rdW(b, c.gpr[ESI], w));
                c.gpr[ESI] += d;
                break;
            }
            default: { // scas
                doSub(c, getReg(c, EAX, w), rdW(b, c.gpr[EDI], w), 0, w);
                c.gpr[EDI] += d;
                break;
            }
        }
        if (!repAny) break;
        c.gpr[ECX] -= 1;
        if (kind == 3 || kind == 7) { // cmps/scas honor the REPE/REPNE condition
            const bool z = (c.eflags & FLAG_ZF) != 0;
            if (in.rep && !z) break;
            if (in.repne && z) break;
        }
    }
}

// ---------------- mul / div ----------------

inline void doMulDiv(Cpu& c, unsigned reg, uint32_t src, int w) {
    const uint32_t mask = maskW(w);
    switch (reg) {
        case 4: { // mul: unsigned widening
            uint64_t p = (uint64_t)getReg(c, EAX, w) * (src & mask);
            uint32_t hi;
            if (w == 1) {
                setReg(c, EAX, 2, (uint32_t)p); // AX = AL*src
                hi = (uint32_t)(p >> 8) & 0xFF;
            } else {
                setReg(c, EAX, w, (uint32_t)p & mask);
                setReg(c, EDX, w, (uint32_t)(p >> (w * 8)) & mask);
                hi = (uint32_t)(p >> (w * 8)) & mask;
            }
            c.eflags &= ~(FLAG_CF | FLAG_OF);
            if (hi) c.eflags |= FLAG_CF | FLAG_OF;
            setZSP(c, (uint32_t)p, w); // ZSP undefined by spec: from low result
            return;
        }
        case 5: { // one-operand imul: signed widening
            auto sx = [&](uint32_t v) -> int64_t {
                return w == 1 ? (int8_t)v : w == 2 ? (int16_t)v : (int32_t)v;
            };
            int64_t p = sx(getReg(c, EAX, w)) * sx(src);
            if (w == 1) {
                setReg(c, EAX, 2, (uint32_t)p & 0xFFFF);
            } else {
                setReg(c, EAX, w, (uint32_t)p & mask);
                setReg(c, EDX, w, (uint32_t)((uint64_t)p >> (w * 8)) & mask);
            }
            int64_t lo = w == 1 ? (int8_t)p : w == 2 ? (int16_t)p : (int32_t)p;
            c.eflags &= ~(FLAG_CF | FLAG_OF);
            if (p != lo) c.eflags |= FLAG_CF | FLAG_OF;
            setZSP(c, (uint32_t)p, w);
            return;
        }
        case 6: { // div
            src &= mask;
            if (src == 0) throw DeFault{};
            uint64_t num = w == 1 ? getReg(c, EAX, 2)
                                  : ((uint64_t)getReg(c, EDX, w) << (w * 8)) |
                                        getReg(c, EAX, w);
            uint64_t q = num / src, r = num % src;
            if (q > mask) throw DeFault{};
            if (w == 1) {
                setReg(c, EAX, 1, (uint32_t)q);
                setReg(c, 4 /*AH*/, 1, (uint32_t)r);
            } else {
                setReg(c, EAX, w, (uint32_t)q);
                setReg(c, EDX, w, (uint32_t)r);
            }
            return; // flags undefined: left untouched
        }
        default: { // 7: idiv
            src &= mask;
            auto sx = [&](uint32_t v) -> int64_t {
                return w == 1 ? (int8_t)v : w == 2 ? (int16_t)v : (int32_t)v;
            };
            int64_t den = sx(src);
            if (den == 0) throw DeFault{};
            int64_t num;
            if (w == 1) {
                num = (int16_t)getReg(c, EAX, 2);
            } else if (w == 2) {
                num = (int32_t)((getReg(c, EDX, 2) << 16) | getReg(c, EAX, 2));
            } else {
                num = (int64_t)(((uint64_t)c.gpr[EDX] << 32) | c.gpr[EAX]);
            }
            int64_t q = num / den, r = num % den;
            int64_t qmin = -(int64_t)(mask >> 1) - 1, qmax = (int64_t)(mask >> 1);
            if (q < qmin || q > qmax) throw DeFault{};
            if (w == 1) {
                setReg(c, EAX, 1, (uint32_t)q & 0xFF);
                setReg(c, 4 /*AH*/, 1, (uint32_t)r & 0xFF);
            } else {
                setReg(c, EAX, w, (uint32_t)q & mask);
                setReg(c, EDX, w, (uint32_t)r & mask);
            }
            return;
        }
    }
}

// ---------------- BT family ----------------

// btOp: 0 bt, 1 bts, 2 btr, 3 btc. Returns via flags; may write back.
inline void doBt(Cpu& c, const Bus& b, const RM& rm, unsigned btOp, uint32_t off,
                 int w, bool immForm) {
    uint32_t addr = rm.addr;
    uint32_t bit;
    uint32_t v;
    if (rm.mem && !immForm) {
        // Register-offset form addresses the full bit string.
        int32_t so = (int32_t)off;
        addr += (uint32_t)((so >> (w == 2 ? 4 : 5)) * w);
        bit = (uint32_t)so & (w == 2 ? 15 : 31);
        v = rdW(b, addr, w);
    } else {
        bit = off & (unsigned)(w * 8 - 1);
        v = rm.mem ? rdW(b, addr, w) : getReg(c, rm.reg, w);
    }
    c.eflags &= ~FLAG_CF;
    if ((v >> bit) & 1) c.eflags |= FLAG_CF;
    if (btOp == 0) return;
    if (btOp == 1) v |= 1u << bit;
    else if (btOp == 2) v &= ~(1u << bit);
    else v ^= 1u << bit;
    if (rm.mem) wrW(b, addr, w, v);
    else setReg(c, rm.reg, w, v);
}

// ---------------- one instruction ----------------

// Returns Exit::Steps to continue.
RunResult exec1(Cpu& c, const Bus& b, const Inst& in, uint32_t next) {
    const int w = in.opsize; // W-width for this instruction
    c.eip = next;            // branches overwrite; faults restore in run()

    const uint8_t op = in.op;

    if (in.twobyte) {
        switch (op) {
            case 0x0B: throw UdFault{}; // UD2
            case 0x18:
            case 0x1F: return {}; // hint/long NOP
            case 0x31: {          // rdtsc — deterministic: icount is the TSC
                c.gpr[EAX] = (uint32_t)c.icount;
                c.gpr[EDX] = (uint32_t)(c.icount >> 32);
                return {};
            }
            case 0xA2: { // cpuid — Pentium III persona (family 6 model 8)
                switch (c.gpr[EAX]) {
                    case 0:
                        c.gpr[EAX] = 1;
                        c.gpr[EBX] = 0x756E6547; // "Genu"
                        c.gpr[EDX] = 0x49656E69; // "ineI"
                        c.gpr[ECX] = 0x6C65746E; // "ntel"
                        break;
                    case 1:
                        c.gpr[EAX] = 0x00000686;
                        c.gpr[EBX] = 0;
                        c.gpr[ECX] = 0;
                        // FPU TSC CX8 CMOV MMX FXSR SSE
                        c.gpr[EDX] = (1u << 0) | (1u << 4) | (1u << 8) |
                                     (1u << 15) | (1u << 23) | (1u << 24) |
                                     (1u << 25);
                        break;
                    default:
                        c.gpr[EAX] = c.gpr[EBX] = c.gpr[ECX] = c.gpr[EDX] = 0;
                        break;
                }
                return {};
            }
            case 0xA0: push(c, b, 0x3B); return {}; // push fs (dummy selector)
            case 0xA8: push(c, b, 0x00); return {}; // push gs
            case 0xA1:
            case 0xA9: (void)pop(c, b); return {}; // pop fs/gs: base unchanged
        }
        if (op >= 0x40 && op <= 0x4F) { // cmovcc r, rm
            RM rm = resolveRM(c, in);
            uint32_t v = rmRead(c, b, rm, w); // memory is read even if not taken
            if (cond(c, op & 15)) setReg(c, in.reg, w, v);
            return {};
        }
        if (op >= 0x80 && op <= 0x8F) { // jcc relW
            if (cond(c, op & 15)) c.eip = next + in.imm;
            return {};
        }
        if (op >= 0x90 && op <= 0x9F) { // setcc rm8
            RM rm = resolveRM(c, in);
            rmWrite(c, b, rm, 1, cond(c, op & 15) ? 1 : 0);
            return {};
        }
        if (op >= 0xC8) { // bswap r32
            uint32_t v = c.gpr[op & 7];
            c.gpr[op & 7] = __builtin_bswap32(v);
            return {};
        }
        switch (op) {
            case 0xA3: // bt rm, r
            case 0xAB: // bts
            case 0xB3: // btr
            case 0xBB: { // btc
                RM rm = resolveRM(c, in);
                unsigned k = op == 0xA3 ? 0 : op == 0xAB ? 1 : op == 0xB3 ? 2 : 3;
                doBt(c, b, rm, k, getReg(c, in.reg, w), w, false);
                return {};
            }
            case 0xBA: { // group 8: bt/bts/btr/btc rm, imm8
                if (in.reg < 4) throw UdFault{};
                RM rm = resolveRM(c, in);
                doBt(c, b, rm, in.reg - 4, in.imm, w, true);
                return {};
            }
            case 0xA4: // shld rm, r, imm8
            case 0xA5: // shld rm, r, cl
            case 0xAC: // shrd rm, r, imm8
            case 0xAD: { // shrd rm, r, cl
                RM rm = resolveRM(c, in);
                unsigned count = ((op & 1) ? c.gpr[ECX] : in.imm) & 31;
                uint32_t dst = rmRead(c, b, rm, w), src = getReg(c, in.reg, w);
                if (count == 0) return {};
                const unsigned bits = (unsigned)w * 8;
                if (count >= bits) return {}; // undefined; leave unchanged
                uint32_t res, cf;
                if (op < 0xA8) { // shld
                    res = ((dst << count) | (src >> (bits - count))) & maskW(w);
                    cf = (dst >> (bits - count)) & 1;
                } else { // shrd
                    res = ((dst >> count) | (src << (bits - count))) & maskW(w);
                    cf = (dst >> (count - 1)) & 1;
                }
                c.eflags &= ~(FLAG_CF | FLAG_OF | FLAG_AF);
                if (cf) c.eflags |= FLAG_CF;
                if (((res ^ dst) >> msbW(w)) & 1) c.eflags |= FLAG_OF;
                setZSP(c, res, w);
                rmWrite(c, b, rm, w, res);
                return {};
            }
            case 0xAF: { // imul r, rm
                RM rm = resolveRM(c, in);
                int64_t a = w == 2 ? (int16_t)getReg(c, in.reg, w)
                                   : (int32_t)getReg(c, in.reg, w);
                int64_t v = w == 2 ? (int16_t)rmRead(c, b, rm, w)
                                   : (int32_t)rmRead(c, b, rm, w);
                int64_t p = a * v;
                uint32_t res = (uint32_t)p & maskW(w);
                int64_t lo = w == 2 ? (int16_t)res : (int32_t)res;
                c.eflags &= ~(FLAG_CF | FLAG_OF);
                if (p != lo) c.eflags |= FLAG_CF | FLAG_OF;
                setZSP(c, res, w);
                setReg(c, in.reg, w, res);
                return {};
            }
            case 0xB0:   // cmpxchg rm8, r8
            case 0xB1: { // cmpxchg rmW, rW
                const int cw = op == 0xB0 ? 1 : w;
                RM rm = resolveRM(c, in);
                uint32_t dst = rmRead(c, b, rm, cw);
                uint32_t acc = getReg(c, EAX, cw);
                doSub(c, acc, dst, 0, cw);
                if (acc == dst) {
                    rmWrite(c, b, rm, cw, getReg(c, in.reg, cw));
                } else {
                    setReg(c, EAX, cw, dst);
                }
                return {};
            }
            case 0xC0:   // xadd rm8, r8
            case 0xC1: { // xadd rmW, rW
                const int cw = op == 0xC0 ? 1 : w;
                RM rm = resolveRM(c, in);
                uint32_t dst = rmRead(c, b, rm, cw), src = getReg(c, in.reg, cw);
                uint32_t sum = doAdd(c, dst, src, 0, cw);
                setReg(c, in.reg, cw, dst);
                rmWrite(c, b, rm, cw, sum);
                return {};
            }
            case 0xB6:   // movzx r, rm8
            case 0xB7:   // movzx r, rm16
            case 0xBE:   // movsx r, rm8
            case 0xBF: { // movsx r, rm16
                const int sw = (op & 1) ? 2 : 1;
                RM rm = resolveRM(c, in);
                uint32_t v = rmRead(c, b, rm, sw);
                if (op >= 0xBE)
                    v = sw == 1 ? (uint32_t)(int32_t)(int8_t)v
                                : (uint32_t)(int32_t)(int16_t)v;
                setReg(c, in.reg, w, v);
                return {};
            }
            case 0xBC:   // bsf
            case 0xBD: { // bsr
                RM rm = resolveRM(c, in);
                uint32_t v = rmRead(c, b, rm, w) & maskW(w);
                c.eflags &= ~FLAG_ZF;
                if (v == 0) {
                    c.eflags |= FLAG_ZF; // dest unchanged (real-CPU behavior)
                    return {};
                }
                unsigned idx = op == 0xBC ? (unsigned)__builtin_ctz(v)
                                          : 31u - (unsigned)__builtin_clz(v);
                setReg(c, in.reg, w, idx);
                return {};
            }
            case 0xC7: { // cmpxchg8b m64
                if (in.reg != 1 || in.mod == 3) throw UdFault{};
                RM rm = resolveRM(c, in);
                uint64_t cur = rdW(b, rm.addr, 4) |
                               ((uint64_t)rdW(b, rm.addr + 4, 4) << 32);
                uint64_t exp = c.gpr[EAX] | ((uint64_t)c.gpr[EDX] << 32);
                if (cur == exp) {
                    wrW(b, rm.addr, 4, c.gpr[EBX]);
                    wrW(b, rm.addr + 4, 4, c.gpr[ECX]);
                    c.eflags |= FLAG_ZF;
                } else {
                    c.gpr[EAX] = (uint32_t)cur;
                    c.gpr[EDX] = (uint32_t)(cur >> 32);
                    c.eflags &= ~FLAG_ZF;
                }
                return {};
            }
        }
        throw UdFault{};
    }

    // ---- one-byte map ----

    // ALU families 00-3D: idx = bits 5:3, form = bits 2:0 (0..5).
    if (op < 0x40 && (op & 7) <= 5) {
        const unsigned idx = (op >> 3) & 7;
        const unsigned form = op & 7;
        const int aw = (form & 1) ? w : 1;
        uint32_t res;
        switch (form) {
            case 0:
            case 1: { // rm, r
                RM rm = resolveRM(c, in);
                res = aluOp(c, idx, rmRead(c, b, rm, aw), getReg(c, in.reg, aw), aw);
                if (idx != 7) rmWrite(c, b, rm, aw, res);
                return {};
            }
            case 2:
            case 3: { // r, rm
                RM rm = resolveRM(c, in);
                res = aluOp(c, idx, getReg(c, in.reg, aw), rmRead(c, b, rm, aw), aw);
                if (idx != 7) setReg(c, in.reg, aw, res);
                return {};
            }
            default: { // AL/eAX, imm
                res = aluOp(c, idx, getReg(c, EAX, aw), in.imm, aw);
                if (idx != 7) setReg(c, EAX, aw, res);
                return {};
            }
        }
    }

    if (op >= 0x40 && op <= 0x4F) { // inc/dec r — CF preserved
        const uint32_t savedCF = c.eflags & FLAG_CF;
        unsigned r = op & 7;
        uint32_t v = getReg(c, r, w);
        uint32_t res = op < 0x48 ? doAdd(c, v, 1, 0, w) : doSub(c, v, 1, 0, w);
        setReg(c, r, w, res);
        c.eflags = (c.eflags & ~FLAG_CF) | savedCF;
        return {};
    }
    if (op >= 0x50 && op <= 0x57) { // push r
        push(c, b, getReg(c, op & 7, w), w);
        return {};
    }
    if (op >= 0x58 && op <= 0x5F) { // pop r
        setReg(c, op & 7, w, pop(c, b, w));
        return {};
    }
    if (op >= 0x70 && op <= 0x7F) { // jcc rel8
        if (cond(c, op & 15)) c.eip = next + in.imm;
        return {};
    }
    if (op >= 0x91 && op <= 0x97) { // xchg eAX, r
        uint32_t t = getReg(c, EAX, w);
        setReg(c, EAX, w, getReg(c, op & 7, w));
        setReg(c, op & 7, w, t);
        return {};
    }
    if (op >= 0xB0 && op <= 0xB7) { // mov r8, imm8
        setReg(c, op & 7, 1, in.imm);
        return {};
    }
    if (op >= 0xB8 && op <= 0xBF) { // mov rW, immW
        setReg(c, op & 7, w, in.imm);
        return {};
    }
    if (op >= 0xA4 && op <= 0xAF && op != 0xA8 && op != 0xA9) { // string family
        doString(c, b, in);
        return {};
    }
    if (op >= 0xD8 && op <= 0xDF) throw UdFault{}; // x87: F1-scope, not yet

    switch (op) {
        case 0x60: { // pusha
            uint32_t sp = c.gpr[ESP];
            for (int r = 0; r < 8; r++) push(c, b, r == ESP ? sp : c.gpr[r], w);
            return {};
        }
        case 0x61: { // popa (ESP slot discarded)
            for (int r = 7; r >= 0; r--) {
                uint32_t v = pop(c, b, w);
                if (r != ESP) setReg(c, (unsigned)r, w, v);
            }
            return {};
        }
        case 0x68: push(c, b, in.imm, w); return {}; // push immW
        case 0x6A: // push imm8 (sign-extended)
            push(c, b, (uint32_t)(int32_t)(int8_t)in.imm, w);
            return {};
        case 0x69:   // imul r, rm, immW
        case 0x6B: { // imul r, rm, imm8
            RM rm = resolveRM(c, in);
            int64_t a = w == 2 ? (int16_t)rmRead(c, b, rm, w)
                               : (int32_t)rmRead(c, b, rm, w);
            int64_t m = op == 0x6B ? (int8_t)in.imm
                                   : (w == 2 ? (int16_t)in.imm : (int32_t)in.imm);
            int64_t p = a * m;
            uint32_t res = (uint32_t)p & maskW(w);
            int64_t lo = w == 2 ? (int16_t)res : (int32_t)res;
            c.eflags &= ~(FLAG_CF | FLAG_OF);
            if (p != lo) c.eflags |= FLAG_CF | FLAG_OF;
            setZSP(c, res, w);
            setReg(c, in.reg, w, res);
            return {};
        }
        case 0x80:
        case 0x82: { // ALU rm8, imm8
            RM rm = resolveRM(c, in);
            uint32_t res = aluOp(c, in.reg, rmRead(c, b, rm, 1), in.imm, 1);
            if (in.reg != 7) rmWrite(c, b, rm, 1, res);
            return {};
        }
        case 0x81: { // ALU rmW, immW
            RM rm = resolveRM(c, in);
            uint32_t res = aluOp(c, in.reg, rmRead(c, b, rm, w), in.imm, w);
            if (in.reg != 7) rmWrite(c, b, rm, w, res);
            return {};
        }
        case 0x83: { // ALU rmW, imm8 sign-extended
            RM rm = resolveRM(c, in);
            uint32_t imm = (uint32_t)(int32_t)(int8_t)in.imm;
            uint32_t res = aluOp(c, in.reg, rmRead(c, b, rm, w), imm, w);
            if (in.reg != 7) rmWrite(c, b, rm, w, res);
            return {};
        }
        case 0x84:
        case 0x85: { // test rm, r
            const int aw = (op & 1) ? w : 1;
            RM rm = resolveRM(c, in);
            setLogic(c, rmRead(c, b, rm, aw) & getReg(c, in.reg, aw), aw);
            return {};
        }
        case 0x86:
        case 0x87: { // xchg rm, r
            const int aw = (op & 1) ? w : 1;
            RM rm = resolveRM(c, in);
            uint32_t t = rmRead(c, b, rm, aw);
            rmWrite(c, b, rm, aw, getReg(c, in.reg, aw));
            setReg(c, in.reg, aw, t);
            return {};
        }
        case 0x88:
        case 0x89: { // mov rm, r
            const int aw = (op & 1) ? w : 1;
            RM rm = resolveRM(c, in);
            rmWrite(c, b, rm, aw, getReg(c, in.reg, aw));
            return {};
        }
        case 0x8A:
        case 0x8B: { // mov r, rm
            const int aw = (op & 1) ? w : 1;
            RM rm = resolveRM(c, in);
            setReg(c, in.reg, aw, rmRead(c, b, rm, aw));
            return {};
        }
        case 0x8C: { // mov rm16, sreg — flat dummy selectors
            static const uint16_t sel[6] = {0x23, 0x1B, 0x23, 0x23, 0x3B, 0x00};
            if (in.reg >= 6) throw UdFault{};
            RM rm = resolveRM(c, in);
            rmWrite(c, b, rm, rm.mem ? 2 : w, sel[in.reg]);
            return {};
        }
        case 0x8E: { // mov sreg, rm16 — selector loads are meaningless here;
                     // fs/gs bases are set by the host (peload/k32web). Ignore.
            RM rm = resolveRM(c, in);
            (void)rmRead(c, b, rm, 2);
            return {};
        }
        case 0x8D: { // lea r, m
            if (in.mod == 3) throw UdFault{};
            RM rm = resolveRM(c, in);
            setReg(c, in.reg, w, rm.addr);
            return {};
        }
        case 0x8F: { // pop rm
            if (in.reg != 0) throw UdFault{};
            uint32_t v = pop(c, b, w);
            RM rm = resolveRM(c, in); // resolved after pop: [esp] forms use new ESP
            rmWrite(c, b, rm, w, v);
            return {};
        }
        case 0x90: return {}; // nop
        case 0x98: { // cwde / cbw
            if (w == 4) c.gpr[EAX] = (uint32_t)(int32_t)(int16_t)getReg(c, EAX, 2);
            else setReg(c, EAX, 2, (uint32_t)(int16_t)(int8_t)getReg(c, EAX, 1));
            return {};
        }
        case 0x99: { // cdq / cwd
            if (w == 4) c.gpr[EDX] = (uint32_t)((int32_t)c.gpr[EAX] >> 31);
            else setReg(c, EDX, 2, ((int16_t)getReg(c, EAX, 2)) < 0 ? 0xFFFF : 0);
            return {};
        }
        case 0x9B: return {}; // fwait
        case 0x9C: push(c, b, (c.eflags & ~(FLAG_TF)) | 0x2, w); return {}; // pushf
        case 0x9D: { // popf
            uint32_t v = pop(c, b, w);
            c.eflags = EFLAGS_FIXED | (v & EFLAGS_WRITABLE);
            return {};
        }
        case 0x9E: { // sahf
            uint32_t ah = getReg(c, 4 /*AH*/, 1);
            const uint32_t m = FLAG_SF | FLAG_ZF | FLAG_AF | FLAG_PF | FLAG_CF;
            c.eflags = (c.eflags & ~m) | (ah & m);
            return {};
        }
        case 0x9F: { // lahf
            uint32_t f = (c.eflags & (FLAG_SF | FLAG_ZF | FLAG_AF | FLAG_PF |
                                      FLAG_CF)) | 0x2;
            setReg(c, 4 /*AH*/, 1, f);
            return {};
        }
        case 0xA0:   // mov al, [moffs]
        case 0xA1: { // mov eAX, [moffs]
            uint32_t a = in.imm;
            if (in.seg == SEG_FS) a += c.fs_base;
            else if (in.seg == SEG_GS) a += c.gs_base;
            setReg(c, EAX, op == 0xA0 ? 1 : w, rdW(b, a, op == 0xA0 ? 1 : w));
            return {};
        }
        case 0xA2:   // mov [moffs], al
        case 0xA3: { // mov [moffs], eAX
            uint32_t a = in.imm;
            if (in.seg == SEG_FS) a += c.fs_base;
            else if (in.seg == SEG_GS) a += c.gs_base;
            wrW(b, a, op == 0xA2 ? 1 : w, getReg(c, EAX, op == 0xA2 ? 1 : w));
            return {};
        }
        case 0xA8: setLogic(c, getReg(c, EAX, 1) & in.imm, 1); return {}; // test al
        case 0xA9: setLogic(c, getReg(c, EAX, w) & in.imm, w); return {}; // test eAX
        case 0xC0:
        case 0xC1:
        case 0xD0:
        case 0xD1:
        case 0xD2:
        case 0xD3: { // shift group
            const int aw = (op & 1) ? w : 1;
            unsigned count = op <= 0xC1 ? in.imm
                             : op <= 0xD1 ? 1
                                          : (c.gpr[ECX] & 0xFF);
            RM rm = resolveRM(c, in);
            uint32_t res = doShift(c, in.reg, rmRead(c, b, rm, aw), count, aw);
            rmWrite(c, b, rm, aw, res);
            return {};
        }
        case 0xC2: { // ret imm16
            c.eip = pop(c, b);
            c.gpr[ESP] += in.imm;
            return {};
        }
        case 0xC3: c.eip = pop(c, b); return {}; // ret
        case 0xC6:
        case 0xC7: { // mov rm, imm
            if (in.reg != 0) throw UdFault{};
            const int aw = (op & 1) ? w : 1;
            RM rm = resolveRM(c, in);
            rmWrite(c, b, rm, aw, in.imm);
            return {};
        }
        case 0xC9: { // leave
            c.gpr[ESP] = c.gpr[EBP];
            c.gpr[EBP] = pop(c, b);
            return {};
        }
        case 0xCC: return {Exit::Int3};
        case 0xCD: return {Exit::IntN, FaultKind::None, (uint8_t)in.imm};
        case 0xCE: // into
            if (c.eflags & FLAG_OF) return {Exit::IntN, FaultKind::None, 4};
            return {};
        case 0xD6: setReg(c, EAX, 1, (c.eflags & FLAG_CF) ? 0xFF : 0); return {};
        case 0xD7: { // xlat: al = [ebx + zx(al)]
            uint32_t a = c.gpr[EBX] + getReg(c, EAX, 1);
            if (in.seg == SEG_FS) a += c.fs_base;
            else if (in.seg == SEG_GS) a += c.gs_base;
            setReg(c, EAX, 1, rdW(b, a, 1));
            return {};
        }
        case 0xE0:   // loopne
        case 0xE1:   // loope
        case 0xE2: { // loop
            c.gpr[ECX] -= 1;
            bool take = c.gpr[ECX] != 0;
            if (op == 0xE0) take = take && !(c.eflags & FLAG_ZF);
            if (op == 0xE1) take = take && (c.eflags & FLAG_ZF);
            if (take) c.eip = next + in.imm;
            return {};
        }
        case 0xE3: // jecxz
            if (c.gpr[ECX] == 0) c.eip = next + in.imm;
            return {};
        case 0xE8: { // call relW
            push(c, b, next);
            c.eip = next + in.imm;
            return {};
        }
        case 0xE9:
        case 0xEB: c.eip = next + in.imm; return {}; // jmp
        case 0xF4: return {Exit::Hlt};
        case 0xF5: c.eflags ^= FLAG_CF; return {}; // cmc
        case 0xF6:
        case 0xF7: { // group 3
            const int aw = (op & 1) ? w : 1;
            RM rm = resolveRM(c, in);
            switch (in.reg) {
                case 0:
                case 1: // test rm, imm
                    setLogic(c, rmRead(c, b, rm, aw) & in.imm, aw);
                    return {};
                case 2: // not — no flags
                    rmWrite(c, b, rm, aw, ~rmRead(c, b, rm, aw));
                    return {};
                case 3: { // neg
                    uint32_t v = rmRead(c, b, rm, aw);
                    rmWrite(c, b, rm, aw, doSub(c, 0, v, 0, aw));
                    return {};
                }
                default:
                    doMulDiv(c, in.reg, rmRead(c, b, rm, aw), aw);
                    return {};
            }
        }
        case 0xF8: c.eflags &= ~FLAG_CF; return {}; // clc
        case 0xF9: c.eflags |= FLAG_CF; return {};  // stc
        case 0xFA:
        case 0xFB: return {}; // cli/sti: user mode, no interrupt model
        case 0xFC: c.eflags &= ~FLAG_DF; return {}; // cld
        case 0xFD: c.eflags |= FLAG_DF; return {};  // std
        case 0xFE: { // inc/dec rm8
            if (in.reg > 1) throw UdFault{};
            const uint32_t savedCF = c.eflags & FLAG_CF;
            RM rm = resolveRM(c, in);
            uint32_t v = rmRead(c, b, rm, 1);
            uint32_t res = in.reg == 0 ? doAdd(c, v, 1, 0, 1) : doSub(c, v, 1, 0, 1);
            rmWrite(c, b, rm, 1, res);
            c.eflags = (c.eflags & ~FLAG_CF) | savedCF;
            return {};
        }
        case 0xFF: { // group 5
            RM rm = resolveRM(c, in);
            switch (in.reg) {
                case 0:
                case 1: { // inc/dec rmW
                    const uint32_t savedCF = c.eflags & FLAG_CF;
                    uint32_t v = rmRead(c, b, rm, w);
                    uint32_t res =
                        in.reg == 0 ? doAdd(c, v, 1, 0, w) : doSub(c, v, 1, 0, w);
                    rmWrite(c, b, rm, w, res);
                    c.eflags = (c.eflags & ~FLAG_CF) | savedCF;
                    return {};
                }
                case 2: { // call rm
                    uint32_t target = rmRead(c, b, rm, 4);
                    push(c, b, next);
                    c.eip = target;
                    return {};
                }
                case 4: c.eip = rmRead(c, b, rm, 4); return {}; // jmp rm
                case 6: push(c, b, rmRead(c, b, rm, w), w); return {}; // push rm
                default: throw UdFault{}; // far call/jmp: no segmentation here
            }
        }
    }
    throw UdFault{};
}

} // namespace

// Direct-mapped decode cache. Each slot holds a decoded instruction plus the
// raw bytes it came from; a hit is confirmed by memcmp against live memory, so
// self-modifying code re-decodes automatically without any write tracking.
class DecodeCache {
  public:
    static constexpr unsigned kBits = 16; // 65536 slots (~4 MB)
    static constexpr uint32_t kMask = (1u << kBits) - 1;

    struct Slot {
        uint32_t eip = 0xFFFFFFFFu; // tag; this value never appears as a real
                                    // hostcall-free EIP we cache
        uint8_t len = 0;
        uint8_t bytes[15] = {};
        Inst inst;
    };

    // Return the decoded instruction at eip, from cache when the bytes still
    // match, decoding (and may throw) on a miss.
    const Inst& fetch(const Bus& bus, uint32_t eip) {
        Slot& s = table_[(eip ^ (eip >> kBits)) & kMask];
        if (s.eip == eip && eip + s.len <= bus.size &&
            std::memcmp(s.bytes, bus.base + eip, s.len) == 0)
            return s.inst;
        Inst in;
        decode(bus, eip, in); // throws MemFault/UdFault on bad fetch — slot kept
        s.eip = eip;
        s.len = in.len;
        std::memcpy(s.bytes, bus.base + eip, in.len);
        s.inst = in;
        return s.inst;
    }
    void clear() {
        for (Slot& s : table_) s.eip = 0xFFFFFFFFu;
    }

  private:
    std::vector<Slot> table_{size_t(1) << kBits};
};

DecodeCache* decode_cache_new() { return new DecodeCache(); }
void decode_cache_free(DecodeCache* c) { delete c; }
void decode_cache_clear(DecodeCache* c) {
    if (c) c->clear();
}

RunResult run(Cpu& cpu, const Bus& bus, uint64_t max_steps, DecodeCache* cache) {
    RunResult r{};
    for (uint64_t n = 0; n < max_steps; n++) {
        if (cpu.eip >= HOSTCALL_BASE && cpu.eip < HOSTCALL_END) {
            r.exit = Exit::Hostcall;
            r.steps = n;
            return r;
        }
        const uint32_t start = cpu.eip;
        try {
            Inst local;
            const Inst& in = cache ? cache->fetch(bus, start)
                                   : (decode(bus, start, local), local);
            RunResult one = exec1(cpu, bus, in, start + in.len);
            cpu.icount++;
            if (one.exit != Exit::Steps) {
                one.steps = n + 1;
                return one;
            }
        } catch (const MemFault& f) {
            cpu.eip = start;
            return {Exit::Fault, f.kind, 0, f.addr, n};
        } catch (const UdFault&) {
            cpu.eip = start;
            return {Exit::Fault, FaultKind::Ud, 6, 0, n};
        } catch (const DeFault&) {
            cpu.eip = start;
            return {Exit::Fault, FaultKind::De, 0, 0, n};
        }
    }
    r.steps = max_steps;
    return r;
}

RunResult step(Cpu& cpu, const Bus& bus) { return run(cpu, bus, 1); }

} // namespace zhelezo
