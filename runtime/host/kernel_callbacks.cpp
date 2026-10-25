// SPDX-License-Identifier: GPL-2.0-or-later
//
// Callbacks (sceKernel*Callback) and the CB variants of the waits. A callback belongs to the
// thread that created it and runs on that thread, with its stack and registers, when the thread
// is in a CB wait (sceKernelSleepThreadCB, sceKernelWaitSemaCB, ...) or calls
// sceKernelCheckCallback. A notification sets the wait aside; once the thread would get to run,
// its callbacks run one after another and the wait then resumes, unless it can end by then.
// Semantics follow PPSSPP (Core/HLE/sceKernelThread.cpp, KernelWaitHelpers.h; GPL-2.0-or-later).

#include "kernel.hpp"

#include <algorithm>
#include <limits>

namespace pspweb {
namespace {

constexpr std::uint32_t kErrorError = 0x80020001u;
constexpr std::uint32_t kErrorIllegalContext = 0x80020064u;
constexpr std::uint32_t kErrorIllegalAddr = 0x800200D3u;
constexpr std::uint32_t kErrorIllegalArgument = 0x800200D2u;
constexpr std::uint32_t kErrorUnknownCallback = 0x800201A1u;
constexpr std::uint32_t kErrorWaitTimeout = 0x800201A8u;
constexpr std::uint32_t kErrorWaitDelete = 0x800201B5u;

constexpr std::uint32_t kEventWaitOr = 0x01u;
constexpr std::uint32_t kEventWaitClearAll = 0x10u;
constexpr std::uint32_t kEventWaitClear = 0x20u;

// Stack the kernel takes below a thread's stack pointer before running a callback on it.
constexpr std::uint32_t kCallbackStackUse = 0x80u;

void callback_return_trampoline(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx);
Kernel *g_callback_kernel = nullptr;

void callback_return_trampoline(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    g_callback_kernel->callback_returned(ctx);
}

} // namespace

void Kernel::hle_cb(const char *library, std::uint32_t nid, const char *name, void (Kernel::*handler)(Ctx &)) {
    hle(library, nid, name, handler);
    rt_.register_hle(library, nid, [this, handler](psprecomp::Runtime &, Ctx &ctx) {
        cb_call_ = true;
        (this->*handler)(ctx);
        cb_call_ = false;
    });
}

std::vector<Thread *> Kernel::waiters(WaitType wait, std::int32_t uid, bool by_priority) {
    std::vector<Thread *> list;
    for (auto &[tid, t] : threads_) {
        (void)tid;
        if (waits_on(t, wait, uid)) list.push_back(&t);
    }
    std::sort(list.begin(), list.end(), [by_priority](const Thread *a, const Thread *b) {
        if (by_priority && a->priority != b->priority) return a->priority < b->priority;
        return a->wait_sequence < b->wait_sequence;
    });
    return list;
}

std::int32_t Kernel::pending_callback(const Thread &t) const {
    for (const auto &[uid, cb] : callbacks_)
        if (cb.thread == t.uid && cb.notify_count != 0) return uid;
    return 0;
}

bool Kernel::start_pending_callback(Thread &t, Ctx &ctx, bool waiting, bool reschedule_after) {
    const std::int32_t uid = pending_callback(t);
    if (uid == 0) return false;
    Callback &cb = callbacks_.at(uid);
    CallFrame frame;
    frame.ctx = ctx;
    frame.callback = uid;
    frame.waiting = waiting;
    frame.reschedule_after = reschedule_after;
    frame.wait = t.wait;
    frame.wait_uid = t.wait_uid;
    frame.wait_count = t.wait_count;
    frame.wait_mode = t.wait_mode;
    frame.wait_out_address = t.wait_out_address;
    frame.wait_extra = t.wait_extra;
    frame.wait_result_address = t.wait_result_address;
    frame.timeout_address = t.timeout_address;
    frame.wake_time_us = t.wake_time_us;
    frame.wait_sequence = t.wait_sequence;
    t.calls.push_back(frame);

    ctx.gpr[29] -= kCallbackStackUse;
    ctx.gpr[4] = static_cast<std::uint32_t>(cb.notify_count);
    ctx.gpr[5] = static_cast<std::uint32_t>(cb.notify_arg);
    ctx.gpr[6] = cb.common;
    ctx.gpr[31] = kCallbackReturnAddress;
    ctx.pc = cb.entry;
    cb.notify_count = 0;
    cb.notify_arg = 0;

    t.wait = WaitType::None;
    t.cb_wait = false;
    t.wait_paused = false;
    t.wake_time_us = 0u;
    t.timeout_address = 0u;
    if (t.uid == current_uid_) t.state = ThreadState::Running;
    else make_ready(t);
    return true;
}

void Kernel::callback_returned(Ctx &ctx) {
    Thread *t = current();
    if (t == nullptr || t->calls.empty()) {
        rt_.stop("callback returned on a thread that runs no callback");
        return;
    }
    const std::uint32_t result = ctx.gpr[2];
    const CallFrame frame = t->calls.back();
    t->calls.pop_back();
    // A callback that returns non-zero is deleted.
    if (result != 0u) callbacks_.erase(frame.callback);

    ctx = frame.ctx;
    t->wait = frame.wait;
    t->wait_uid = frame.wait_uid;
    t->wait_count = frame.wait_count;
    t->wait_mode = frame.wait_mode;
    t->wait_out_address = frame.wait_out_address;
    t->wait_extra = frame.wait_extra;
    t->wait_result_address = frame.wait_result_address;
    t->timeout_address = frame.timeout_address;
    t->wake_time_us = frame.wake_time_us;
    t->wait_sequence = frame.wait_sequence;
    // Callbacks notified meanwhile run before the thread goes back to what it was doing.
    if (start_pending_callback(*t, ctx, frame.waiting, frame.reschedule_after)) return;

    t->state = ThreadState::Running;
    if (frame.waiting) {
        if (!resume_wait(*t, ctx)) {
            t->ctx = ctx;
            reschedule(ctx);
            return;
        }
    } else {
        ctx.gpr[2] = 1u; // sceKernelCheckCallback ran callbacks
        t->wait = WaitType::None;
    }
    if (frame.reschedule_after) maybe_preempt(ctx);
}

bool Kernel::wait_object_exists(const Thread &t) const {
    switch (t.wait) {
    case WaitType::Sema: return semaphores_.contains(t.wait_uid);
    case WaitType::EventFlag: return event_flags_.contains(t.wait_uid);
    case WaitType::Mbx: return mailboxes_.contains(t.wait_uid);
    case WaitType::Mutex: return mutexes_.contains(t.wait_uid);
    case WaitType::LwMutex: return lwmutexes_.contains(t.wait_uid);
    case WaitType::Fpl: return fpls_.contains(t.wait_uid);
    case WaitType::Vpl: return vpls_.contains(t.wait_uid);
    case WaitType::ThreadEnd: return threads_.contains(t.wait_uid);
    case WaitType::MsgPipeSend:
    case WaitType::MsgPipeReceive: return msgpipes_.contains(t.wait_uid);
    case WaitType::Tlspl: return tlspls_.contains(t.wait_uid);
    default: return true;
    }
}

bool Kernel::try_complete_wait(Thread &t, std::uint32_t &result) {
    auto &m = rt_.memory();
    result = 0u;
    switch (t.wait) {
    case WaitType::Sleep:
        if (t.wakeup_count == 0u) return false;
        --t.wakeup_count;
        return true;
    case WaitType::ThreadEnd: {
        const Thread &target = threads_.at(t.wait_uid);
        if (target.state != ThreadState::Dormant && target.state != ThreadState::Dead) return false;
        result = static_cast<std::uint32_t>(target.exit_status);
        return true;
    }
    case WaitType::Sema: return try_satisfy_sema(t);
    case WaitType::EventFlag: return try_satisfy_event(t);
    case WaitType::Mbx: {
        Mailbox &box = mailboxes_.at(t.wait_uid);
        if (box.count == 0) return false;
        result = mbx_receive(box, t.wait_out_address);
        return true;
    }
    case WaitType::Mutex: {
        Mutex &mutex = mutexes_.at(t.wait_uid);
        if (mutex.count != 0) return false;
        mutex.owner = t.uid;
        mutex.count = static_cast<std::int32_t>(t.wait_count);
        return true;
    }
    case WaitType::LwMutex: {
        const std::uint32_t workarea = lwmutexes_.at(t.wait_uid).workarea;
        if (m.load32(workarea) != 0u) return false;
        m.store32(workarea, t.wait_count);
        m.store32(workarea + 4u, static_cast<std::uint32_t>(t.uid));
        return true;
    }
    case WaitType::Fpl: {
        const std::uint32_t address = fpl_allocate(fpls_.at(t.wait_uid));
        if (address == 0u) return false;
        if (m.contains(t.wait_out_address, 4u)) m.store32(t.wait_out_address, address);
        return true;
    }
    case WaitType::Vpl: {
        const std::uint32_t address = vpl_allocate(vpls_.at(t.wait_uid), t.wait_count);
        if (address == 0u) return false;
        if (m.contains(t.wait_out_address, 4u)) m.store32(t.wait_out_address, address);
        return true;
    }
    case WaitType::MsgPipeSend:
    case WaitType::MsgPipeReceive:
        return msgpipe_resume(t, result);
    case WaitType::Tlspl: {
        const std::uint32_t address = tlspl_allocate(tlspls_.at(t.wait_uid), t.uid);
        if (address == 0u) return false;
        if (t.wait_out_address != 0u) m.store32(t.wait_out_address, address);
        else result = address;
        return true;
    }
    case WaitType::Vblank:
        return true;
    default:
        return false;
    }
}

bool Kernel::resume_wait(Thread &t, Ctx &ctx) {
    t.cb_wait = true;
    t.wait_paused = false;
    std::uint32_t result = 0u;
    bool done = false;
    if (!wait_object_exists(t)) {
        if (t.timeout_address != 0u) rt_.memory().store32(t.timeout_address, 0u);
        t.timeout_address = 0u;
        result = kErrorWaitDelete;
        done = true;
    } else if (try_complete_wait(t, result)) {
        done = true;
    } else if (t.wake_time_us != 0u && now_us() >= t.wake_time_us) {
        result = t.wait == WaitType::Delay ? t.wait_extra : kErrorWaitTimeout;
        if (t.timeout_address != 0u) rt_.memory().store32(t.timeout_address, 0u);
        t.timeout_address = 0u;
        done = true;
    }
    if (!done) {
        t.state = ThreadState::Waiting;
        t.wait_sequence = ++wait_sequence_;
        return false;
    }
    const ThreadState state = t.state;
    t.ctx = ctx;
    wake(t, result);
    ctx = t.ctx;
    t.state = state;
    t.cb_wait = false;
    return true;
}

void Kernel::dispatch_callbacks() {
    if (callbacks_.empty()) return;
    std::int32_t best = std::numeric_limits<std::int32_t>::max();
    if (const Thread *cur = current(); cur != nullptr && cur->state == ThreadState::Running) best = cur->priority;
    if (const Thread *ready = pick_ready(); ready != nullptr) best = std::min(best, ready->priority);
    for (auto &[uid, t] : threads_) {
        (void)uid;
        if (t.state != ThreadState::Waiting || !t.cb_wait || t.suspended || t.priority >= best) continue;
        // A callback runs nested at most one level deep.
        if (t.calls.size() > 1u) continue;
        if (start_pending_callback(t, t.ctx, true, true)) continue;
        if (t.wait_paused) {
            // Its callbacks were cancelled meanwhile: back to the wait, which may have ended.
            Ctx saved = t.ctx;
            if (resume_wait(t, saved)) {
                t.ctx = saved;
                make_ready(t);
            }
        }
    }
}

// ---------------------------------------------------------------------------

void Kernel::install_callbacks() {
    g_callback_kernel = this;
    rt_.register_function(kCallbackReturnAddress, &callback_return_trampoline, "pspweb_callback_return");
    const char *tm = "ThreadManForUser";
    hle(tm, 0xE81CAF8Fu, "sceKernelCreateCallback", &Kernel::sceKernelCreateCallback);
    hle(tm, 0xEDBA5844u, "sceKernelDeleteCallback", &Kernel::sceKernelDeleteCallback);
    hle(tm, 0xC11BA8C4u, "sceKernelNotifyCallback", &Kernel::sceKernelNotifyCallback);
    hle(tm, 0xBA4051D6u, "sceKernelCancelCallback", &Kernel::sceKernelCancelCallback);
    hle(tm, 0x2A3D44FFu, "sceKernelGetCallbackCount", &Kernel::sceKernelGetCallbackCount);
    hle(tm, 0x730ED8BCu, "sceKernelReferCallbackStatus", &Kernel::sceKernelReferCallbackStatus);
    hle(tm, 0x349D6D6Cu, "sceKernelCheckCallback", &Kernel::sceKernelCheckCallback);
    hle("LoadExecForUser", 0x4AC57943u, "sceKernelRegisterExitCallback", &Kernel::sceKernelRegisterExitCallback);
    hle("LoadExecForUser", 0x362A956Bu, "LoadExecForUser_362A956B", &Kernel::LoadExecForUser_362A956B);
}

void Kernel::sceKernelCreateCallback(Ctx &ctx) {
    const std::uint32_t name = ctx.gpr[4], entry = ctx.gpr[5];
    if (name == 0u) return finish(ctx, kErrorError);
    if ((entry & 0xF0000000u) != 0u) return finish(ctx, kErrorIllegalAddr);
    const std::int32_t uid = new_uid();
    callbacks_[uid] = Callback{read_string(name, 31), entry, ctx.gpr[6], current_uid_, 0, 0};
    finish(ctx, static_cast<std::uint32_t>(uid));
}

void Kernel::sceKernelDeleteCallback(Ctx &ctx) {
    finish(ctx, callbacks_.erase(static_cast<std::int32_t>(ctx.gpr[4])) != 0u ? 0u : kErrorUnknownCallback);
}

void Kernel::sceKernelNotifyCallback(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (!callbacks_.contains(uid)) return finish(ctx, kErrorUnknownCallback);
    finish(ctx, 0u);
    notify_callback(ctx, uid, static_cast<std::int32_t>(ctx.gpr[5]));
}

void Kernel::notify_callback(Ctx &ctx, std::int32_t uid, std::int32_t arg) {
    const auto it = callbacks_.find(uid);
    if (it == callbacks_.end()) return;
    Callback &cb = it->second;
    ++cb.notify_count;
    cb.notify_arg = arg;
    // A thread in a CB wait leaves the wait at once; its callbacks run once it would get to run.
    Thread *t = thread(cb.thread);
    if (t == nullptr || t->uid == current_uid_ || t->state != ThreadState::Waiting || !t->cb_wait) return;
    t->wait_paused = true;
    dispatch_callbacks();
    maybe_preempt(ctx);
}

void Kernel::sceKernelCancelCallback(Ctx &ctx) {
    const auto it = callbacks_.find(static_cast<std::int32_t>(ctx.gpr[4]));
    if (it == callbacks_.end()) return finish(ctx, kErrorUnknownCallback);
    it->second.notify_count = 0;
    it->second.notify_arg = 0;
    finish(ctx, 0u);
}

void Kernel::sceKernelGetCallbackCount(Ctx &ctx) {
    const auto it = callbacks_.find(static_cast<std::int32_t>(ctx.gpr[4]));
    finish(ctx, it == callbacks_.end() ? kErrorUnknownCallback : static_cast<std::uint32_t>(it->second.notify_count));
}

// SceKernelCallbackInfo: size, name[32], threadId, callback, common, notifyCount, notifyArg.
void Kernel::sceKernelReferCallbackStatus(Ctx &ctx) {
    const auto it = callbacks_.find(static_cast<std::int32_t>(ctx.gpr[4]));
    if (it == callbacks_.end()) return finish(ctx, kErrorUnknownCallback);
    const std::uint32_t info = ctx.gpr[5];
    auto &m = rt_.memory();
    if (info == 0u || !m.contains(info, 4u) || m.load32(info) == 0u) return finish(ctx, 0u);
    const Callback &cb = it->second;
    m.store32(info, 0x38u);
    write_object_name(info, cb.name);
    m.store32(info + 0x24u, static_cast<std::uint32_t>(cb.thread));
    m.store32(info + 0x28u, cb.entry);
    m.store32(info + 0x2Cu, cb.common);
    m.store32(info + 0x30u, static_cast<std::uint32_t>(cb.notify_count));
    m.store32(info + 0x34u, static_cast<std::uint32_t>(cb.notify_arg));
    finish(ctx, 0u);
}

void Kernel::sceKernelCheckCallback(Ctx &ctx) {
    Thread *t = current();
    if (t == nullptr) return finish(ctx, 0u);
    // Unlike a CB wait, this refuses to run callbacks from inside one.
    if (!t->calls.empty()) return finish(ctx, kErrorIllegalContext);
    finish(ctx, 0u);
    t->wait = WaitType::None;
    start_pending_callback(*t, ctx, false, false);
}

// Validates the exit callback's argument block (pointed at from 8 bytes before its common
// argument) and marks its parameter area: what LoadExecForUser_362A956B does per PPSSPP.
void Kernel::LoadExecForUser_362A956B(Ctx &ctx) {
    auto &m = rt_.memory();
    const auto it = callbacks_.find(exit_callback_);
    if (it == callbacks_.end()) return finish(ctx, kErrorUnknownCallback);
    const std::uint32_t arg = it->second.common;
    if (!m.contains(arg - 8u, 8u)) return finish(ctx, kErrorIllegalAddr);
    if (m.load32(arg - 8u) >= 4u) return finish(ctx, kErrorIllegalArgument);
    const std::uint32_t params = m.load32(arg - 4u);
    if ((params & 3u) != 0u || !m.contains(params, 12u)) return finish(ctx, kErrorIllegalAddr);
    if (m.load32(params) < 12u) return finish(ctx, 0x800201BCu); // ILLEGAL_SIZE
    m.store32(params + 4u, 0u);
    m.store32(params + 8u, 0xFFFFFFFFu);
    finish(ctx, 0u);
}

void Kernel::sceKernelRegisterExitCallback(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    // Older SDKs accept an unknown callback.
    if (!callbacks_.contains(uid)) return finish(ctx, sdk_version_ >= 0x03090500u ? kErrorIllegalArgument : 0u);
    exit_callback_ = uid;
    finish(ctx, 0u);
}

} // namespace pspweb
