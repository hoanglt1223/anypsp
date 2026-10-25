// SPDX-License-Identifier: GPL-2.0-or-later
//
// Lightweight mutexes (sceKernel*LwMutex). The lock level and owner live in a
// workarea in guest memory, which games read directly; the kernel object only
// keeps the waiters. Semantics and error codes follow PPSSPP
// (Core/HLE/sceKernelMutex.cpp, GPL-2.0-or-later).

#include "kernel.hpp"

namespace pspweb {
namespace {

// NativeLwMutexWorkarea: lockLevel, lockThread, attr, numWaitThreads, uid, pad[3].
constexpr std::uint32_t kLockLevel = 0u, kLockThread = 4u, kAttr = 8u, kNumWaitThreads = 12u, kUid = 16u;
constexpr std::uint32_t kWorkareaSize = 32u;

constexpr std::uint32_t kAttrPriority = 0x100u;
constexpr std::uint32_t kAttrRecursive = 0x200u;

constexpr std::uint32_t kErrorError = 0x80020001u;
constexpr std::uint32_t kErrorIllegalAddr = 0x800200D3u;
constexpr std::uint32_t kErrorIllegalAttr = 0x80020191u;
constexpr std::uint32_t kErrorAccessError = 0x8002013Cu;
constexpr std::uint32_t kErrorIllegalCount = 0x800201BDu;
constexpr std::uint32_t kErrorWaitTimeout = 0x800201A8u;
constexpr std::uint32_t kErrorWaitDelete = 0x800201B5u;
constexpr std::uint32_t kErrorMutexTrylockFailed = 0x800201C4u;
constexpr std::uint32_t kErrorNoSuchLwMutex = 0x800201CAu;
constexpr std::uint32_t kErrorLwMutexTrylockFailed = 0x800201CBu;
constexpr std::uint32_t kErrorLwMutexNotLocked = 0x800201CCu;
constexpr std::uint32_t kErrorLwMutexLockOverflow = 0x800201CDu;
constexpr std::uint32_t kErrorLwMutexUnlockUnderflow = 0x800201CEu;
constexpr std::uint32_t kErrorLwMutexAlreadyLocked = 0x800201CFu;
constexpr std::uint32_t kLockPending = 1u;

} // namespace

void Kernel::install_lwmutex() {
    const char *tm = "ThreadManForUser";
    const char *kl = "Kernel_Library";
    hle(tm, 0x19CFF145u, "sceKernelCreateLwMutex", &Kernel::sceKernelCreateLwMutex);
    hle(tm, 0x60107536u, "sceKernelDeleteLwMutex", &Kernel::sceKernelDeleteLwMutex);
    hle(tm, 0x4C145944u, "sceKernelReferLwMutexStatusByID", &Kernel::sceKernelReferLwMutexStatusByID);
    hle(kl, 0xBEA46419u, "sceKernelLockLwMutex", &Kernel::sceKernelLockLwMutex);
    hle_cb(kl, 0x1FC64E09u, "sceKernelLockLwMutexCB", &Kernel::sceKernelLockLwMutex);
    hle(kl, 0xDC692EE3u, "sceKernelTryLockLwMutex", &Kernel::sceKernelTryLockLwMutex);
    hle(kl, 0x37431849u, "sceKernelTryLockLwMutex_600", &Kernel::sceKernelTryLockLwMutex600);
    hle(kl, 0x15B6446Bu, "sceKernelUnlockLwMutex", &Kernel::sceKernelUnlockLwMutex);
    hle(kl, 0xC1734599u, "sceKernelReferLwMutexStatus", &Kernel::sceKernelReferLwMutexStatus);
}

void Kernel::sceKernelCreateLwMutex(Ctx &ctx) {
    const std::uint32_t workarea = ctx.gpr[4], name = ctx.gpr[5], attr = ctx.gpr[6];
    const auto initial = static_cast<std::int32_t>(ctx.gpr[7]);
    auto &memory = rt_.memory();
    if (name == 0u) return finish(ctx, kErrorError);
    if (attr >= 0x400u) return finish(ctx, kErrorIllegalAttr);
    if (initial < 0 || ((attr & kAttrRecursive) == 0u && initial > 1)) return finish(ctx, kErrorIllegalCount);
    if (!memory.contains(workarea, kWorkareaSize)) return finish(ctx, kErrorIllegalAddr);
    const std::int32_t uid = new_uid();
    lwmutexes_[uid] = LwMutex{read_string(name, 32), attr, workarea, initial};
    memory.zero(workarea, kWorkareaSize);
    memory.store32(workarea + kLockLevel, static_cast<std::uint32_t>(initial));
    memory.store32(workarea + kLockThread, initial == 0 ? 0u : static_cast<std::uint32_t>(current_uid_));
    memory.store32(workarea + kAttr, attr);
    memory.store32(workarea + kUid, static_cast<std::uint32_t>(uid));
    finish(ctx, 0u);
}

void Kernel::sceKernelDeleteLwMutex(Ctx &ctx) {
    const std::uint32_t workarea = ctx.gpr[4];
    auto &memory = rt_.memory();
    if (workarea == 0u || !memory.contains(workarea, kWorkareaSize)) return finish(ctx, kErrorIllegalAddr);
    const auto uid = static_cast<std::int32_t>(memory.load32(workarea + kUid));
    if (lwmutexes_.erase(uid) == 0u) return finish(ctx, kErrorNoSuchLwMutex);
    bool woke = false;
    for (auto &[tid, t] : threads_) {
        (void)tid;
        if (t.state == ThreadState::Waiting && t.wait == WaitType::LwMutex && t.wait_uid == uid) {
            wake(t, kErrorWaitDelete);
            woke = true;
        }
    }
    memory.zero(workarea, kWorkareaSize);
    finish(ctx, 0u);
    if (woke) maybe_preempt(ctx);
}

// 0 when locked, kLockPending when it has to wait, otherwise an error code.
std::uint32_t Kernel::lock_lwmutex(std::uint32_t workarea, std::int32_t count) {
    auto &memory = rt_.memory();
    const auto level = static_cast<std::int32_t>(memory.load32(workarea + kLockLevel));
    const std::uint32_t attr = memory.load32(workarea + kAttr);
    const auto uid = static_cast<std::int32_t>(memory.load32(workarea + kUid));
    if (count <= 0 || (count > 1 && (attr & kAttrRecursive) == 0u)) return kErrorIllegalCount;
    if (static_cast<std::int32_t>(static_cast<std::uint32_t>(count) + static_cast<std::uint32_t>(level)) < 0)
        return kErrorLwMutexLockOverflow;
    if (uid == -1) return kErrorNoSuchLwMutex;
    if (level == 0) {
        if (memory.load32(workarea + kLockThread) != 0u && !lwmutexes_.contains(uid)) return kErrorNoSuchLwMutex;
        memory.store32(workarea + kLockLevel, static_cast<std::uint32_t>(count));
        memory.store32(workarea + kLockThread, static_cast<std::uint32_t>(current_uid_));
        return 0u;
    }
    if (static_cast<std::int32_t>(memory.load32(workarea + kLockThread)) == current_uid_) {
        if ((attr & kAttrRecursive) == 0u) return kErrorLwMutexAlreadyLocked;
        memory.store32(workarea + kLockLevel, static_cast<std::uint32_t>(level + count));
        return 0u;
    }
    return kLockPending;
}

void Kernel::sceKernelLockLwMutex(Ctx &ctx) {
    const std::uint32_t workarea = ctx.gpr[4], timeout = ctx.gpr[6];
    const auto count = static_cast<std::int32_t>(ctx.gpr[5]);
    auto &memory = rt_.memory();
    if (!memory.contains(workarea, kWorkareaSize)) return finish(ctx, kErrorAccessError);
    const std::uint32_t result = lock_lwmutex(workarea, count);
    if (result != kLockPending) return finish(ctx, result);
    const auto uid = static_cast<std::int32_t>(memory.load32(workarea + kUid));
    if (!lwmutexes_.contains(uid)) return finish(ctx, kErrorNoSuchLwMutex);
    if (timeout != 0u && memory.contains(timeout, 4u) && memory.load32(timeout) == 0u)
        return finish(ctx, kErrorWaitTimeout);
    Thread *self = current();
    if (self != nullptr) {
        self->wait_uid = uid;
        self->wait_count = static_cast<std::uint32_t>(count);
        begin_timeout(*self, timeout);
    }
    block(ctx, WaitType::LwMutex, 0u);
}

void Kernel::sceKernelTryLockLwMutex(Ctx &ctx) {
    if (!rt_.memory().contains(ctx.gpr[4], kWorkareaSize)) return finish(ctx, kErrorAccessError);
    const std::uint32_t result = lock_lwmutex(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]));
    finish(ctx, result == 0u ? 0u : kErrorMutexTrylockFailed);
}

void Kernel::sceKernelTryLockLwMutex600(Ctx &ctx) {
    if (!rt_.memory().contains(ctx.gpr[4], kWorkareaSize)) return finish(ctx, kErrorAccessError);
    const std::uint32_t result = lock_lwmutex(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]));
    finish(ctx, result == kLockPending ? kErrorLwMutexTrylockFailed : result);
}

void Kernel::sceKernelUnlockLwMutex(Ctx &ctx) {
    const std::uint32_t workarea = ctx.gpr[4];
    const auto count = static_cast<std::int32_t>(ctx.gpr[5]);
    auto &memory = rt_.memory();
    if (!memory.contains(workarea, kWorkareaSize)) return finish(ctx, kErrorAccessError);
    const auto uid = static_cast<std::int32_t>(memory.load32(workarea + kUid));
    const std::uint32_t attr = memory.load32(workarea + kAttr);
    const auto level = static_cast<std::int32_t>(memory.load32(workarea + kLockLevel));
    if (uid == -1) return finish(ctx, kErrorNoSuchLwMutex);
    if (count <= 0 || ((attr & kAttrRecursive) == 0u && count > 1)) return finish(ctx, kErrorIllegalCount);
    if (level == 0 || static_cast<std::int32_t>(memory.load32(workarea + kLockThread)) != current_uid_)
        return finish(ctx, kErrorLwMutexNotLocked);
    if (level < count) return finish(ctx, kErrorLwMutexUnlockUnderflow);
    memory.store32(workarea + kLockLevel, static_cast<std::uint32_t>(level - count));
    if (level - count != 0) return finish(ctx, 0u);

    // Hand the lock to the first waiter (by priority with the priority attribute).
    Thread *next = nullptr;
    const auto found = lwmutexes_.find(uid);
    if (found != lwmutexes_.end()) {
        for (auto &[tid, t] : threads_) {
            (void)tid;
            if (t.state != ThreadState::Waiting || t.wait != WaitType::LwMutex || t.wait_uid != uid) continue;
            if (next == nullptr || ((found->second.attr & kAttrPriority) != 0u && t.priority < next->priority)) next = &t;
        }
    }
    if (next == nullptr) {
        memory.store32(workarea + kLockThread, 0u);
        return finish(ctx, 0u);
    }
    memory.store32(workarea + kLockLevel, next->wait_count);
    memory.store32(workarea + kLockThread, static_cast<std::uint32_t>(next->uid));
    wake(*next, 0u);
    finish(ctx, 0u);
    maybe_preempt(ctx);
}

// SceKernelLwMutexInfo: size, name[32], attr, uid, workarea, initCount, currentCount, lockThread, numWaitThreads.
void Kernel::refer_lwmutex(Ctx &ctx, std::int32_t uid, std::uint32_t info) {
    const auto found = lwmutexes_.find(uid);
    if (found == lwmutexes_.end()) return finish(ctx, kErrorNoSuchLwMutex);
    auto &memory = rt_.memory();
    if (info == 0u || !memory.contains(info, 4u)) return finish(ctx, 0u);
    const std::uint32_t size = memory.load32(info);
    if (size == 0u) return finish(ctx, 0u);
    const LwMutex &m = found->second;
    std::uint32_t waiting = 0u;
    for (const auto &[tid, t] : threads_) {
        (void)tid;
        if (t.state == ThreadState::Waiting && t.wait == WaitType::LwMutex && t.wait_uid == uid) ++waiting;
    }
    std::uint8_t record[68]{};
    const auto put32 = [&record](std::size_t at, std::uint32_t value) {
        for (std::size_t i = 0; i < 4u; ++i) record[at + i] = static_cast<std::uint8_t>(value >> (8u * i));
    };
    put32(0, 68u);
    for (std::size_t i = 0; i < 31u && i < m.name.size(); ++i) record[4 + i] = static_cast<std::uint8_t>(m.name[i]);
    put32(36, m.attr);
    put32(40, static_cast<std::uint32_t>(uid));
    put32(44, m.workarea);
    put32(48, static_cast<std::uint32_t>(m.initial_count));
    put32(52, memory.load32(m.workarea + kLockLevel));
    put32(56, memory.load32(m.workarea + kLockThread));
    put32(60, waiting);
    memory.store32(m.workarea + kNumWaitThreads, waiting);
    const std::uint32_t length = size < 68u ? size : 68u;
    memory.copy_in(info, std::span<const std::uint8_t>(record, length));
    finish(ctx, 0u);
}

void Kernel::sceKernelReferLwMutexStatus(Ctx &ctx) {
    if (!rt_.memory().contains(ctx.gpr[4], kWorkareaSize)) return finish(ctx, kErrorAccessError);
    refer_lwmutex(ctx, static_cast<std::int32_t>(rt_.memory().load32(ctx.gpr[4] + kUid)), ctx.gpr[5]);
}

void Kernel::sceKernelReferLwMutexStatusByID(Ctx &ctx) {
    refer_lwmutex(ctx, static_cast<std::int32_t>(ctx.gpr[4]), ctx.gpr[5]);
}

} // namespace pspweb
