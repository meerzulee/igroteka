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
    // Resume a process that returned early because run_entry/run_more hit its
    // step budget (scheduler + thread state persist in the Machine). Runs up to
    // `delta_steps` more retired instructions, then returns so a host loop (the
    // browser slice loop) can blit a frame / pump input and call again.
    int run_more(uint64_t delta_steps);

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

    // --- cooperative thread scheduler (kernel32 threads/sync map onto this) ---
    // A shared retired-instruction clock, used by the timing HLE so time is one
    // monotonic count across all threads (not a per-CPU snapshot).
    uint64_t proc_ticks() const { return proc_icount_; }
    uint32_t current_tid() const { return cur_tid_; } // 0 = main thread
    // Per-thread TLS slot storage (the alloc bitmap stays global in k32web; the
    // VALUES are per-thread here). i must be < 64.
    uint32_t tls_get(unsigned i) const;
    void tls_set(unsigned i, uint32_t v);

    // Spawn a guest thread starting at `entry(param)` on a fresh stack + TEB.
    // Returns a thread HANDLE (waitable). CREATE_SUSPENDED starts it suspended.
    uint32_t create_thread(uint32_t entry, uint32_t param, bool suspended);
    uint32_t resume_thread(uint32_t handle);  // → previous suspend count
    uint32_t suspend_thread(uint32_t handle); // → previous suspend count
    bool thread_exit_code(uint32_t handle, uint32_t& code) const; // false if unknown

    // Waitable kernel objects. Events: manual/auto reset. Mutexes: recursive
    // ownership by the calling thread. Handles are waitable via begin_wait.
    uint32_t create_event(bool manual_reset, bool signaled);
    uint32_t create_mutex(bool initial_owner);
    void set_event(uint32_t handle);
    void reset_event(uint32_t handle);
    bool release_mutex(uint32_t handle);

    // Block the CURRENT thread on `handles` (WaitForSingle/MultipleObjects). The
    // handler must NOT call ret() after this — the scheduler completes the wait
    // (writing `nargs`-frame ret with WAIT_OBJECT_0+i / WAIT_TIMEOUT / etc.) when
    // an object signals or the timeout (ms, 0xFFFFFFFF = INFINITE) elapses.
    // Illegal (throws) inside a reverse thunk (reentry_depth_ != 0).
    void begin_wait(const std::vector<uint32_t>& handles, bool wait_all,
                    uint32_t timeout_ms, unsigned nargs);

    // Resolve a runtime GetProcAddress request to a callable guest address, or 0
    // if unresolved. Matches by name against the static import table: those
    // slots are already IAT-patched to a hostcall thunk and known to be served
    // by an HLE handler, so returning the thunk address is safe and calling it
    // dispatches exactly as a normal import would. A name the program did not
    // statically import returns 0 (GetProcAddress "not found") — honest
    // graceful degradation, never a pointer we cannot service. Dynamic-only
    // exports become resolvable by promoting them to a forced static import.
    uint32_t resolve_proc(const std::string& name) const;
    // Same, but scoped to a specific module: only imports from `dll` match (DLL
    // names compared case-insensitively, ignoring a trailing ".dll"). Used when
    // GetProcAddress has a real module handle, so a name that collides across
    // DLLs with a different ABI can't resolve to the wrong module's thunk.
    uint32_t resolve_proc(const std::string& dll, const std::string& name) const;

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
    // Guest heap region base (runtime-owned). MUST sit ABOVE the loaded image's
    // virtual end: a real shipped exe like RTW has a huge .bss (RTW's image runs
    // to VA ~0x2b56000 — 28MB of .data alone), so a heap starting at the old
    // 0x02000000 grew straight into the program's own globals and silently
    // corrupted them once the bump watermark climbed past ~0x2100000 (traced:
    // heap writes clobbered a live global std::vector control block, crashing
    // deep in unrelated cleanup). 0x03000000 clears RTW's image with margin and
    // still leaves 464MB of heap inside the 512MB arena. A future k32web
    // VirtualAlloc must avoid [HEAP_BASE, heap watermark).
    static constexpr uint32_t HEAP_BASE = 0x03000000;

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

    // --- scheduler internals ---
    // A guest thread: its saved CPU, stack/TEB regions, run state, and (when
    // Blocked) the parked wait it will resume from. Thread 0 is the main thread.
    struct SchedThread {
        zhelezo::Cpu cpu;
        uint32_t stack_base = 0, teb = 0;
        enum St { Ready, Blocked, Done } st = Ready;
        uint32_t exit_code = 0;
        uint32_t suspend = 0;   // CREATE_SUSPENDED / SuspendThread count
        uint32_t tls[64] = {};  // per-thread TLS values
        // Parked wait (valid while st == Blocked): the objects, mode, deadline
        // (in proc_icount ticks; ~UINT64_MAX = INFINITE), and the stdcall frame
        // width to unwind when the scheduler completes the wait.
        std::vector<uint32_t> wait_handles;
        bool wait_all = false;
        uint64_t wait_deadline = 0;
        unsigned wait_nargs = 0;
    };
    // A waitable kernel object (event or mutex).
    struct Waitable {
        bool is_mutex = false;
        bool manual = false;   // event: manual-reset vs auto-reset
        bool signaled = false; // event signaled state
        uint32_t owner = 0;    // mutex owner tid + 1 (0 = free)
        uint32_t recursion = 0;
    };
    // The cooperative scheduler: round-robin Ready threads, each a bounded slice,
    // completing parked waits between slices, until the process exits. Replaces
    // the top-level drive() in run_entry. NON-RECURSIVE (asserts !sched_active_).
    enum class Slice { Returned, Blocked, Sliced, Exited };
    int scheduler_run(uint64_t slice_steps, uint64_t budget = UINT64_MAX);
    Slice drive_slice(uint32_t sentinel_slot, uint64_t slice_steps);
    bool wait_satisfied(SchedThread& t, uint32_t& which); // which = result on success
    Waitable* waitable(uint32_t handle);
    int thread_index(uint32_t handle) const; // thread handle → threads_ index, or -1

    static constexpr uint32_t THREAD_HANDLE_BASE = 0x30000000u;
    static constexpr uint32_t WAIT_HANDLE_BASE = 0x38000000u;
    std::vector<SchedThread> threads_;
    std::vector<Waitable> waitables_;
    uint32_t cur_tid_ = 0;      // index into threads_ of the running thread
    uint64_t proc_icount_ = 0;  // shared retired-instruction clock
    bool sched_active_ = false; // guards against a recursive pump
    bool yield_blocked_ = false; // a wait handler parked the current thread

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
