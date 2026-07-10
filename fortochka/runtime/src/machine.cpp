#include "runtime/machine.h"

#include <cctype>
#include <cstdio>
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
// Preemptive time-slice: a thread runs at most this many instructions between
// scheduler switch points, so a busy-wait (a spin on a memory flag another
// thread sets, with no wait API call) still yields.
constexpr uint64_t kThreadSliceSteps = 2'000'000;
constexpr uint32_t kThreadStackBytes = 256 * 1024; // per worker-thread stack
constexpr uint32_t kThreadTebBytes = 0x1000;
// Win32 wait return codes.
constexpr uint32_t WAIT_OBJECT_0 = 0x00000000;
constexpr uint32_t WAIT_TIMEOUT = 0x00000102;
} // namespace

uint32_t Machine::tib_addr() const { return TIB_ADDR; }

uint32_t Machine::alloc(uint32_t size) {
    // Align in 64-bit so a near-UINT32_MAX size can't wrap to a tiny value and
    // silently under-allocate (the bounds check below would then pass).
    uint64_t asz = ((uint64_t)size + 7) & ~UINT64_C(7);
    uint32_t va = heap_next_;
    if ((uint64_t)va + asz > arena_.size())
        throw MachineError{"guest heap exhausted"};
    heap_next_ += (uint32_t)asz;
    return va;
}

uint32_t Machine::create_com_vtable(unsigned num_methods, ComHandler handler) {
    uint32_t vtbl = alloc(num_methods * 4);
    for (unsigned i = 0; i < num_methods; i++) {
        uint32_t slot = next_slot_++;
        if (slot >= com_slots_.size()) com_slots_.resize(slot + 1);
        com_slots_[slot] = {handler, i};
        write32(vtbl + 4 * i, hostcall_addr(slot)); // vtable[i] = thunk
    }
    return vtbl;
}

uint32_t Machine::create_com_instance(uint32_t vtable, uint32_t state_bytes) {
    uint32_t obj = alloc(4 + state_bytes); // [vtable_ptr, state...]
    write32(obj, vtable);
    for (uint32_t i = 0; i < state_bytes; i += 4) write32(obj + 4 + i, 0);
    return obj;
}

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

// Normalize a DLL name for comparison: lowercase, drop a trailing ".dll".
static std::string norm_dll(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".dll") == 0)
        s.resize(s.size() - 4);
    return s;
}

uint32_t Machine::resolve_proc(const std::string& name) const {
    // Name-only match (for the image's own handle / an unknown handle): import
    // names are effectively unique across the DLLs we HLE.
    for (const auto& imp : image_.imports)
        if (imp.name == name) return hostcall_addr(imp.slot);
    return 0;
}
uint32_t Machine::resolve_proc(const std::string& dll,
                               const std::string& name) const {
    std::string want = norm_dll(dll);
    for (const auto& imp : image_.imports)
        if (imp.name == name && norm_dll(imp.dll) == want)
            return hostcall_addr(imp.slot);
    return 0;
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
    // COM vtable thunks take priority: a slot bound to a COM method dispatches
    // by index with the object as arg(0).
    if (slot < com_slots_.size() && com_slots_[slot].handler) {
        com_slots_[slot].handler(*this, com_slots_[slot].method);
        return;
    }
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
        RunResult r = run(cpu_, b, step_budget, cache_.get());
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
    heap_next_ = HEAP_BASE;
    cpu_.fs_base = TIB_ADDR;
    write32(TIB_ADDR + 0x00, SEH_END);    // ExceptionList: empty chain
    write32(TIB_ADDR + 0x04, STACK_TOP);  // StackBase (high)
    write32(TIB_ADDR + 0x08, 0x00010000); // StackLimit (low)
    write32(TIB_ADDR + 0x18, TIB_ADDR);   // Self (TEB linear address)

    // Thread 0 (the main thread) exists before any guest code runs so TLS
    // callbacks / CRT startup that touch per-thread TLS have a home.
    threads_.clear();
    waitables_.clear();
    threads_.emplace_back();
    threads_[0].teb = TIB_ADDR;
    cur_tid_ = 0;
    proc_icount_ = 0;
    sched_active_ = false;
    yield_blocked_ = false;

    for (uint32_t cb : image_.tls_callbacks)
        call_guest(cb, {image_.base, 1 /*DLL_PROCESS_ATTACH*/, 0}, step_budget);

    return scheduler_run(kThreadSliceSteps);
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

// ---- cooperative thread scheduler ----

uint32_t Machine::tls_get(unsigned i) const {
    return i < 64 ? threads_[cur_tid_].tls[i] : 0;
}
void Machine::tls_set(unsigned i, uint32_t v) {
    if (i < 64) threads_[cur_tid_].tls[i] = v;
}

Machine::Waitable* Machine::waitable(uint32_t handle) {
    if (handle >= WAIT_HANDLE_BASE &&
        handle - WAIT_HANDLE_BASE < waitables_.size())
        return &waitables_[handle - WAIT_HANDLE_BASE];
    return nullptr;
}
int Machine::thread_index(uint32_t handle) const {
    if (handle >= THREAD_HANDLE_BASE && handle < WAIT_HANDLE_BASE &&
        handle - THREAD_HANDLE_BASE < threads_.size())
        return (int)(handle - THREAD_HANDLE_BASE);
    return -1;
}

uint32_t Machine::create_thread(uint32_t entry, uint32_t param, bool suspended) {
    uint32_t stack = alloc(kThreadStackBytes);
    uint32_t teb = alloc(kThreadTebBytes);
    uint32_t sp = stack + kThreadStackBytes - 16; // top, a little slack
    sp -= 4; write32(sp, param);                  // the thread's single arg
    sp -= 4; write32(sp, hostcall_addr(ENTRY_SLOT)); // return → thread-done sentinel

    SchedThread t;
    t.cpu = Cpu{};
    t.cpu.eip = entry;
    t.cpu.gpr[ESP] = sp;
    t.cpu.fs_base = teb;
    t.stack_base = stack;
    t.teb = teb;
    t.suspend = suspended ? 1 : 0;
    t.st = SchedThread::Ready;
    // Minimal TEB: empty SEH chain + stack bounds + self pointer.
    write32(teb + 0x00, SEH_END);
    write32(teb + 0x04, stack + kThreadStackBytes); // StackBase (high)
    write32(teb + 0x08, stack);                     // StackLimit (low)
    write32(teb + 0x18, teb);                       // Self
    threads_.push_back(std::move(t));
    return THREAD_HANDLE_BASE + (uint32_t)(threads_.size() - 1);
}

uint32_t Machine::resume_thread(uint32_t handle) {
    int i = thread_index(handle);
    if (i < 0) return (uint32_t)-1;
    uint32_t prev = threads_[i].suspend;
    if (threads_[i].suspend > 0) threads_[i].suspend--;
    return prev;
}
uint32_t Machine::suspend_thread(uint32_t handle) {
    int i = thread_index(handle);
    if (i < 0) return (uint32_t)-1;
    return threads_[i].suspend++;
}
bool Machine::thread_exit_code(uint32_t handle, uint32_t& code) const {
    int i = thread_index(handle);
    if (i < 0) return false;
    code = threads_[i].st == SchedThread::Done ? threads_[i].exit_code
                                               : 0x103; // STILL_ACTIVE
    return true;
}

uint32_t Machine::create_event(bool manual_reset, bool signaled) {
    Waitable w;
    w.manual = manual_reset;
    w.signaled = signaled;
    waitables_.push_back(w);
    return WAIT_HANDLE_BASE + (uint32_t)(waitables_.size() - 1);
}
uint32_t Machine::create_mutex(bool initial_owner) {
    Waitable w;
    w.is_mutex = true;
    w.owner = initial_owner ? cur_tid_ + 1 : 0;
    w.recursion = initial_owner ? 1 : 0;
    waitables_.push_back(w);
    return WAIT_HANDLE_BASE + (uint32_t)(waitables_.size() - 1);
}
void Machine::set_event(uint32_t handle) {
    if (Waitable* w = waitable(handle)) w->signaled = true;
}
void Machine::reset_event(uint32_t handle) {
    if (Waitable* w = waitable(handle)) w->signaled = false;
}
bool Machine::release_mutex(uint32_t handle) {
    Waitable* w = waitable(handle);
    if (!w || !w->is_mutex || w->owner != cur_tid_ + 1) return false;
    if (--w->recursion == 0) w->owner = 0;
    return true;
}

void Machine::begin_wait(const std::vector<uint32_t>& handles, bool wait_all,
                         uint32_t timeout_ms, unsigned nargs) {
    if (reentry_depth_ != 0)
        throw MachineError{"blocking wait inside a reverse thunk is unsupported"};
    SchedThread& t = threads_[cur_tid_];
    t.wait_handles = handles;
    t.wait_all = wait_all;
    t.wait_nargs = nargs;
    // Timeout in the shared retired-instruction clock (GetTickCount = icount/1000
    // ⇒ 1 ms ≈ 1000 ticks). INFINITE never elapses.
    t.wait_deadline = timeout_ms == 0xFFFFFFFFu
                          ? UINT64_MAX
                          : proc_icount_ + (uint64_t)timeout_ms * 1000;
    t.st = SchedThread::Blocked;
    yield_blocked_ = true; // drive_slice returns to the scheduler
}

// Is thread `tid`'s parked wait now satisfiable? On success sets `which` to the
// value to return from the wait (WAIT_OBJECT_0[+i] / WAIT_TIMEOUT) and CONSUMES
// the objects (auto-reset events clear, mutexes are acquired).
bool Machine::wait_satisfied(SchedThread& t, uint32_t& which) {
    size_t tid = (size_t)(&t - threads_.data());
    auto avail = [&](uint32_t h) -> bool {
        if (int ti = thread_index(h); ti >= 0)
            return threads_[ti].st == SchedThread::Done;
        Waitable* w = waitable(h);
        if (!w) return false; // unknown handle: never signals
        if (w->is_mutex) return w->owner == 0 || w->owner == tid + 1;
        return w->signaled;
    };
    auto consume = [&](uint32_t h) {
        Waitable* w = waitable(h);
        if (!w) return;
        if (w->is_mutex) { w->owner = (uint32_t)tid + 1; w->recursion++; }
        else if (!w->manual) w->signaled = false; // auto-reset
    };
    if (t.wait_all) {
        for (uint32_t h : t.wait_handles)
            if (!avail(h)) { if (proc_icount_ >= t.wait_deadline) { which = WAIT_TIMEOUT; return true; } return false; }
        for (uint32_t h : t.wait_handles) consume(h);
        which = WAIT_OBJECT_0;
        return true;
    }
    for (size_t i = 0; i < t.wait_handles.size(); i++)
        if (avail(t.wait_handles[i])) {
            consume(t.wait_handles[i]);
            which = WAIT_OBJECT_0 + (uint32_t)i;
            return true;
        }
    if (proc_icount_ >= t.wait_deadline) { which = WAIT_TIMEOUT; return true; }
    return false;
}

// One thread's time-slice: run until it returns to its sentinel, parks on a
// wait, exhausts the slice, or the process exits. cpu_ is already the thread's.
Machine::Slice Machine::drive_slice(uint32_t sentinel_slot, uint64_t slice_steps) {
    Bus b = bus();
    uint64_t start = cpu_.icount;
    for (;;) {
        uint64_t used = cpu_.icount - start;
        if (used >= slice_steps) return Slice::Sliced;
        RunResult r = run(cpu_, b, slice_steps - used, cache_.get());
        switch (r.exit) {
            case Exit::Hostcall: {
                uint32_t slot = (cpu_.eip - HOSTCALL_BASE) / 16;
                if (slot == sentinel_slot) return Slice::Returned;
                dispatch_import(slot);
                if (exited) return Slice::Exited;
                if (yield_blocked_) { yield_blocked_ = false; return Slice::Blocked; }
                break;
            }
            case Exit::Fault: {
                char buf[128];
                std::snprintf(buf, sizeof buf,
                              "cpu fault kind=%d eip=%x bytes=%02x %02x %02x %02x %02x "
                              "ebx=%x eax=%x",
                              (int)r.fault, cpu_.eip, read32(cpu_.eip) & 0xFF,
                              (read32(cpu_.eip) >> 8) & 0xFF, (read32(cpu_.eip) >> 16) & 0xFF,
                              (read32(cpu_.eip) >> 24) & 0xFF, read32(cpu_.eip + 4) & 0xFF,
                              cpu_.gpr[3], cpu_.gpr[0]);
                throw MachineError{buf};
            }
            case Exit::Steps:
                return Slice::Sliced;
            default:
                throw MachineError{"unexpected halt exit=" +
                                   std::to_string((int)r.exit)};
        }
    }
}

int Machine::scheduler_run(uint64_t slice) {
    if (sched_active_) throw MachineError{"scheduler re-entered"};
    sched_active_ = true;
    struct Guard {
        bool& a;
        ~Guard() { a = false; }
    } guard{sched_active_};

    // cpu_ holds thread 0's freshly-set-up state (entry point). Park it into its
    // slot so the loop below can treat every thread uniformly as load-then-save.
    threads_[cur_tid_].cpu = cpu_;

    for (;;) {
        if (exited) return (int)exit_code;

        // 1. Complete any parked waits whose objects are now available. The ret
        //    frame is unwound on the blocked thread's own CPU state.
        for (size_t i = 0; i < threads_.size(); i++) {
            SchedThread& t = threads_[i];
            if (t.st != SchedThread::Blocked) continue;
            uint32_t which;
            if (!wait_satisfied(t, which)) continue;
            Cpu save = cpu_;
            uint32_t save_tid = cur_tid_;
            cpu_ = t.cpu;
            cur_tid_ = (uint32_t)i;
            ret(t.wait_nargs, which); // finishes the WaitFor* stdcall
            t.cpu = cpu_;
            cpu_ = save;
            cur_tid_ = save_tid;
            t.st = SchedThread::Ready;
            t.wait_handles.clear();
        }

        // 2. Pick the next runnable thread, round-robin from the current one.
        int pick = -1;
        size_t n = threads_.size();
        for (size_t k = 1; k <= n; k++) {
            size_t i = (cur_tid_ + k) % n;
            if (threads_[i].st == SchedThread::Ready && threads_[i].suspend == 0) {
                pick = (int)i;
                break;
            }
        }
        if (pick < 0) {
            bool any_live = false, any_blocked = false;
            uint64_t next_deadline = UINT64_MAX;
            for (auto& t : threads_) {
                if (t.st != SchedThread::Done) any_live = true;
                if (t.st == SchedThread::Blocked) {
                    any_blocked = true;
                    if (t.wait_deadline < next_deadline) next_deadline = t.wait_deadline;
                }
            }
            if (!any_live) return (int)exit_code; // everything finished
            // Nobody can run, but a blocked thread has a finite timeout: jump the
            // virtual clock to it so its wait times out (rather than spinning or
            // false-flagging a deadlock — the clock only advances when a thread
            // runs, and here none can).
            if (next_deadline != UINT64_MAX) {
                proc_icount_ = next_deadline;
                continue; // step 1 next iteration completes it as WAIT_TIMEOUT
            }
            if (any_blocked)
                throw MachineError{"all threads blocked on INFINITE waits (deadlock)"};
            throw MachineError{"no runnable thread (all suspended)"};
        }

        // 3. Load the picked thread, run one slice, save it back. The previous
        //    thread was already saved at the end of its own slice, so there is
        //    no separate "save outgoing" step (doing one here would clobber a
        //    just-completed wait when pick == cur_tid_).
        cur_tid_ = (uint32_t)pick;
        cpu_ = threads_[pick].cpu;
        cpu_.icount = proc_icount_;
        Slice res = drive_slice(ENTRY_SLOT, slice);
        proc_icount_ = cpu_.icount;
        threads_[pick].cpu = cpu_;

        switch (res) {
            case Slice::Returned:
                threads_[pick].st = SchedThread::Done;
                threads_[pick].exit_code = cpu_.gpr[EAX];
                // Abandon any mutexes this thread still owned so waiters aren't
                // stranded forever (Codex finding; we release rather than raise
                // WAIT_ABANDONED — enough to keep the scheduler from deadlocking).
                for (Waitable& w : waitables_)
                    if (w.is_mutex && w.owner == (uint32_t)pick + 1) {
                        w.owner = 0;
                        w.recursion = 0;
                    }
                if (pick == 0) { exited = true; exit_code = cpu_.gpr[EAX]; }
                break;
            case Slice::Exited:
                return (int)exit_code;
            case Slice::Blocked: // begin_wait already set st = Blocked
            case Slice::Sliced:  // preempted, still Ready
                break;
        }
    }
}

} // namespace runtime
