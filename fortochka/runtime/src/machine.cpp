#include "runtime/machine.h"

#include <cstring>

namespace runtime {

using namespace zhelezo;

namespace {
constexpr uint32_t ENTRY_SLOT = 0;   // main() returns here → process exit
constexpr uint32_t REENTRY_SLOT = 1; // reverse-thunk guest calls return here
constexpr uint32_t FIRST_IMPORT_SLOT = 2;
constexpr uint32_t STACK_TOP = 0x00380000; // below the 0x400000 image base
// TIB sits in the gap between the stack region and the image base. fs:[0] (the
// SEH chain head) is TIB offset 0; the SEH dispatcher borrows scratch above it
// (see machine.h TIB_SCRATCH_*). Empty chain sentinel is 0xFFFFFFFF (MSVC).
constexpr uint32_t TIB_ADDR = 0x00390000;
constexpr uint32_t SEH_END = 0xFFFFFFFFu;
} // namespace

uint32_t Machine::tib_addr() const { return TIB_ADDR; }

Machine::Machine(uint32_t arena_bytes) : arena_(arena_bytes, 0) {
    next_slot_ = FIRST_IMPORT_SLOT;
    reentry_slot_ = REENTRY_SLOT;
}

uint32_t Machine::hostcall_addr(uint32_t slot) const {
    return HOSTCALL_BASE + slot * 16;
}

const peload::Image& Machine::load(const uint8_t* file, size_t len,
                                   uint32_t force_base) {
    image_ = peload::load(arena_.data(), (uint32_t)arena_.size(), file, len,
                          HOSTCALL_BASE, next_slot_, force_base);
    slots_.assign(next_slot_, nullptr);
    for (const auto& imp : image_.imports) slots_[imp.slot] = &imp;
    loaded_ = true;
    return image_;
}

// ---- memory + stdcall helpers ----

uint32_t Machine::read32(uint32_t va) const {
    if (va >= arena_.size() || arena_.size() - va < 4) return 0;
    uint32_t v;
    std::memcpy(&v, arena_.data() + va, 4);
    return v;
}
void Machine::write32(uint32_t va, uint32_t v) {
    if (va >= arena_.size() || arena_.size() - va < 4) return;
    std::memcpy(arena_.data() + va, &v, 4);
}
std::string Machine::read_cstr(uint32_t va, uint32_t max) const {
    std::string s;
    while (va < arena_.size() && s.size() < max) {
        char c = (char)arena_[va++];
        if (!c) break;
        s.push_back(c);
    }
    return s;
}
uint32_t Machine::arg(unsigned i) const {
    return read32(cpu_.gpr[ESP] + 4 + 4 * i); // [esp]=retaddr, [esp+4]=arg0
}
void Machine::ret(unsigned nargs, uint32_t eax) {
    cpu_.gpr[EAX] = eax;
    cpu_.eip = read32(cpu_.gpr[ESP]);
    cpu_.gpr[ESP] += 4 + 4 * nargs;
}

void Machine::dispatch_import(uint32_t slot) {
    const peload::Import* imp = slot < slots_.size() ? slots_[slot] : nullptr;
    if (!imp) throw MachineError{"hostcall to unbound slot"};
    for (auto& h : handlers_)
        if (h(*this, imp->dll, imp->name)) return;
    throw MachineError{"unimplemented import " + imp->dll + "!" + imp->name};
}

// Core loop: run guest code, service imports, stop when EIP lands on the
// sentinel this frame is waiting for. Shared by run_entry and call_guest, so
// nested reverse thunks reuse it recursively (each call_guest = one C++ frame).
uint32_t Machine::drive(uint32_t sentinel_slot, uint64_t step_budget) {
    Bus b = bus();
    for (;;) {
        RunResult r = run(cpu_, b, step_budget);
        switch (r.exit) {
            case Exit::Hostcall: {
                uint32_t slot = (cpu_.eip - HOSTCALL_BASE) / 16;
                if (slot == sentinel_slot) return cpu_.gpr[EAX];
                dispatch_import(slot);
                if (exited) return exit_code;
                break;
            }
            case Exit::Fault:
                throw MachineError{"cpu fault kind=" + std::to_string((int)r.fault) +
                                   " eip=" + std::to_string(cpu_.eip) +
                                   " addr=" + std::to_string(r.fault_addr)};
            case Exit::Steps:
                throw MachineError{"runaway (step budget exhausted)"};
            default:
                throw MachineError{"unexpected halt exit=" +
                                   std::to_string((int)r.exit)};
        }
    }
}

int Machine::run_entry(uint64_t step_budget) {
    if (!loaded_) throw MachineError{"run_entry before load"};
    // The TIB region [TIB_ADDR, +0x1000) is fixed; a PE whose mapped image
    // covers it would silently corrupt fs:[0] and the SEH scratch. Reject it
    // (a real relocation policy comes with the memory-map work).
    if (image_.base < TIB_ADDR + 0x1000 && image_.base + image_.size > TIB_ADDR)
        throw MachineError{"image overlaps the TIB region at 0x390000"};
    cpu_ = Cpu{};
    cpu_.eip = image_.entry;
    cpu_.gpr[ESP] = STACK_TOP - 4;
    write32(cpu_.gpr[ESP], hostcall_addr(ENTRY_SLOT)); // main rets → process end

    // Thread Information Block: fs:[0] = SEH chain head (empty), plus the few
    // fields era CRTs read. reentry_depth_ resets with the fresh process.
    reentry_depth_ = 0;
    cpu_.fs_base = TIB_ADDR;
    write32(TIB_ADDR + 0x00, SEH_END);    // ExceptionList: empty chain
    write32(TIB_ADDR + 0x04, STACK_TOP);  // StackBase (high)
    write32(TIB_ADDR + 0x08, 0x00010000); // StackLimit (low)
    write32(TIB_ADDR + 0x18, TIB_ADDR);   // Self (TEB linear address)

    for (uint32_t cb : image_.tls_callbacks)
        call_guest(cb, {image_.base, 1 /*DLL_PROCESS_ATTACH*/, 0}, step_budget);

    return (int)drive(ENTRY_SLOT, step_budget);
}

uint32_t Machine::call_guest(uint32_t func_va, const std::vector<uint32_t>& args,
                             uint64_t step_budget) {
    // Each reverse thunk is one host C++ frame; a guest that re-enters without
    // bound (WndProc that SendMessages itself) would blow the native stack.
    // Cap the nesting — real apps stay well under this; runaway guests error.
    struct DepthGuard {
        unsigned& d;
        ~DepthGuard() { --d; }
    } guard{++reentry_depth_};
    if (reentry_depth_ > kMaxReentry)
        throw MachineError{"reverse-thunk recursion too deep (guest runaway?)"};

    const uint32_t saved_esp = cpu_.gpr[ESP];
    const uint32_t saved_eip = cpu_.eip;

    uint32_t sp = cpu_.gpr[ESP];
    for (size_t i = args.size(); i-- > 0;) { // push right-to-left
        sp -= 4;
        write32(sp, args[i]);
    }
    sp -= 4;
    write32(sp, hostcall_addr(reentry_slot_)); // guest ret → REENTRY sentinel
    cpu_.gpr[ESP] = sp;
    cpu_.eip = func_va;

    uint32_t result = drive(reentry_slot_, step_budget);

    // Restore caller frame; the guest cleaned its own args if stdcall, but we
    // restore unconditionally so cdecl callees don't leak stack either.
    cpu_.gpr[ESP] = saved_esp;
    cpu_.eip = saved_eip;
    return result;
}

} // namespace runtime
