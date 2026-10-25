// SPDX-License-Identifier: GPL-2.0-or-later
//
// Semaphores, event flags, mutexes and message boxes. Waiters are served first come first
// served, or by priority with the priority attribute. A message box keeps its queue the way the
// PSP does, as a circular list through the first word of the guest's packets. Semantics and
// error codes follow PPSSPP (Core/HLE/sceKernelSemaphore.cpp, sceKernelEventFlag.cpp,
// sceKernelMutex.cpp, sceKernelMbx.cpp; GPL-2.0-or-later).

#include "kernel.hpp"
#include "kernel_errors.hpp"

#include <algorithm>

namespace pspweb {
namespace {

constexpr std::uint32_t kAttrPriority = 0x100u;
constexpr std::uint32_t kMutexRecursive = 0x200u;
constexpr std::uint32_t kEventWaitMultiple = 0x200u;
constexpr std::uint32_t kEventWaitOr = 0x01u;
constexpr std::uint32_t kEventWaitClearAll = 0x10u;
constexpr std::uint32_t kEventWaitClear = 0x20u;
constexpr std::uint32_t kEventWaitKnown = kEventWaitOr | kEventWaitClearAll | kEventWaitClear;
constexpr std::uint32_t kMbxMessagePriority = 0x400u;

bool event_matches(std::uint32_t pattern, std::uint32_t bits, std::uint32_t mode) {
    return (mode & kEventWaitOr) != 0u ? (pattern & bits) != 0u : (pattern & bits) == bits;
}

} // namespace

bool Kernel::valid_word(std::uint32_t address) const {
    return address != 0u && (address & 3u) == 0u && rt_.memory().contains(address, 4u);
}

// ---------------------------------------------------------------------------
// Semaphores

bool Kernel::try_satisfy_sema(Thread &t) {
    auto it = semaphores_.find(t.wait_uid);
    if (it == semaphores_.end() || it->second.count < static_cast<std::int32_t>(t.wait_count)) return false;
    it->second.count -= static_cast<std::int32_t>(t.wait_count);
    return true;
}

// Wakes every waiter the count now satisfies, in order, skipping those it does not.
bool Kernel::sema_wake(std::int32_t uid) {
    const Semaphore &sema = semaphores_.at(uid);
    bool woke = false;
    for (Thread *t : waiters(WaitType::Sema, uid, (sema.attr & kAttrPriority) != 0u)) {
        if (!try_satisfy_sema(*t)) continue;
        wake(*t, 0u);
        woke = true;
    }
    return woke;
}

void Kernel::sceKernelCreateSema(Ctx &ctx) {
    if (ctx.gpr[4] == 0u) return finish(ctx, err::kError);
    if (ctx.gpr[5] >= 0x200u) return finish(ctx, err::kIllegalAttr);
    const std::int32_t uid = new_uid();
    const auto init = static_cast<std::int32_t>(ctx.gpr[6]);
    semaphores_[uid] = Semaphore{read_string(ctx.gpr[4], 31), init, static_cast<std::int32_t>(ctx.gpr[7]), ctx.gpr[5], init};
    finish(ctx, static_cast<std::uint32_t>(uid));
}

void Kernel::sceKernelDeleteSema(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (!semaphores_.contains(uid)) return finish(ctx, err::kUnknownSemid);
    const auto list = waiters(WaitType::Sema, uid);
    for (Thread *t : list) wake(*t, err::kWaitDelete);
    semaphores_.erase(uid);
    finish(ctx, 0u);
    if (!list.empty()) maybe_preempt(ctx);
}

void Kernel::sceKernelCancelSema(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = semaphores_.find(uid);
    if (it == semaphores_.end()) return finish(ctx, err::kUnknownSemid);
    const auto count = static_cast<std::int32_t>(ctx.gpr[5]);
    if (count > it->second.max) return finish(ctx, err::kIllegalCount);
    const auto list = waiters(WaitType::Sema, uid);
    if (rt_.memory().contains(ctx.gpr[6], 4u)) rt_.memory().store32(ctx.gpr[6], static_cast<std::uint32_t>(list.size()));
    it->second.count = count < 0 ? it->second.init : count;
    for (Thread *t : list) wake(*t, err::kWaitCancel);
    finish(ctx, 0u);
    if (!list.empty()) maybe_preempt(ctx);
}

void Kernel::sceKernelSignalSema(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = semaphores_.find(uid);
    if (it == semaphores_.end()) return finish(ctx, err::kUnknownSemid);
    const auto signal = static_cast<std::int32_t>(ctx.gpr[5]);
    const auto waiting = static_cast<std::int64_t>(count_waiters(WaitType::Sema, uid));
    if (static_cast<std::int64_t>(it->second.count) + signal - waiting > it->second.max) return finish(ctx, err::kSemaOverflow);
    it->second.count += signal;
    const bool woke = sema_wake(uid);
    finish(ctx, 0u);
    if (woke) maybe_preempt(ctx);
}

void Kernel::sceKernelWaitSema(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto need = static_cast<std::int32_t>(ctx.gpr[5]);
    const std::uint32_t timeout = ctx.gpr[6];
    if (need <= 0) return finish(ctx, err::kIllegalCount);
    const auto it = semaphores_.find(uid);
    if (it == semaphores_.end()) return finish(ctx, err::kUnknownSemid);
    if (need > it->second.max) return finish(ctx, err::kIllegalCount);
    Thread *self = current();
    // With callbacks pending, a CB wait always waits and takes the count after them.
    const bool callbacks = cb_call_ && self != nullptr && pending_callback(*self) != 0;
    if (it->second.count >= need && count_waiters(WaitType::Sema, uid) == 0u && !callbacks) {
        it->second.count -= need;
        return finish(ctx, 0u);
    }
    if (!callbacks && timeout_at_once(timeout)) return finish(ctx, err::kWaitTimeout);
    if (self != nullptr) {
        self->wait_uid = uid;
        self->wait_count = static_cast<std::uint32_t>(need);
        begin_timeout(*self, timeout);
    }
    block(ctx, WaitType::Sema, 0u);
}

void Kernel::sceKernelPollSema(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto need = static_cast<std::int32_t>(ctx.gpr[5]);
    if (need <= 0) return finish(ctx, err::kIllegalCount);
    const auto it = semaphores_.find(uid);
    if (it == semaphores_.end()) return finish(ctx, err::kUnknownSemid);
    if (it->second.count >= need && count_waiters(WaitType::Sema, uid) == 0u) {
        it->second.count -= need;
        return finish(ctx, 0u);
    }
    finish(ctx, err::kSemaZero);
}

// SceKernelSemaInfo: size, name[32], attr, initCount, currentCount, maxCount, numWaitThreads.
void Kernel::sceKernelReferSemaStatus(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = semaphores_.find(uid);
    if (it == semaphores_.end()) return finish(ctx, err::kUnknownSemid);
    auto &m = rt_.memory();
    const std::uint32_t info = ctx.gpr[5];
    if (!valid_word(info)) return finish(ctx, 0xFFFFFFFFu);
    if (m.load32(info) == 0u) return finish(ctx, 0u);
    m.store32(info, 0x38u);
    write_object_name(info, it->second.name);
    m.store32(info + 0x24u, it->second.attr);
    m.store32(info + 0x28u, static_cast<std::uint32_t>(it->second.init));
    m.store32(info + 0x2Cu, static_cast<std::uint32_t>(it->second.count));
    m.store32(info + 0x30u, static_cast<std::uint32_t>(it->second.max));
    m.store32(info + 0x34u, count_waiters(WaitType::Sema, uid));
    finish(ctx, 0u);
}

// ---------------------------------------------------------------------------
// Event flags. A waiter keeps its bits in wait_count, its mode in wait_mode and where the pattern
// goes in wait_out_address.

bool Kernel::try_satisfy_event(Thread &t) {
    auto it = event_flags_.find(t.wait_uid);
    if (it == event_flags_.end()) return false;
    std::uint32_t &pattern = it->second.pattern;
    if (!event_matches(pattern, t.wait_count, t.wait_mode)) return false;
    if (valid_word(t.wait_out_address)) rt_.memory().store32(t.wait_out_address, pattern);
    if ((t.wait_mode & kEventWaitClear) != 0u) pattern &= ~t.wait_count;
    if ((t.wait_mode & kEventWaitClearAll) != 0u) pattern = 0u;
    return true;
}

// Ends the waits on an event flag with an error, handing each the current pattern.
bool Kernel::event_flag_release(std::int32_t uid, std::uint32_t result) {
    const std::uint32_t pattern = event_flags_.at(uid).pattern;
    const auto list = waiters(WaitType::EventFlag, uid);
    for (Thread *t : list) {
        if (valid_word(t->wait_out_address)) rt_.memory().store32(t->wait_out_address, pattern);
        wake(*t, result);
    }
    return !list.empty();
}

void Kernel::sceKernelCreateEventFlag(Ctx &ctx) {
    const std::uint32_t attr = ctx.gpr[5];
    if (ctx.gpr[4] == 0u) return finish(ctx, err::kError);
    if ((attr & 0x100u) != 0u || attr >= 0x300u) return finish(ctx, err::kIllegalAttr);
    const std::int32_t uid = new_uid();
    event_flags_[uid] = EventFlag{read_string(ctx.gpr[4], 31), attr, ctx.gpr[6], ctx.gpr[6]};
    finish(ctx, static_cast<std::uint32_t>(uid));
}

void Kernel::sceKernelDeleteEventFlag(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (!event_flags_.contains(uid)) return finish(ctx, err::kUnknownEvfid);
    const bool woke = event_flag_release(uid, err::kWaitDelete);
    event_flags_.erase(uid);
    finish(ctx, 0u);
    if (woke) maybe_preempt(ctx);
}

void Kernel::sceKernelCancelEventFlag(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = event_flags_.find(uid);
    if (it == event_flags_.end()) return finish(ctx, err::kUnknownEvfid);
    if (valid_word(ctx.gpr[6])) rt_.memory().store32(ctx.gpr[6], count_waiters(WaitType::EventFlag, uid));
    it->second.pattern = ctx.gpr[5];
    const bool woke = event_flag_release(uid, err::kWaitCancel);
    finish(ctx, 0u);
    if (woke) maybe_preempt(ctx);
}

void Kernel::sceKernelSetEventFlag(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = event_flags_.find(uid);
    if (it == event_flags_.end()) return finish(ctx, err::kUnknownEvfid);
    it->second.pattern |= ctx.gpr[5];
    bool woke = false;
    for (Thread *t : waiters(WaitType::EventFlag, uid)) {
        if (!try_satisfy_event(*t)) continue;
        wake(*t, 0u);
        woke = true;
    }
    finish(ctx, 0u);
    if (woke) maybe_preempt(ctx);
}

void Kernel::sceKernelClearEventFlag(Ctx &ctx) {
    const auto it = event_flags_.find(static_cast<std::int32_t>(ctx.gpr[4]));
    if (it == event_flags_.end()) return finish(ctx, err::kUnknownEvfid);
    it->second.pattern &= ctx.gpr[5];
    finish(ctx, 0u);
}

void Kernel::sceKernelWaitEventFlag(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t bits = ctx.gpr[5], mode = ctx.gpr[6], out = ctx.gpr[7], timeout = ctx.gpr[8];
    if ((mode & ~kEventWaitKnown) != 0u) return finish(ctx, err::kIllegalMode);
    if (bits == 0u) return finish(ctx, err::kEvfIllegalPattern);
    if (!dispatch_enabled_) return finish(ctx, err::kCanNotWait);
    const auto it = event_flags_.find(uid);
    if (it == event_flags_.end()) return finish(ctx, err::kUnknownEvfid);
    Thread *self = current();
    if (self == nullptr) return finish(ctx, 0u);
    self->wait_uid = uid;
    self->wait_count = bits;
    self->wait_mode = mode;
    self->wait_out_address = out;
    const bool callbacks = cb_call_ && pending_callback(*self) != 0;
    // A CB wait with callbacks pending runs them first and checks the pattern afterwards.
    if (!callbacks && try_satisfy_event(*self)) return finish(ctx, 0u);
    if (count_waiters(WaitType::EventFlag, uid) > 0u && (it->second.attr & kEventWaitMultiple) == 0u)
        return finish(ctx, err::kEvfMulti);
    if (!callbacks && timeout_at_once(timeout)) return finish(ctx, err::kWaitTimeout);
    // A zero timeout leaves the pattern unwritten.
    if (valid_word(timeout) && rt_.memory().load32(timeout) == 0u) self->wait_out_address = 0u;
    begin_timeout(*self, timeout);
    block(ctx, WaitType::EventFlag, 0u);
}

void Kernel::sceKernelPollEventFlag(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t bits = ctx.gpr[5], mode = ctx.gpr[6], out = ctx.gpr[7];
    if ((mode & ~kEventWaitKnown) != 0u) return finish(ctx, err::kIllegalMode);
    if ((mode & kEventWaitClear) != 0u && (mode & kEventWaitClearAll) != 0u) return finish(ctx, err::kIllegalMode);
    if (bits == 0u) return finish(ctx, err::kEvfIllegalPattern);
    const auto it = event_flags_.find(uid);
    if (it == event_flags_.end()) return finish(ctx, err::kUnknownEvfid);
    Thread probe;
    probe.wait_uid = uid;
    probe.wait_count = bits;
    probe.wait_mode = mode;
    probe.wait_out_address = out;
    if (try_satisfy_event(probe)) return finish(ctx, 0u);
    if (valid_word(out)) rt_.memory().store32(out, it->second.pattern);
    if (count_waiters(WaitType::EventFlag, uid) > 0u && (it->second.attr & kEventWaitMultiple) == 0u)
        return finish(ctx, err::kEvfMulti);
    finish(ctx, err::kEvfCond);
}

// SceKernelEventFlagInfo: size, name[32], attr, initPattern, currentPattern, numWaitThreads.
void Kernel::sceKernelReferEventFlagStatus(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = event_flags_.find(uid);
    if (it == event_flags_.end()) return finish(ctx, err::kUnknownEvfid);
    auto &m = rt_.memory();
    const std::uint32_t info = ctx.gpr[5];
    if (!valid_word(info)) return finish(ctx, 0xFFFFFFFFu);
    if (m.load32(info) == 0u) return finish(ctx, 0u);
    m.store32(info, 0x34u);
    write_object_name(info, it->second.name);
    m.store32(info + 0x24u, it->second.attr);
    m.store32(info + 0x28u, it->second.init);
    m.store32(info + 0x2Cu, it->second.pattern);
    m.store32(info + 0x30u, count_waiters(WaitType::EventFlag, uid));
    finish(ctx, 0u);
}

// ---------------------------------------------------------------------------
// Mutexes. A waiter keeps its count in wait_count.

std::uint32_t Kernel::mutex_lock_check(const Mutex &mutex, std::int32_t count) const {
    const bool recursive = (mutex.attr & kMutexRecursive) != 0u;
    if (count <= 0 || (count > 1 && !recursive)) return err::kIllegalCount;
    if (static_cast<std::int32_t>(static_cast<std::uint32_t>(count) + static_cast<std::uint32_t>(mutex.count)) < 0)
        return err::kMutexLockOverflow;
    if (mutex.count != 0 && mutex.owner == current_uid_) return recursive ? 0u : err::kMutexRecursiveNotAllowed;
    return mutex.count == 0 ? 0u : 1u;
}

// Hands an unlocked mutex to its first waiter; false when nobody waits.
bool Kernel::mutex_hand_over(std::int32_t uid) {
    Mutex &mutex = mutexes_.at(uid);
    const auto list = waiters(WaitType::Mutex, uid, (mutex.attr & kAttrPriority) != 0u);
    mutex.owner = 0;
    mutex.count = 0;
    if (list.empty()) return false;
    Thread &next = *list.front();
    mutex.owner = next.uid;
    mutex.count = static_cast<std::int32_t>(next.wait_count);
    wake(next, 0u);
    return true;
}

// A thread that ends gives up the mutexes it holds.
void Kernel::mutex_thread_end(std::int32_t thread_uid) {
    for (auto &[uid, mutex] : mutexes_)
        if (mutex.count > 0 && mutex.owner == thread_uid) mutex_hand_over(uid);
}

void Kernel::sceKernelCreateMutex(Ctx &ctx) {
    const std::uint32_t attr = ctx.gpr[5];
    const auto initial = static_cast<std::int32_t>(ctx.gpr[6]);
    if (ctx.gpr[4] == 0u) return finish(ctx, err::kError);
    if ((attr & ~0xBFFu) != 0u) return finish(ctx, err::kIllegalAttr);
    if (initial < 0 || ((attr & kMutexRecursive) == 0u && initial > 1)) return finish(ctx, err::kIllegalCount);
    const std::int32_t uid = new_uid();
    mutexes_[uid] = Mutex{read_string(ctx.gpr[4], 31), attr, initial, initial > 0 ? current_uid_ : 0, initial};
    finish(ctx, static_cast<std::uint32_t>(uid));
}

void Kernel::sceKernelDeleteMutex(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (!mutexes_.contains(uid)) return finish(ctx, err::kMutexNotFound);
    const auto list = waiters(WaitType::Mutex, uid);
    for (Thread *t : list) wake(*t, err::kWaitDelete);
    mutexes_.erase(uid);
    finish(ctx, 0u);
    if (!list.empty()) maybe_preempt(ctx);
}

void Kernel::sceKernelLockMutex(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto count = static_cast<std::int32_t>(ctx.gpr[5]);
    const std::uint32_t timeout = ctx.gpr[6];
    const auto it = mutexes_.find(uid);
    if (it == mutexes_.end()) return finish(ctx, err::kMutexNotFound);
    Mutex &mutex = it->second;
    const std::uint32_t check = mutex_lock_check(mutex, count);
    if (check != 0u && check != 1u) return finish(ctx, check);
    Thread *self = current();
    const bool callbacks = cb_call_ && self != nullptr && pending_callback(*self) != 0;
    if (check == 0u && !callbacks) {
        if (mutex.count == 0) mutex.owner = current_uid_;
        mutex.count += count;
        return finish(ctx, 0u);
    }
    if (check == 1u && timeout_at_once(timeout)) return finish(ctx, err::kWaitTimeout);
    if (self != nullptr) {
        self->wait_uid = uid;
        self->wait_count = static_cast<std::uint32_t>(count);
        begin_timeout(*self, timeout);
    }
    block(ctx, WaitType::Mutex, 0u);
}

void Kernel::sceKernelTryLockMutex(Ctx &ctx) {
    const auto it = mutexes_.find(static_cast<std::int32_t>(ctx.gpr[4]));
    if (it == mutexes_.end()) return finish(ctx, err::kMutexNotFound);
    const auto count = static_cast<std::int32_t>(ctx.gpr[5]);
    const std::uint32_t check = mutex_lock_check(it->second, count);
    if (check == 1u) return finish(ctx, err::kMutexLocked);
    if (check != 0u) return finish(ctx, check);
    if (it->second.count == 0) it->second.owner = current_uid_;
    it->second.count += count;
    finish(ctx, 0u);
}

void Kernel::sceKernelUnlockMutex(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = mutexes_.find(uid);
    if (it == mutexes_.end()) return finish(ctx, err::kMutexNotFound);
    Mutex &mutex = it->second;
    const auto count = static_cast<std::int32_t>(ctx.gpr[5]);
    if (count <= 0 || ((mutex.attr & kMutexRecursive) == 0u && count > 1)) return finish(ctx, err::kIllegalCount);
    if (mutex.count == 0 || mutex.owner != current_uid_) return finish(ctx, err::kMutexUnlocked);
    if (mutex.count < count) return finish(ctx, err::kMutexUnlockUnderflow);
    mutex.count -= count;
    finish(ctx, 0u);
    if (mutex.count == 0 && mutex_hand_over(uid)) maybe_preempt(ctx);
}

void Kernel::sceKernelCancelMutex(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = mutexes_.find(uid);
    if (it == mutexes_.end()) return finish(ctx, err::kMutexNotFound);
    Mutex &mutex = it->second;
    const auto count = static_cast<std::int32_t>(ctx.gpr[5]);
    if (count > 0) {
        const std::uint32_t check = mutex_lock_check(mutex, count);
        if (check != 0u && check != 1u && check != err::kMutexLockOverflow && check != err::kMutexRecursiveNotAllowed)
            return finish(ctx, check);
    }
    const auto list = waiters(WaitType::Mutex, uid);
    if (valid_word(ctx.gpr[6])) rt_.memory().store32(ctx.gpr[6], static_cast<std::uint32_t>(list.size()));
    for (Thread *t : list) wake(*t, err::kWaitCancel);
    if (count <= 0) {
        mutex.count = 0;
        mutex.owner = 0;
    } else {
        mutex.count = count;
        mutex.owner = current_uid_;
    }
    finish(ctx, 0u);
    if (!list.empty()) maybe_preempt(ctx);
}

// SceKernelMutexInfo: size, name[32], attr, initCount, currentCount, currentOwner, numWaitThreads.
void Kernel::sceKernelReferMutexStatus(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = mutexes_.find(uid);
    if (it == mutexes_.end()) return finish(ctx, err::kMutexNotFound);
    auto &m = rt_.memory();
    const std::uint32_t info = ctx.gpr[5];
    if (!valid_word(info)) return finish(ctx, 0xFFFFFFFFu);
    if (m.load32(info) == 0u) return finish(ctx, 0u);
    m.store32(info, 0x38u);
    write_object_name(info, it->second.name);
    m.store32(info + 0x24u, it->second.attr);
    m.store32(info + 0x28u, static_cast<std::uint32_t>(it->second.init));
    m.store32(info + 0x2Cu, static_cast<std::uint32_t>(it->second.count));
    m.store32(info + 0x30u, it->second.count > 0 ? static_cast<std::uint32_t>(it->second.owner) : 0xFFFFFFFFu);
    m.store32(info + 0x34u, count_waiters(WaitType::Mutex, uid));
    finish(ctx, 0u);
}

// ---------------------------------------------------------------------------
// Message boxes. Packets start with the next pointer and then a priority byte; the box points at
// the last packet, whose next pointer is the first one.

// Takes the first message into `out`; 0 or an error for a damaged list.
std::uint32_t Kernel::mbx_receive(Mailbox &box, std::uint32_t out) {
    auto &m = rt_.memory();
    const std::uint32_t first = box.head;
    if (!valid_word(box.head)) return err::kIllegalAddr;
    std::int32_t steps = 0;
    while (true) {
        std::uint32_t next = m.load32(box.head);
        if (!valid_word(next)) return err::kIllegalAddr;
        if (next == first) {
            if (box.head != first) {
                next = m.load32(next);
                m.store32(box.head, next);
                box.head = next;
                break;
            }
            if (steps < box.count - 1) return err::kMessageboxDuplicateMessage;
            box.head = 0u;
            break;
        }
        box.head = next;
        if (++steps > box.count) return err::kIllegalAddr;
    }
    m.store32(out, first);
    --box.count;
    return 0u;
}

void Kernel::sceKernelCreateMbx(Ctx &ctx) {
    const std::uint32_t attr = ctx.gpr[5];
    if (ctx.gpr[4] == 0u) return finish(ctx, err::kError);
    if (((attr & ~(kAttrPriority | kMbxMessagePriority)) & ~0xFFu) != 0u) return finish(ctx, err::kIllegalAttr);
    const std::int32_t uid = new_uid();
    mailboxes_[uid] = Mailbox{read_string(ctx.gpr[4], 31), attr, 0u, 0};
    finish(ctx, static_cast<std::uint32_t>(uid));
}

void Kernel::sceKernelDeleteMbx(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (!mailboxes_.contains(uid)) return finish(ctx, err::kUnknownMbxid);
    const auto list = waiters(WaitType::Mbx, uid);
    for (Thread *t : list) wake(*t, err::kWaitDelete);
    mailboxes_.erase(uid);
    finish(ctx, 0u);
    if (!list.empty()) maybe_preempt(ctx);
}

void Kernel::sceKernelSendMbx(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t packet = ctx.gpr[5];
    const auto it = mailboxes_.find(uid);
    if (it == mailboxes_.end()) return finish(ctx, err::kUnknownMbxid);
    Mailbox &box = it->second;
    auto &m = rt_.memory();
    if (!m.contains(packet, 8u)) return finish(ctx, 0xFFFFFFFFu);
    if (box.count == 0) {
        const auto list = waiters(WaitType::Mbx, uid, (box.attr & kAttrPriority) != 0u);
        if (!list.empty()) {
            Thread &t = *list.front();
            m.store32(t.wait_out_address, packet);
            wake(t, 0u);
            finish(ctx, 0u);
            maybe_preempt(ctx);
            return;
        }
        ++box.count;
        m.store32(packet, packet);
        box.head = packet;
        return finish(ctx, 0u);
    }
    std::uint32_t next = box.head, prev = 0u;
    for (std::int32_t i = 0; i < box.count; ++i) {
        if (next == packet) return finish(ctx, err::kMessageboxDuplicateMessage);
        if (!valid_word(next)) return finish(ctx, err::kIllegalAddr);
        prev = next;
        next = m.load32(next);
    }
    // prev is the last packet now, and next the first (the head).
    if ((box.attr & kMbxMessagePriority) != 0u) {
        const std::uint8_t priority = m.load8(packet + 4u);
        for (std::int32_t i = 0; i < box.count; ++i) {
            if (priority < m.load8(next + 4u)) {
                m.store32(packet, next);
                m.store32(prev, packet);
                if (i == 0) box.head = packet;
                ++box.count;
                return finish(ctx, 0u);
            }
            prev = next;
            next = m.load32(next);
        }
    }
    ++box.count;
    m.store32(prev, packet);
    m.store32(packet, box.head);
    finish(ctx, 0u);
}

void Kernel::sceKernelReceiveMbx(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t out = ctx.gpr[5], timeout = ctx.gpr[6];
    const auto it = mailboxes_.find(uid);
    if (it == mailboxes_.end()) return finish(ctx, err::kUnknownMbxid);
    if (it->second.count > 0) return finish(ctx, mbx_receive(it->second, out));
    if (timeout_at_once(timeout)) return finish(ctx, err::kWaitTimeout);
    Thread *self = current();
    if (self != nullptr) {
        self->wait_uid = uid;
        self->wait_out_address = out;
        begin_timeout(*self, timeout);
    }
    block(ctx, WaitType::Mbx, 0u);
}

void Kernel::sceKernelPollMbx(Ctx &ctx) {
    const auto it = mailboxes_.find(static_cast<std::int32_t>(ctx.gpr[4]));
    if (it == mailboxes_.end()) return finish(ctx, err::kUnknownMbxid);
    if (it->second.count == 0) return finish(ctx, err::kMboxNoMessage);
    finish(ctx, mbx_receive(it->second, ctx.gpr[5]));
}

void Kernel::sceKernelCancelReceiveMbx(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (!mailboxes_.contains(uid)) return finish(ctx, err::kUnknownMbxid);
    const auto list = waiters(WaitType::Mbx, uid);
    for (Thread *t : list) wake(*t, err::kWaitCancel);
    if (ctx.gpr[5] != 0u && rt_.memory().contains(ctx.gpr[5], 4u))
        rt_.memory().store32(ctx.gpr[5], static_cast<std::uint32_t>(list.size()));
    finish(ctx, 0u);
    if (!list.empty()) maybe_preempt(ctx);
}

// SceKernelMbxInfo: size, name[32], attr, numWaitThreads, numMessages, firstMessage. Like the
// PSP, this walks the packet list and repairs the head from what the game did to the packets.
void Kernel::sceKernelReferMbxStatus(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = mailboxes_.find(uid);
    if (it == mailboxes_.end()) return finish(ctx, err::kUnknownMbxid);
    Mailbox &box = it->second;
    auto &m = rt_.memory();
    const std::uint32_t info = ctx.gpr[5];
    if (!valid_word(info)) return finish(ctx, 0xFFFFFFFFu);
    const std::uint32_t head = box.head;
    std::uint32_t packet = head;
    for (std::int32_t i = 0; i < box.count; ++i) {
        if (packet == 0u || !m.contains(packet, 4u)) {
            box.head = 0u;
            packet = 0u;
            break;
        }
        const std::uint32_t next = m.load32(packet);
        if (next == 0u) {
            box.head = 0u;
            packet = 0u;
            break;
        }
        if (next == packet) {
            if (box.count > 1) box.head = packet;
            break;
        }
        packet = next;
    }
    if (packet != 0u && packet != head) {
        for (std::int32_t i = 0; i < box.count; ++i) {
            if (packet == 0u || !m.contains(packet, 4u)) break;
            const std::uint32_t next = m.load32(packet);
            if (next == head) {
                box.head = packet;
                break;
            }
            if (next == 0u || next == packet) break;
            packet = next;
        }
    }
    if (m.load32(info) == 0u) return finish(ctx, 0u);
    m.store32(info, 0x34u);
    write_object_name(info, box.name);
    m.store32(info + 0x24u, box.attr);
    m.store32(info + 0x28u, count_waiters(WaitType::Mbx, uid));
    m.store32(info + 0x2Cu, static_cast<std::uint32_t>(box.count));
    m.store32(info + 0x30u, box.head);
    finish(ctx, 0u);
}

void Kernel::install_sync() {
    const char *tm = "ThreadManForUser";
    hle(tm, 0xD6DA4BA1u, "sceKernelCreateSema", &Kernel::sceKernelCreateSema);
    hle(tm, 0x28B6489Cu, "sceKernelDeleteSema", &Kernel::sceKernelDeleteSema);
    hle(tm, 0x3F53E640u, "sceKernelSignalSema", &Kernel::sceKernelSignalSema);
    hle(tm, 0x4E3A1105u, "sceKernelWaitSema", &Kernel::sceKernelWaitSema);
    hle_cb(tm, 0x6D212BACu, "sceKernelWaitSemaCB", &Kernel::sceKernelWaitSema);
    hle(tm, 0x58B1F937u, "sceKernelPollSema", &Kernel::sceKernelPollSema);
    hle(tm, 0x8FFDF9A2u, "sceKernelCancelSema", &Kernel::sceKernelCancelSema);
    hle(tm, 0xBC6FEBC5u, "sceKernelReferSemaStatus", &Kernel::sceKernelReferSemaStatus);
    hle(tm, 0x55C20A00u, "sceKernelCreateEventFlag", &Kernel::sceKernelCreateEventFlag);
    hle(tm, 0xEF9E4C70u, "sceKernelDeleteEventFlag", &Kernel::sceKernelDeleteEventFlag);
    hle(tm, 0x1FB15A32u, "sceKernelSetEventFlag", &Kernel::sceKernelSetEventFlag);
    hle(tm, 0x812346E4u, "sceKernelClearEventFlag", &Kernel::sceKernelClearEventFlag);
    hle(tm, 0x402FCF22u, "sceKernelWaitEventFlag", &Kernel::sceKernelWaitEventFlag);
    hle_cb(tm, 0x328C546Au, "sceKernelWaitEventFlagCB", &Kernel::sceKernelWaitEventFlag);
    hle(tm, 0x30FD48F0u, "sceKernelPollEventFlag", &Kernel::sceKernelPollEventFlag);
    hle(tm, 0xCD203292u, "sceKernelCancelEventFlag", &Kernel::sceKernelCancelEventFlag);
    hle(tm, 0xA66B0120u, "sceKernelReferEventFlagStatus", &Kernel::sceKernelReferEventFlagStatus);
    hle(tm, 0xB7D098C6u, "sceKernelCreateMutex", &Kernel::sceKernelCreateMutex);
    hle(tm, 0xF8170FBEu, "sceKernelDeleteMutex", &Kernel::sceKernelDeleteMutex);
    hle(tm, 0xB011B11Fu, "sceKernelLockMutex", &Kernel::sceKernelLockMutex);
    hle_cb(tm, 0x5BF4DD27u, "sceKernelLockMutexCB", &Kernel::sceKernelLockMutex);
    hle(tm, 0x0DDCD2C9u, "sceKernelTryLockMutex", &Kernel::sceKernelTryLockMutex);
    hle(tm, 0x6B30100Fu, "sceKernelUnlockMutex", &Kernel::sceKernelUnlockMutex);
    hle(tm, 0x87D9223Cu, "sceKernelCancelMutex", &Kernel::sceKernelCancelMutex);
    hle(tm, 0xA9C2CB9Au, "sceKernelReferMutexStatus", &Kernel::sceKernelReferMutexStatus);
    hle(tm, 0x8125221Du, "sceKernelCreateMbx", &Kernel::sceKernelCreateMbx);
    hle(tm, 0x86255ADAu, "sceKernelDeleteMbx", &Kernel::sceKernelDeleteMbx);
    hle(tm, 0xE9B3061Eu, "sceKernelSendMbx", &Kernel::sceKernelSendMbx);
    hle(tm, 0x18260574u, "sceKernelReceiveMbx", &Kernel::sceKernelReceiveMbx);
    hle_cb(tm, 0xF3986382u, "sceKernelReceiveMbxCB", &Kernel::sceKernelReceiveMbx);
    hle(tm, 0x0D81716Au, "sceKernelPollMbx", &Kernel::sceKernelPollMbx);
    hle(tm, 0x87D4DD36u, "sceKernelCancelReceiveMbx", &Kernel::sceKernelCancelReceiveMbx);
    hle(tm, 0xA8E8C846u, "sceKernelReferMbxStatus", &Kernel::sceKernelReferMbxStatus);
}

} // namespace pspweb
