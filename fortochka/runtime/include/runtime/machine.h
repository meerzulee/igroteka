// Machine — the guest process + the run loop that dispatches hostcalls back to
// HLE modules. Owns the reverse-thunk primitive: call_guest() re-enters the
// interpreter at a guest function pointer and returns its result. This is what
// DispatchMessage needs to invoke a wndproc, what TLS callbacks need, what any
// host→guest callback needs. Part of Fortochka (MIT).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "peload/peload.h"
#include "zhelezo/zhelezo.h"

namespace runtime {

class Machine;

// An HLE import handler. Args are already lifted; return the stdcall result in
// EAX by calling m.ret(nargs, value), or set Machine::exited for ExitProcess.
// Returns false if this handler does not serve (dll,name).
using Handler = std::function<bool(Machine& m, const std::string& dll,
                                   const std::string& name)>;

class Machine {
  public:
    // arena_bytes is zeroed and owned by the Machine.
    explicit Machine(uint32_t arena_bytes);

    zhelezo::Bus bus() const { return {arena_.data() != nullptr ? const_cast<uint8_t*>(arena_.data()) : nullptr, (uint32_t)arena_.size()}; }
    uint8_t* mem() { return arena_.data(); }
    uint32_t mem_size() const { return (uint32_t)arena_.size(); }
    zhelezo::Cpu& cpu() { return cpu_; }

    // Load a PE32; records imports and assigns hostcall slots. Throws
    // peload::LoadError.
    const peload::Image& load(const uint8_t* file, size_t len,
                              uint32_t force_base = 0);

    void add_handler(Handler h) { handlers_.push_back(std::move(h)); }

    // Set up the initial stack and entry return sentinel, then run any TLS
    // callbacks, then the entry point. Returns the process exit code.
    // Throws MachineError on fault / unimplemented import / runaway.
    int run_entry(uint64_t step_budget = 200'000'000);

    // Reverse thunk: call a guest function at `func_va` with `args` pushed
    // right-to-left (stdcall/cdecl compatible), on the current stack. Runs
    // until the matching return sentinel, returns EAX. Nestable (SendMessage
    // inside a wndproc). Throws MachineError on fault/runaway.
    uint32_t call_guest(uint32_t func_va, const std::vector<uint32_t>& args,
                        uint64_t step_budget = 200'000'000);

    // --- helpers for handlers ---
    uint32_t arg(unsigned i) const;           // stdcall arg i (i>=0)
    void ret(unsigned nargs, uint32_t eax);    // finish a stdcall
    uint32_t read32(uint32_t va) const;
    void write32(uint32_t va, uint32_t v);
    std::string read_cstr(uint32_t va, uint32_t max = 4096) const;

    // TIB / SEH. tib_addr() is the guest linear address of the Thread
    // Information Block; fs_base points here, so fs:[0] is the SEH chain head.
    // The SEH dispatcher (k32web) borrows guest scratch above the TIB fields
    // for the synthetic EXCEPTION_RECORD and CONTEXT it hands to guest
    // handlers — laid out here so both sides agree.
    uint32_t tib_addr() const;
    static constexpr uint32_t TIB_SCRATCH_RECORD = 0x100; // EXCEPTION_RECORD
    static constexpr uint32_t TIB_SCRATCH_CONTEXT = 0x200; // CONTEXT stub
    static constexpr uint32_t SEH_CHAIN_END = 0xFFFFFFFFu;

    // --- guest heap + COM support ---
    // Guest heap region base (runtime-owned, below the image); a future
    // k32web VirtualAlloc must avoid [HEAP_BASE, heap watermark).
    static constexpr uint32_t HEAP_BASE = 0x02000000;

    // Bump-allocate `size` bytes (8-aligned) of guest memory; returns the guest
    // VA. HLE-owned allocations (COM vtables/objects) are never freed — see the
    // no-free debt note by the members below; a real free path lands with the
    // resource-HLE work (F3).
    uint32_t alloc(uint32_t size);

    // COM: one vtable per interface CLASS, shared by all its instances (real COM
    // semantics; the handler gets object identity via arg(0), so per-object
    // vtables would only waste hostcall slots). create_com_vtable allocates the
    // vtable + `num_methods` hostcall thunks once and returns its guest VA;
    // calling thunk i dispatches `handler(*this, i)` with the object as arg(0)
    // (COM's `this`). create_com_instance makes an object: [vtable_ptr, state...]
    // with `state_bytes` zeroed.
    using ComHandler = std::function<void(Machine&, unsigned method)>;
    uint32_t create_com_vtable(unsigned num_methods, ComHandler handler);
    uint32_t create_com_instance(uint32_t vtable, uint32_t state_bytes);

    bool exited = false;
    uint32_t exit_code = 0;

  private:
    // Push a return-sentinel frame and drive run() until EIP hits it, servicing
    // hostcalls to real imports along the way. Used by both run_entry and
    // call_guest so nested guest calls share one dispatch loop.
    uint32_t drive(uint32_t sentinel_slot, uint64_t step_budget);
    uint32_t hostcall_addr(uint32_t slot) const;
    void dispatch_import(uint32_t slot);

    // Reverse-thunk nesting cap. Legit SendMessage chains are a few dozen deep;
    // this bounds host C++ recursion so a runaway guest errors instead of
    // overflowing the native stack.
    static constexpr unsigned kMaxReentry = 512;

    std::vector<uint8_t> arena_;
    zhelezo::Cpu cpu_;
    // Per-guest decode cache: memoizes decoded instructions across the whole
    // process lifetime so hot blocks skip re-decoding. Custom deleter keeps the
    // type opaque here.
    std::unique_ptr<zhelezo::DecodeCache, void (*)(zhelezo::DecodeCache*)> cache_{
        zhelezo::decode_cache_new(), zhelezo::decode_cache_free};
    std::vector<Handler> handlers_;
    std::vector<const peload::Import*> slots_; // index by hostcall slot
    uint32_t next_slot_ = 0;
    uint32_t reentry_slot_ = 0; // reserved sentinel for reverse-thunk returns
    unsigned reentry_depth_ = 0; // current reverse-thunk nesting depth
    peload::Image image_;
    bool loaded_ = false;

    // Guest heap bump pointer (never decremented — HLE allocations are immortal
    // until the resource-HLE free path lands in F3) and the COM vtable-thunk
    // registry: hostcall slot -> (handler, method index). A slot is either an
    // import or a COM thunk, never both (the ranges are disjoint: next_slot_ is
    // monotonic and slots_ is sized at load).
    uint32_t heap_next_ = HEAP_BASE;
    struct ComSlot {
        ComHandler handler;
        unsigned method;
    };
    std::vector<ComSlot> com_slots_; // indexed by hostcall slot
};

struct MachineError {
    std::string what;
};

} // namespace runtime
