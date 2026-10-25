// SPDX-License-Identifier: GPL-2.0-or-later
//
// Threads (ThreadManForUser): creation, start, exit, deletion, sleeping and delays, priorities,
// suspension and the status queries. A thread stays known until it is deleted; dormant threads
// can be started again. Semantics, error codes and the timing constants follow PPSSPP
// (Core/HLE/sceKernelThread.cpp, GPL-2.0-or-later) and pspautotests' threads/ tests.

#include "kernel.hpp"
#include "kernel_errors.hpp"

#include <algorithm>
#include <cstring>

namespace pspweb {
namespace {

constexpr std::uint32_t kAttrKernel = 0x00001000u;
constexpr std::uint32_t kAttrNoFillStack = 0x00100000u;
constexpr std::uint32_t kAttrClearStack = 0x00200000u;
constexpr std::uint32_t kAttrLowStack = 0x00400000u;
constexpr std::uint32_t kAttrUser = 0x80000000u;
constexpr std::uint32_t kAttrUserMask = 0xF8F060FFu;
constexpr std::uint32_t kAttrUserErase = 0x78800000u;
constexpr std::uint32_t kAttrVfpu = 0x00004000u;

constexpr std::int32_t kLowestPriority = 0x77;
constexpr std::int32_t kHighestPriority = 0x08;

// SceKernelThreadInfo is 104 bytes, 108 from SDK 2.6 on.
constexpr std::uint32_t kThreadInfoSize = 104u;
constexpr std::uint32_t kThreadInfoSizeAfter260 = 108u;

// Threadman object types (sceKernelGetThreadmanIdType / IdList).
enum : std::uint32_t {
    kTmidThread = 1, kTmidSemaphore, kTmidEventFlag, kTmidMbox, kTmidVpl, kTmidFpl, kTmidMpipe, kTmidCallback,
    kTmidThreadEventHandler, kTmidAlarm, kTmidVTimer, kTmidMutex, kTmidLwMutex, kTmidTlspl,
    kTmidSleepThread = 64, kTmidDelayThread, kTmidSuspendThread, kTmidDormantThread
};

std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

} // namespace

// The PSP's wait type numbers, for the status queries.
std::uint32_t Kernel::psp_wait_type(const Thread &t) {
    switch (t.wait) {
    case WaitType::Sleep: return 1u;
    case WaitType::Delay: return 2u;
    case WaitType::Sema: return 3u;
    case WaitType::EventFlag: return 4u;
    case WaitType::Mbx: return 5u;
    case WaitType::Vpl: return 6u;
    case WaitType::Fpl: return 7u;
    case WaitType::MsgPipeSend:
    case WaitType::MsgPipeReceive: return 8u;
    case WaitType::ThreadEnd: return 9u;
    case WaitType::Vblank: return 12u;
    case WaitType::Mutex: return 13u;
    case WaitType::LwMutex: return 14u;
    case WaitType::CtrlRead: return 15u;
    case WaitType::Io: return 16u;
    case WaitType::Ge: return 17u;
    case WaitType::Tlspl: return 21u;
    case WaitType::Callback: return 20u;
    case WaitType::None: break;
    }
    return 0u;
}

std::uint32_t Kernel::psp_thread_status(const Thread &t) {
    std::uint32_t status = 0x10u;
    switch (t.state) {
    case ThreadState::Running: status = 0x01u; break;
    case ThreadState::Ready: status = 0x02u; break;
    case ThreadState::Waiting: status = 0x04u; break;
    case ThreadState::Dormant: status = 0x10u; break;
    case ThreadState::Dead: status = 0x20u; break;
    }
    // A suspended ready thread reports only SUSPEND; a suspended waiting one WAIT|SUSPEND.
    if (t.suspended) status = (status & ~0x03u) | 0x08u;
    return status;
}

Thread *Kernel::find_thread(std::int32_t uid) {
    const auto it = threads_.find(uid);
    return it == threads_.end() ? nullptr : &it->second;
}

bool Kernel::timeout_at_once(std::uint32_t timeout_address, int base_percent, int step_percent) {
    auto &m = rt_.memory();
    if (timeout_address == 0u || (timeout_address & 3u) != 0u || !m.contains(timeout_address, 4u)) return false;
    const std::uint32_t micro = std::min(m.load32(timeout_address), 100u);
    return chance(base_percent - static_cast<int>(micro) * step_percent);
}

// On hardware these outcomes depend on the phase of the clock: the likelier one, except that a
// likely-but-not-certain one is spread evenly so a polling loop still yields sometimes.
bool Kernel::chance(int percent) {
    if (percent < 50) return false;
    if (percent >= 100) return true;
    chance_accumulator_ += percent;
    if (chance_accumulator_ >= 100) {
        chance_accumulator_ -= 100;
        return true;
    }
    return false;
}

void Kernel::reset_thread(Thread &t, std::int32_t lowest_priority) {
    reset_context(t.ctx);
    t.ctx.pc = t.entry;
    // A thread that would be better than the starting thread goes back to its initial priority.
    if (t.priority < lowest_priority) t.priority = t.init_priority;
    t.wait = WaitType::None;
    t.wait_uid = 0;
    t.wait_count = t.wait_mode = t.wait_out_address = t.wait_extra = t.timeout_address = 0u;
    t.wake_time_us = 0u;
    t.exit_status = static_cast<std::int32_t>(err::kNotDormant);
    t.cb_wait = false;
    t.wait_paused = false;
    t.calls.clear();
    auto &m = rt_.memory();
    const std::uint32_t base = t.stack_top - t.stack_size;
    if ((t.attr & kAttrNoFillStack) == 0u) {
        if (std::uint8_t *stack = m.raw_pointer(base, t.stack_size); stack != nullptr) std::memset(stack, 0xFF, t.stack_size);
        else for (std::uint32_t i = 0; i < t.stack_size; ++i) m.store8(base + i, 0xFFu);
    }
    // The k0 block takes the top 256 bytes of the stack.
    const std::uint32_t k0 = t.stack_top - 0x100u;
    for (std::uint32_t i = 0; i < 0x100u; i += 4u) m.store32(k0 + i, 0u);
    m.store32(k0 + 0xC0u, static_cast<std::uint32_t>(t.uid));
    m.store32(k0 + 0xC8u, base);
    m.store32(k0 + 0xF8u, 0xFFFFFFFFu);
    m.store32(k0 + 0xFCu, 0xFFFFFFFFu);
    m.store32(base, static_cast<std::uint32_t>(t.uid));
    t.ctx.gpr[26] = k0;
    t.ctx.gpr[29] = k0;
    t.ctx.gpr[28] = t.gp;
    t.ctx.gpr[31] = kThreadReturnAddress;
}

void Kernel::start_thread(Thread &t, std::uint32_t arglen, const std::vector<std::uint8_t> &args) {
    const Thread *cur = current();
    reset_thread(t, cur != nullptr ? cur->priority : 0);
    std::uint32_t sp = t.ctx.gpr[29];
    if (arglen != 0u && !args.empty()) {
        sp -= align_up(arglen, 16u);
        for (std::uint32_t i = 0; i < arglen && i < args.size(); ++i) rt_.memory().store8(sp + i, args[i]);
        t.ctx.gpr[4] = arglen;
        t.ctx.gpr[5] = sp;
    } else {
        t.ctx.gpr[4] = 0u;
        t.ctx.gpr[5] = 0u;
    }
    // The kernel keeps 64 bytes below the arguments.
    sp -= 64u;
    t.ctx.gpr[29] = sp;
    t.ctx.gpr[30] = sp;
    make_ready(t);
}

void Kernel::stop_thread(Thread &t, std::int32_t status) {
    tlspl_thread_end(t.uid);
    mutex_thread_end(t.uid);
    t.state = ThreadState::Dormant;
    t.suspended = false;
    t.exit_status = status;
    for (Thread *waiter : waiters(WaitType::ThreadEnd, t.uid)) wake(*waiter, static_cast<std::uint32_t>(status));
    t.wait = WaitType::None;
    t.wait_uid = 0;
    t.wake_time_us = 0u;
    t.timeout_address = 0u;
    t.cb_wait = false;
    t.wait_paused = false;
    t.calls.clear();
}

void Kernel::delete_thread(std::int32_t uid) {
    Thread *t = find_thread(uid);
    if (t == nullptr) return;
    if (t->state != ThreadState::Dormant) stop_thread(*t, static_cast<std::int32_t>(err::kThreadTerminated));
    if (t->stack_block >= 0) {
        if ((t->attr & kAttrClearStack) != 0u) rt_.memory().zero(t->stack_top - t->stack_size, t->stack_size);
        blocks_.erase(t->stack_block);
    }
    // Callbacks go with the thread that created them.
    std::erase_if(callbacks_, [uid](const auto &entry) { return entry.second.thread == uid; });
    threads_.erase(uid);
    if (current_uid_ == uid) current_uid_ = -1;
}

void Kernel::exit_thread(Ctx &ctx, std::int32_t status, bool remove) {
    Thread *t = current();
    if (t == nullptr) {
        rt_.stop("idle");
        return;
    }
    const std::int32_t uid = t->uid;
    if (const auto call = guest_calls_.find(uid); call != guest_calls_.end()) {
        // A guest function run for the kernel finished: hand its result to the thread waiting for it.
        stop_thread(*t, status);
        const std::int32_t caller_uid = call->second.caller;
        const std::uint32_t result = call->second.done ? call->second.done(static_cast<std::uint32_t>(status))
                                                       : static_cast<std::uint32_t>(status);
        guest_calls_.erase(call);
        delete_thread(uid);
        Thread *caller = find_thread(caller_uid);
        if (caller != nullptr && caller->state == ThreadState::Waiting && caller->wait == WaitType::Callback)
            wake(*caller, result);
        reschedule(ctx);
        return;
    }
    if (remove) delete_thread(uid);
    else stop_thread(*t, status);
    reschedule(ctx);
}

void Kernel::thread_returned(Ctx &ctx) {
    // The thread that ran module_start goes away when it returns, as on the PSP, freeing its stack.
    exit_thread(ctx, static_cast<std::int32_t>(ctx.gpr[2]), current_uid_ == loader_uid_);
}

// Hands the CPU to `next` at once, the running thread going to the front of its priority.
void Kernel::switch_to(Thread &next, Ctx &ctx) {
    Thread *cur = current();
    if (cur != nullptr && cur->state == ThreadState::Running) {
        cur->ctx = ctx;
        make_ready(*cur, true);
    }
    ctx = next.ctx;
    next.state = ThreadState::Running;
    current_uid_ = next.uid;
    psprecomp::set_runtime_thread_identity(next.uid, next.name);
}

// ---------------------------------------------------------------------------

void Kernel::sceKernelCreateThread(Ctx &ctx) {
    const std::uint32_t name = ctx.gpr[4], entry = ctx.gpr[5], stack_size = ctx.gpr[7];
    const auto priority = static_cast<std::int32_t>(ctx.gpr[6]);
    std::uint32_t attr = ctx.gpr[8];
    if (name == 0u) return finish(ctx, err::kError);
    if (stack_size < 0x200u) return finish(ctx, err::kIllegalStackSize);
    if (priority < kHighestPriority || priority > kLowestPriority) return finish(ctx, err::kIllegalPriority);
    if (entry != 0u && !rt_.memory().contains(entry, 4u)) return finish(ctx, err::kIllegalAddr);
    if ((attr & ~kAttrUserMask) != 0u) return finish(ctx, err::kIllegalAttr);
    attr &= ~kAttrUserErase;
    if ((attr & kAttrKernel) == 0u) attr |= kAttrUser;
    attr |= 0xFFu;

    Thread t;
    t.uid = new_uid();
    t.name = read_string(name, 31);
    t.entry = entry;
    t.priority = t.init_priority = priority;
    t.attr = attr;
    t.stack_size = align_up(stack_size, 0x100u);
    t.stack_block = allocate("stack:" + t.name, (attr & kAttrLowStack) != 0u ? 0u : 1u, t.stack_size, 0u);
    if (t.stack_block < 0) return finish(ctx, err::kNoMemory);
    t.stack_top = blocks_.at(t.stack_block).address + t.stack_size;
    t.gp = ctx.gpr[28];
    t.exit_status = static_cast<std::int32_t>(err::kDormant);
    const std::int32_t uid = t.uid;
    threads_[uid] = std::move(t);
    finish(ctx, static_cast<std::uint32_t>(uid));
}

void Kernel::sceKernelStartThread(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t arglen = ctx.gpr[5], argp = ctx.gpr[6];
    if (uid == 0) return finish(ctx, err::kIllegalThid);
    if (static_cast<std::int32_t>(arglen) < 0 || (argp & 0x80000000u) != 0u) return finish(ctx, err::kIllegalAddr);
    Thread *t = find_thread(uid);
    if (t == nullptr) return finish(ctx, err::kUnknownThid);
    if (t->state != ThreadState::Dormant) return finish(ctx, err::kNotDormant);
    std::vector<std::uint8_t> args;
    if (argp != 0u && rt_.memory().contains(argp, arglen))
        for (std::uint32_t i = 0; i < arglen; ++i) args.push_back(rt_.memory().load8(argp + i));
    start_thread(*t, args.empty() ? 0u : arglen, args);
    finish(ctx, 0u);
    // A better thread gets the CPU straight away.
    const Thread *cur = current();
    if (cur != nullptr && dispatch_enabled_ && cur->priority > t->priority) switch_to(*t, ctx);
}

void Kernel::sceKernelExitThread(Ctx &ctx) {
    auto status = static_cast<std::int32_t>(ctx.gpr[4]);
    if (status < 0) status = static_cast<std::int32_t>(err::kIllegalArgument);
    finish(ctx, 0u);
    exit_thread(ctx, status, false);
}

void Kernel::sceKernelExitDeleteThread(Ctx &ctx) {
    finish(ctx, 0u);
    exit_thread(ctx, static_cast<std::int32_t>(ctx.gpr[4]), true);
}

void Kernel::sceKernelDeleteThread(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (uid == 0 || uid == current_uid_) return finish(ctx, err::kNotDormant);
    Thread *t = find_thread(uid);
    if (t == nullptr) return finish(ctx, err::kUnknownThid);
    if (t->state != ThreadState::Dormant) return finish(ctx, err::kNotDormant);
    delete_thread(uid);
    finish(ctx, 0u);
}

void Kernel::sceKernelTerminateThread(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (uid == 0 || uid == current_uid_) return finish(ctx, err::kIllegalThid);
    Thread *t = find_thread(uid);
    if (t == nullptr) return finish(ctx, err::kUnknownThid);
    if (t->state == ThreadState::Dormant) return finish(ctx, err::kDormant);
    stop_thread(*t, static_cast<std::int32_t>(err::kThreadTerminated));
    t->priority = t->init_priority;
    finish(ctx, 0u);
}

void Kernel::sceKernelTerminateDeleteThread(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (uid == 0 || uid == current_uid_) return finish(ctx, err::kIllegalThid);
    if (find_thread(uid) == nullptr) return finish(ctx, err::kUnknownThid);
    delete_thread(uid);
    finish(ctx, 0u);
}

void Kernel::sceKernelSleepThread(Ctx &ctx) {
    Thread *t = current();
    if (t == nullptr) return finish(ctx, 0u);
    // With callbacks pending, they run first and the sleep then takes the wakeup.
    if (t->wakeup_count > 0u && !(cb_call_ && pending_callback(*t) != 0)) {
        --t->wakeup_count;
        return finish(ctx, 0u);
    }
    t->wait_uid = 0;
    block(ctx, WaitType::Sleep, 0u);
}

void Kernel::sceKernelWakeupThread(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (uid == current_uid_) return finish(ctx, err::kIllegalThid);
    Thread *t = find_thread(uid);
    if (t == nullptr) return finish(ctx, err::kUnknownThid);
    finish(ctx, 0u);
    if (waits_on(*t, WaitType::Sleep, 0)) {
        wake(*t, 0u);
        maybe_preempt(ctx);
    } else {
        ++t->wakeup_count;
    }
}

void Kernel::sceKernelCancelWakeupThread(Ctx &ctx) {
    Thread *t = ctx.gpr[4] == 0u ? current() : find_thread(static_cast<std::int32_t>(ctx.gpr[4]));
    if (t == nullptr) return finish(ctx, err::kUnknownThid);
    const std::uint32_t count = t->wakeup_count;
    t->wakeup_count = 0u;
    finish(ctx, count);
}

// A delay never ends early: at least 210us, else about 10us more than asked. If the deadline has
// passed by the time the alarm is set (likely for 0us, rare for 1us, more so on a thread's first
// wait), the call returns at once.
void Kernel::delay_current(Ctx &ctx, std::uint64_t usec) {
    Thread *t = current();
    if (t == nullptr) return finish(ctx, 0u);
    const int gap_percent = t->has_waited ? 60 : 165;
    if (chance(gap_percent - static_cast<int>(std::min<std::uint64_t>(usec, 2u)) * 100) &&
        !(cb_call_ && pending_callback(*t) != 0))
        return finish(ctx, 0u);
    if (usec > 0x8000000000000000ull) usec -= 0x8000000000000000ull;
    if (usec > 0x0010000000000000ull) usec >>= 12u;
    const std::uint64_t delay = usec < 200u ? 210u : usec + 10u;
    t->wake_time_us = now_us() + delay;
    t->wait_uid = t->uid;
    t->wait_extra = 0u;
    block(ctx, WaitType::Delay, 0u);
}

void Kernel::sceKernelDelayThread(Ctx &ctx) { delay_current(ctx, ctx.gpr[4]); }

void Kernel::sceKernelDelaySysClockThread(Ctx &ctx) {
    auto &m = rt_.memory();
    const std::uint32_t clock = ctx.gpr[4];
    if (!m.contains(clock, 8u)) return finish(ctx, 0x8002006Au); // ILLEGAL_ADDRESS
    delay_current(ctx, m.load32(clock) | static_cast<std::uint64_t>(m.load32(clock + 4u)) << 32u);
}

void Kernel::sceKernelGetThreadId(Ctx &ctx) { finish(ctx, static_cast<std::uint32_t>(current_uid_)); }

void Kernel::sceKernelGetThreadCurrentPriority(Ctx &ctx) {
    const Thread *t = current();
    finish(ctx, t != nullptr ? static_cast<std::uint32_t>(t->priority) : 0x20u);
}

void Kernel::sceKernelChangeThreadPriority(Ctx &ctx) {
    auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    auto priority = static_cast<std::int32_t>(ctx.gpr[5]);
    if (uid == 0) uid = current_uid_;
    // 0 means the running thread's priority, not the target's.
    if (priority == 0) {
        if (const Thread *cur = current(); cur != nullptr) priority = cur->priority;
    }
    Thread *t = find_thread(uid);
    if (t == nullptr) return finish(ctx, err::kUnknownThid);
    if (t->state == ThreadState::Dormant) return finish(ctx, err::kDormant);
    if (priority < kHighestPriority || priority > kLowestPriority) return finish(ctx, err::kIllegalPriority);
    t->priority = priority;
    finish(ctx, 0u);
    // A running or ready thread goes to the back of its new priority.
    if (t->state == ThreadState::Ready) make_ready(*t);
    if (t->state == ThreadState::Running) {
        t->ctx = ctx;
        make_ready(*t);
        if (dispatch_enabled_) reschedule(ctx);
        else load_back(*t, ctx);
        return;
    }
    maybe_preempt(ctx);
}

// The running thread stays on the CPU (dispatch disabled): undo a make_ready.
void Kernel::load_back(Thread &t, Ctx &ctx) {
    t.state = ThreadState::Running;
    ctx = t.ctx;
}

void Kernel::sceKernelRotateThreadReadyQueue(Ctx &ctx) {
    Thread *cur = current();
    auto priority = static_cast<std::int32_t>(ctx.gpr[4]);
    if (cur == nullptr) return finish(ctx, 0u);
    if (priority == 0) priority = cur->priority;
    if (priority < kHighestPriority || priority > kLowestPriority) return finish(ctx, err::kIllegalPriority);
    finish(ctx, 0u);
    bool any_ready = false;
    Thread *first = nullptr;
    for (auto &[uid, t] : threads_) {
        (void)uid;
        if (t.state != ThreadState::Ready || t.suspended || t.priority != priority) continue;
        any_ready = true;
        if (first == nullptr || t.ready_sequence < first->ready_sequence) first = &t;
    }
    if (!any_ready) return;
    if (cur->priority == priority) {
        // Yield to the other threads of this priority.
        cur->ctx = ctx;
        make_ready(*cur);
        if (dispatch_enabled_) reschedule(ctx);
        else load_back(*cur, ctx);
        return;
    }
    // The first ready thread of that priority goes to the back.
    make_ready(*first);
    maybe_preempt(ctx);
}

void Kernel::sceKernelWaitThreadEnd(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t timeout = ctx.gpr[5];
    if (uid == 0 || uid == current_uid_) return finish(ctx, err::kIllegalThid);
    if (!dispatch_ok()) return finish(ctx, err::kCanNotWait);
    Thread *target = find_thread(uid);
    if (target == nullptr) return finish(ctx, err::kUnknownThid);
    if (target->state == ThreadState::Dormant) return finish(ctx, static_cast<std::uint32_t>(target->exit_status));
    if (timeout_at_once(timeout)) return finish(ctx, err::kWaitTimeout);
    Thread *self = current();
    if (self != nullptr) {
        self->wait_uid = uid;
        begin_timeout(*self, rt_.memory().contains(timeout, 4u) ? timeout : 0u);
    }
    block(ctx, WaitType::ThreadEnd, 0u);
}

void Kernel::sceKernelReleaseWaitThread(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (uid == 0 || uid == current_uid_) return finish(ctx, err::kIllegalThid);
    Thread *t = find_thread(uid);
    if (t == nullptr) return finish(ctx, err::kUnknownThid);
    if (t->state != ThreadState::Waiting || t->wait == WaitType::Callback) return finish(ctx, err::kNotWait);
    if (t->timeout_address != 0u) rt_.memory().store32(t->timeout_address, 0u);
    t->timeout_address = 0u;
    wake(*t, err::kReleaseWait);
    finish(ctx, 0u);
    maybe_preempt(ctx);
}

void Kernel::sceKernelSuspendThread(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (uid == 0 || uid == current_uid_) return finish(ctx, err::kIllegalThid);
    Thread *t = find_thread(uid);
    if (t == nullptr) return finish(ctx, err::kUnknownThid);
    if (t->state == ThreadState::Dormant) return finish(ctx, err::kDormant);
    if (t->suspended) return finish(ctx, err::kSuspend);
    t->suspended = true;
    finish(ctx, 0u);
}

void Kernel::sceKernelResumeThread(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (uid == 0 || uid == current_uid_) return finish(ctx, err::kIllegalThid);
    Thread *t = find_thread(uid);
    if (t == nullptr) return finish(ctx, err::kUnknownThid);
    if (!t->suspended) return finish(ctx, err::kNotSuspend);
    t->suspended = false;
    // A thread that was ready joins the back of its priority.
    if (t->state == ThreadState::Ready) make_ready(*t);
    finish(ctx, 0u);
    maybe_preempt(ctx);
}

void Kernel::sceKernelGetThreadExitStatus(Ctx &ctx) {
    const Thread *t = find_thread(static_cast<std::int32_t>(ctx.gpr[4]));
    if (t == nullptr) return finish(ctx, err::kUnknownThid);
    finish(ctx, t->state == ThreadState::Dormant ? static_cast<std::uint32_t>(t->exit_status) : err::kNotDormant);
}

// SceKernelThreadInfo: size, name[32], attr, status, entry, stack, stackSize, gpReg, initPriority,
// currentPriority, waitType, waitId, wakeupCount, exitStatus, runClocks (8), intrPreemptCount,
// threadPreemptCount, releaseCount, (from SDK 2.6) one more word.
void Kernel::sceKernelReferThreadStatus(Ctx &ctx) {
    const auto uid = ctx.gpr[4] == 0u ? current_uid_ : static_cast<std::int32_t>(ctx.gpr[4]);
    const Thread *t = find_thread(uid);
    if (t == nullptr) return finish(ctx, err::kUnknownThid);
    auto &m = rt_.memory();
    const std::uint32_t info = ctx.gpr[5];
    if ((info & 3u) != 0u || !m.contains(info, 4u)) return finish(ctx, 0u);
    const std::uint32_t wanted = m.load32(info);
    const bool after260 = sdk_version_ > 0x02060010u;
    if (after260 && wanted > kThreadInfoSizeAfter260) return finish(ctx, err::kIllegalSize);
    const std::uint32_t native = after260 ? kThreadInfoSizeAfter260 : kThreadInfoSize;
    std::uint8_t record[kThreadInfoSizeAfter260]{};
    const auto put32 = [&record](std::size_t at, std::uint32_t value) {
        for (std::size_t i = 0; i < 4u; ++i) record[at + i] = static_cast<std::uint8_t>(value >> (8u * i));
    };
    put32(0x00, native);
    for (std::size_t i = 0; i < 31u && i < t->name.size(); ++i) record[4 + i] = static_cast<std::uint8_t>(t->name[i]);
    put32(0x24, t->attr);
    put32(0x28, psp_thread_status(*t));
    put32(0x2C, t->entry);
    put32(0x30, t->stack_top - t->stack_size);
    put32(0x34, t->stack_size);
    put32(0x38, t->gp);
    put32(0x3C, static_cast<std::uint32_t>(t->init_priority));
    put32(0x40, static_cast<std::uint32_t>(t->priority));
    const bool waiting = t->state == ThreadState::Waiting;
    put32(0x44, waiting ? psp_wait_type(*t) : 0u);
    put32(0x48, waiting ? static_cast<std::uint32_t>(t->wait_uid) : 0u);
    put32(0x4C, t->wakeup_count);
    put32(0x50, static_cast<std::uint32_t>(t->exit_status));
    put32(0x64, t->releases);
    const std::uint32_t length = std::min(wanted, native);
    if (length != 0u) m.copy_in(info, std::span<const std::uint8_t>(record, length));
    finish(ctx, 0u);
}

// SceKernelThreadRunStatus: size, status, currentPriority, waitType, waitId, wakeupCount,
// runClocks (8), intrPreemptCount, threadPreemptCount, releaseCount.
void Kernel::sceKernelReferThreadRunStatus(Ctx &ctx) {
    const auto uid = ctx.gpr[4] == 0u ? current_uid_ : static_cast<std::int32_t>(ctx.gpr[4]);
    const Thread *t = find_thread(uid);
    if (t == nullptr) return finish(ctx, err::kUnknownThid);
    auto &m = rt_.memory();
    const std::uint32_t info = ctx.gpr[5];
    if (!m.contains(info, 0x2Cu)) return finish(ctx, 0xFFFFFFFFu);
    m.zero(info, 0x2Cu);
    m.store32(info, 0x2Cu);
    m.store32(info + 4u, psp_thread_status(*t));
    m.store32(info + 8u, static_cast<std::uint32_t>(t->priority));
    const bool waiting = t->state == ThreadState::Waiting;
    m.store32(info + 12u, waiting ? psp_wait_type(*t) : 0u);
    m.store32(info + 16u, waiting ? static_cast<std::uint32_t>(t->wait_uid) : 0u);
    m.store32(info + 20u, t->wakeup_count);
    m.store32(info + 40u, t->releases);
    finish(ctx, 0u);
}

void Kernel::sceKernelChangeCurrentThreadAttr(Ctx &ctx) {
    const std::uint32_t clear = ctx.gpr[4], set = ctx.gpr[5];
    if ((clear & ~kAttrVfpu) != 0u || (set & ~kAttrVfpu) != 0u) return finish(ctx, err::kIllegalAttr);
    Thread *t = current();
    if (t == nullptr) return finish(ctx, 0xFFFFFFFFu);
    t->attr = (t->attr & ~clear) | set;
    finish(ctx, 0u);
}

// Bytes at the bottom of the stack still holding the 0xFF fill (after the thread id at 0).
void Kernel::sceKernelGetThreadStackFreeSize(Ctx &ctx) {
    const auto uid = ctx.gpr[4] == 0u ? current_uid_ : static_cast<std::int32_t>(ctx.gpr[4]);
    const Thread *t = find_thread(uid);
    if (t == nullptr) return finish(ctx, err::kUnknownThid);
    auto &m = rt_.memory();
    const std::uint32_t base = t->stack_top - t->stack_size;
    std::uint32_t size = 0u;
    for (std::uint32_t offset = 0x10u; offset < t->stack_size && m.load8(base + offset) == 0xFFu; ++offset) ++size;
    finish(ctx, size & ~3u);
}

void Kernel::sceKernelCheckThreadStack(Ctx &ctx) {
    const Thread *t = current();
    if (t == nullptr) return finish(ctx, 0u);
    const std::uint32_t sp = ctx.gpr[29], base = t->stack_top - t->stack_size;
    finish(ctx, sp > base && sp <= t->stack_top ? sp - base : 0u);
}

void Kernel::sceKernelSuspendDispatchThread(Ctx &ctx) {
    const bool was = dispatch_enabled_;
    dispatch_enabled_ = false;
    finish(ctx, was ? 1u : 0u);
}

void Kernel::sceKernelResumeDispatchThread(Ctx &ctx) {
    dispatch_enabled_ = ctx.gpr[4] != 0u;
    finish(ctx, 0u);
    if (dispatch_enabled_) maybe_preempt(ctx);
}

std::uint32_t Kernel::threadman_type(std::int32_t uid) const {
    if (threads_.contains(uid)) return kTmidThread;
    if (semaphores_.contains(uid)) return kTmidSemaphore;
    if (event_flags_.contains(uid)) return kTmidEventFlag;
    if (mailboxes_.contains(uid)) return kTmidMbox;
    if (vpls_.contains(uid)) return kTmidVpl;
    if (fpls_.contains(uid)) return kTmidFpl;
    if (msgpipes_.contains(uid)) return kTmidMpipe;
    if (callbacks_.contains(uid)) return kTmidCallback;
    if (alarms_.contains(uid)) return kTmidAlarm;
    if (vtimers_.contains(uid)) return kTmidVTimer;
    if (mutexes_.contains(uid)) return kTmidMutex;
    if (lwmutexes_.contains(uid)) return kTmidLwMutex;
    if (tlspls_.contains(uid)) return kTmidTlspl;
    return 0u;
}

void Kernel::sceKernelGetThreadmanIdType(Ctx &ctx) {
    const std::uint32_t type = threadman_type(static_cast<std::int32_t>(ctx.gpr[4]));
    finish(ctx, type != 0u ? type : err::kIllegalArgument);
}

void Kernel::sceKernelGetThreadmanIdList(Ctx &ctx) {
    const std::uint32_t type = ctx.gpr[4], buffer = ctx.gpr[5], size = ctx.gpr[6], count_out = ctx.gpr[7];
    auto &m = rt_.memory();
    if (size >= 0x8000000u) return finish(ctx, err::kIllegalAddr);
    if (size > 0u && !m.contains(buffer, 4u)) return finish(ctx, err::kIllegalArgument);
    std::vector<std::int32_t> ids;
    const auto add_keys = [&ids](const auto &map) {
        for (const auto &entry : map) ids.push_back(entry.first);
    };
    if (type >= kTmidThread && type <= kTmidTlspl) {
        switch (type) {
        case kTmidThread: add_keys(threads_); break;
        case kTmidSemaphore: add_keys(semaphores_); break;
        case kTmidEventFlag: add_keys(event_flags_); break;
        case kTmidMbox: add_keys(mailboxes_); break;
        case kTmidVpl: add_keys(vpls_); break;
        case kTmidFpl: add_keys(fpls_); break;
        case kTmidMpipe: add_keys(msgpipes_); break;
        case kTmidCallback: add_keys(callbacks_); break;
        case kTmidAlarm: add_keys(alarms_); break;
        case kTmidVTimer: add_keys(vtimers_); break;
        case kTmidMutex: add_keys(mutexes_); break;
        case kTmidLwMutex: add_keys(lwmutexes_); break;
        case kTmidTlspl: add_keys(tlspls_); break;
        default: break;
        }
    } else if (type >= kTmidSleepThread && type <= kTmidDormantThread) {
        for (const auto &[uid, t] : threads_) {
            bool match = false;
            if (type == kTmidSleepThread) match = t.state == ThreadState::Waiting && t.wait == WaitType::Sleep;
            else if (type == kTmidDelayThread) match = t.state == ThreadState::Waiting && t.wait == WaitType::Delay;
            else if (type == kTmidSuspendThread) match = t.suspended;
            else match = t.state == ThreadState::Dormant;
            if (match) ids.push_back(uid);
        }
    } else {
        return finish(ctx, err::kIllegalType);
    }
    const auto total = static_cast<std::uint32_t>(ids.size());
    for (std::uint32_t i = 0; i < total && i < size; ++i) m.store32(buffer + i * 4u, static_cast<std::uint32_t>(ids[i]));
    if (count_out != 0u && m.contains(count_out, 4u)) m.store32(count_out, total);
    finish(ctx, std::min(total, size));
}

void Kernel::install_threads() {
    const char *tm = "ThreadManForUser";
    hle(tm, 0x446D8DE6u, "sceKernelCreateThread", &Kernel::sceKernelCreateThread);
    hle(tm, 0xF475845Du, "sceKernelStartThread", &Kernel::sceKernelStartThread);
    hle(tm, 0xAA73C935u, "sceKernelExitThread", &Kernel::sceKernelExitThread);
    hle(tm, 0x809CE29Bu, "sceKernelExitDeleteThread", &Kernel::sceKernelExitDeleteThread);
    hle(tm, 0x9FA03CD3u, "sceKernelDeleteThread", &Kernel::sceKernelDeleteThread);
    hle(tm, 0x616403BAu, "sceKernelTerminateThread", &Kernel::sceKernelTerminateThread);
    hle(tm, 0x383F7BCCu, "sceKernelTerminateDeleteThread", &Kernel::sceKernelTerminateDeleteThread);
    hle(tm, 0x9ACE131Eu, "sceKernelSleepThread", &Kernel::sceKernelSleepThread);
    hle_cb(tm, 0x82826F70u, "sceKernelSleepThreadCB", &Kernel::sceKernelSleepThread);
    hle(tm, 0xD59EAD2Fu, "sceKernelWakeupThread", &Kernel::sceKernelWakeupThread);
    hle(tm, 0xFCCFAD26u, "sceKernelCancelWakeupThread", &Kernel::sceKernelCancelWakeupThread);
    hle(tm, 0xCEADEB47u, "sceKernelDelayThread", &Kernel::sceKernelDelayThread);
    hle_cb(tm, 0x68DA9E36u, "sceKernelDelayThreadCB", &Kernel::sceKernelDelayThread);
    hle(tm, 0xBD123D9Eu, "sceKernelDelaySysClockThread", &Kernel::sceKernelDelaySysClockThread);
    hle_cb(tm, 0x1181E963u, "sceKernelDelaySysClockThreadCB", &Kernel::sceKernelDelaySysClockThread);
    hle(tm, 0x293B45B8u, "sceKernelGetThreadId", &Kernel::sceKernelGetThreadId);
    hle("Kernel_Library", 0x293B45B8u, "sceKernelGetThreadId", &Kernel::sceKernelGetThreadId);
    hle(tm, 0x94AA61EEu, "sceKernelGetThreadCurrentPriority", &Kernel::sceKernelGetThreadCurrentPriority);
    hle(tm, 0x71BC9871u, "sceKernelChangeThreadPriority", &Kernel::sceKernelChangeThreadPriority);
    hle(tm, 0x912354A7u, "sceKernelRotateThreadReadyQueue", &Kernel::sceKernelRotateThreadReadyQueue);
    hle(tm, 0x278C0DF5u, "sceKernelWaitThreadEnd", &Kernel::sceKernelWaitThreadEnd);
    hle_cb(tm, 0x840E8133u, "sceKernelWaitThreadEndCB", &Kernel::sceKernelWaitThreadEnd);
    hle(tm, 0x2C34E053u, "sceKernelReleaseWaitThread", &Kernel::sceKernelReleaseWaitThread);
    hle(tm, 0x9944F31Fu, "sceKernelSuspendThread", &Kernel::sceKernelSuspendThread);
    hle(tm, 0x75156E8Fu, "sceKernelResumeThread", &Kernel::sceKernelResumeThread);
    hle(tm, 0x3B183E26u, "sceKernelGetThreadExitStatus", &Kernel::sceKernelGetThreadExitStatus);
    hle(tm, 0x17C1684Eu, "sceKernelReferThreadStatus", &Kernel::sceKernelReferThreadStatus);
    hle(tm, 0xFFC36A14u, "sceKernelReferThreadRunStatus", &Kernel::sceKernelReferThreadRunStatus);
    hle(tm, 0xEA748E31u, "sceKernelChangeCurrentThreadAttr", &Kernel::sceKernelChangeCurrentThreadAttr);
    hle(tm, 0x52089CA1u, "sceKernelGetThreadStackFreeSize", &Kernel::sceKernelGetThreadStackFreeSize);
    hle(tm, 0xD13BDE95u, "sceKernelCheckThreadStack", &Kernel::sceKernelCheckThreadStack);
    hle(tm, 0x3AD58B8Cu, "sceKernelSuspendDispatchThread", &Kernel::sceKernelSuspendDispatchThread);
    hle(tm, 0x27E22EC2u, "sceKernelResumeDispatchThread", &Kernel::sceKernelResumeDispatchThread);
    hle(tm, 0x57CF62DDu, "sceKernelGetThreadmanIdType", &Kernel::sceKernelGetThreadmanIdType);
    hle(tm, 0x94416130u, "sceKernelGetThreadmanIdList", &Kernel::sceKernelGetThreadmanIdList);
}

} // namespace pspweb
