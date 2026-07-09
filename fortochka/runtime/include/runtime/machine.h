// Machine — the guest process + the run loop that dispatches hostcalls back to
// HLE modules. Owns the reverse-thunk primitive: call_guest() re-enters the
// interpreter at a guest function pointer and returns its result. This is what
// DispatchMessage needs to invoke a wndproc, what TLS callbacks need, what any
// host→guest callback needs. Part of Fortochka (MIT).
#pragma once

#include <cstdint>
#include <functional>
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
    std::vector<Handler> handlers_;
    std::vector<const peload::Import*> slots_; // index by hostcall slot
    uint32_t next_slot_ = 0;
    uint32_t reentry_slot_ = 0; // reserved sentinel for reverse-thunk returns
    unsigned reentry_depth_ = 0; // current reverse-thunk nesting depth
    peload::Image image_;
    bool loaded_ = false;
};

struct MachineError {
    std::string what;
};

} // namespace runtime
