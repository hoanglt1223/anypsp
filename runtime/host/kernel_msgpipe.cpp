// SPDX-License-Identifier: GPL-2.0-or-later
//
// Message pipes (sceKernel*MsgPipe): byte streams between threads, through a buffer in the user
// partition or, for a pipe created without one, straight from a sending thread to a receiving
// one. A FULL transfer waits until all of it went through; an ASAP one until some did. The data
// stays at the start of the buffer and moves down as it is read, as on the PSP. Semantics and
// error codes follow PPSSPP (Core/HLE/sceKernelMsgPipe.cpp, GPL-2.0-or-later).
//
// A waiting thread keeps its transfer in its wait fields: wait_out_address is its buffer,
// wait_extra the bytes it asked for when it started waiting, wait_count the bytes still to go,
// wait_mode FULL (0) or ASAP (1) and wait_result_address where the byte count goes.

#include "kernel.hpp"
#include "kernel_errors.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace pspweb {
namespace {

constexpr std::uint32_t kAttrPrioritySend = 0x0100u;
constexpr std::uint32_t kAttrPriorityReceive = 0x1000u;
constexpr std::uint32_t kAttrHighMem = 0x4000u;
constexpr std::uint32_t kAttrKnown = kAttrPrioritySend | kAttrPriorityReceive | kAttrHighMem;
constexpr std::uint32_t kWaitFull = 0u;
constexpr std::uint32_t kWaitAsap = 1u;

} // namespace

std::uint32_t Kernel::partition_error(std::int32_t partition) const {
    if (partition < 1 || partition > 6) return err::kIllegalArgument;
    // User code may use the user partition (2, and its alias 6) and the volatile one (5).
    if (partition != 2 && partition != 5 && partition != 6) return err::kIllegalPerm;
    return 0u;
}

void Kernel::guest_copy(std::uint32_t dst, std::uint32_t src, std::uint32_t length) {
    if (length == 0u) return;
    auto &m = rt_.memory();
    m.note_raw_access(src, length, false);
    m.note_raw_access(dst, length, true);
    std::uint8_t *to = m.raw_pointer(dst, length);
    const std::uint8_t *from = m.raw_pointer(src, length);
    if (to != nullptr && from != nullptr) {
        std::memmove(to, from, length);
        return;
    }
    std::vector<std::uint8_t> bytes(length);
    for (std::uint32_t i = 0; i < length; ++i) bytes[i] = m.load8(src + i);
    for (std::uint32_t i = 0; i < length; ++i) m.store8(dst + i, bytes[i]);
}

// A waiting thread's side of a transfer: `length` bytes at its next position go to (`to_pipe`
// false: from) `address`, and its byte count grows.
void Kernel::msgpipe_waiter_transfer(Thread &t, std::uint32_t address, std::uint32_t length, bool from_waiter) {
    const std::uint32_t position = t.wait_out_address + (t.wait_extra - t.wait_count);
    if (from_waiter) guest_copy(address, position, length);
    else guest_copy(position, address, length);
    t.wait_count -= length;
    auto &m = rt_.memory();
    if (t.wait_result_address != 0u && (t.wait_result_address & 3u) == 0u && m.contains(t.wait_result_address, 4u))
        m.store32(t.wait_result_address, m.load32(t.wait_result_address) + length);
}

std::vector<Thread *> Kernel::msgpipe_waiters(std::int32_t uid, bool send) {
    const MsgPipe &pipe = msgpipes_.at(uid);
    return send ? waiters(WaitType::MsgPipeSend, uid, (pipe.attr & kAttrPrioritySend) != 0u)
                : waiters(WaitType::MsgPipeReceive, uid, (pipe.attr & kAttrPriorityReceive) != 0u);
}

// Waiting senders fill the freed space, in order, stopping at the first that does not fit whole.
bool Kernel::msgpipe_check_send(std::int32_t uid) {
    MsgPipe &pipe = msgpipes_.at(uid);
    bool woke = false, filled = false;
    for (Thread *t : msgpipe_waiters(uid, true)) {
        const std::uint32_t free = pipe.size - pipe.used;
        if (free == 0u) break;
        const std::uint32_t bytes = std::min(t->wait_count, free);
        msgpipe_waiter_transfer(*t, pipe.buffer + pipe.used, bytes, true);
        pipe.used += bytes;
        filled = true;
        if (t->wait_mode == kWaitAsap || t->wait_count == 0u) {
            wake(*t, 0u);
            woke = true;
        } else {
            break;
        }
    }
    if (filled) woke |= msgpipe_check_receive(uid);
    return woke;
}

// Waiting receivers take what is buffered, as much as each wants, stopping at the first that
// still needs more.
bool Kernel::msgpipe_check_receive(std::int32_t uid) {
    MsgPipe &pipe = msgpipes_.at(uid);
    bool woke = false, freed = false;
    for (Thread *t : msgpipe_waiters(uid, false)) {
        if (pipe.used == 0u) break;
        const std::uint32_t bytes = std::min(t->wait_count, pipe.used);
        msgpipe_waiter_transfer(*t, pipe.buffer, bytes, false);
        pipe.used -= bytes;
        guest_copy(pipe.buffer, pipe.buffer + bytes, pipe.used);
        freed = true;
        if (t->wait_mode == kWaitAsap || t->wait_count == 0u) {
            wake(*t, 0u);
            woke = true;
        } else {
            break;
        }
    }
    if (freed) woke |= msgpipe_check_send(uid);
    return woke;
}

// One send. Returns the result; `wait` is set when the caller has to wait for the rest, which
// starts at `address` + what went through.
std::uint32_t Kernel::msgpipe_send(std::int32_t uid, std::uint32_t address, std::uint32_t size, std::uint32_t mode,
                                   std::uint32_t result_address, bool poll, bool &woke, bool &wait,
                                   std::uint32_t &sent) {
    MsgPipe &pipe = msgpipes_.at(uid);
    auto &m = rt_.memory();
    std::uint32_t cursor = address;
    const bool result_valid = result_address != 0u && (result_address & 3u) == 0u && m.contains(result_address, 4u);
    if (pipe.size == 0u) {
        for (Thread *t : msgpipe_waiters(uid, false)) {
            if (size == 0u) break;
            const std::uint32_t bytes = std::min(t->wait_count, size);
            if (bytes == 0u) continue;
            msgpipe_waiter_transfer(*t, cursor, bytes, false);
            size -= bytes;
            cursor += bytes;
            if (t->wait_count == 0u || t->wait_mode == kWaitAsap) {
                wake(*t, 0u);
                woke = true;
            }
        }
        if (size != 0u && (mode != kWaitAsap || cursor == address)) {
            sent = cursor - address;
            if (poll) {
                if (result_valid && mode == kWaitAsap) m.store32(result_address, cursor - address);
                return err::kMppFull;
            }
            wait = true;
            return 0u;
        }
    } else {
        if (size > pipe.size) return err::kIllegalSize;
        std::uint32_t bytes = 0u;
        // With senders already waiting, a new one gets in line, space or not.
        if (msgpipe_waiters(uid, true).empty()) {
            if (size <= pipe.size - pipe.used) bytes = size;
            else if (mode == kWaitAsap) bytes = pipe.size - pipe.used;
        }
        if (bytes != 0u) {
            guest_copy(pipe.buffer + pipe.used, cursor, bytes);
            pipe.used += bytes;
            cursor += bytes;
            size -= bytes;
            woke |= msgpipe_check_receive(uid);
        } else if (size != 0u) {
            sent = 0u;
            if (poll) return err::kMppFull;
            wait = true;
            return 0u;
        }
    }
    sent = cursor - address;
    if (result_valid) m.store32(result_address, cursor - address);
    return 0u;
}

std::uint32_t Kernel::msgpipe_receive(std::int32_t uid, std::uint32_t address, std::uint32_t size, std::uint32_t mode,
                                      std::uint32_t result_address, bool poll, bool &woke, bool &wait,
                                      std::uint32_t &received) {
    MsgPipe &pipe = msgpipes_.at(uid);
    auto &m = rt_.memory();
    std::uint32_t cursor = address;
    const bool result_valid = result_address != 0u && (result_address & 3u) == 0u && m.contains(result_address, 4u);
    if (pipe.size == 0u) {
        for (Thread *t : msgpipe_waiters(uid, true)) {
            if (size == 0u) break;
            const std::uint32_t bytes = std::min(t->wait_count, size);
            if (bytes == 0u) continue;
            msgpipe_waiter_transfer(*t, cursor, bytes, true);
            size -= bytes;
            cursor += bytes;
            if (t->wait_count == 0u || t->wait_mode == kWaitAsap) {
                wake(*t, 0u);
                woke = true;
            }
        }
        if (size != 0u && (mode != kWaitAsap || cursor == address)) {
            received = cursor - address;
            if (poll) {
                if (result_valid && mode == kWaitAsap) m.store32(result_address, cursor - address);
                return err::kMppEmpty;
            }
            wait = true;
            return 0u;
        }
    } else {
        if (size > pipe.size) return err::kIllegalSize;
        while (pipe.used > 0u) {
            const std::uint32_t bytes = std::min(size, pipe.used);
            if (bytes == 0u) break;
            guest_copy(cursor, pipe.buffer, bytes);
            pipe.used -= bytes;
            guest_copy(pipe.buffer, pipe.buffer + bytes, pipe.used);
            cursor += bytes;
            size -= bytes;
            woke |= msgpipe_check_send(uid);
        }
        if (size != 0u && (mode != kWaitAsap || cursor == address)) {
            received = cursor - address;
            if (poll) return err::kMppEmpty;
            wait = true;
            return 0u;
        }
    }
    received = cursor - address;
    if (result_valid) m.store32(result_address, cursor - address);
    return 0u;
}

// Shared by the six send and receive calls.
void Kernel::msgpipe_transfer(Ctx &ctx, bool send, bool poll) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t address = ctx.gpr[5], size = ctx.gpr[6], mode = ctx.gpr[7], result_address = ctx.gpr[8];
    const std::uint32_t timeout = poll ? 0u : ctx.gpr[9];
    auto &m = rt_.memory();
    if ((size & 0x80000000u) != 0u) return finish(ctx, err::kIllegalAddr);
    if (size != 0u && !m.contains(address, 1u)) return finish(ctx, err::kIllegalAddr);
    if (mode != kWaitAsap && mode != kWaitFull) return finish(ctx, err::kIllegalMode);
    if (!poll && !dispatch_enabled_) return finish(ctx, err::kCanNotWait);
    if (!msgpipes_.contains(uid)) return finish(ctx, err::kUnknownMppid);

    bool woke = false, wait = false;
    std::uint32_t moved = 0u;
    const std::uint32_t result = send ? msgpipe_send(uid, address, size, mode, result_address, poll, woke, wait, moved)
                                      : msgpipe_receive(uid, address, size, mode, result_address, poll, woke, wait, moved);
    if (!wait) {
        finish(ctx, result);
        if (woke) maybe_preempt(ctx);
        return;
    }
    // Up to 2us the timeout is over before the wait begins.
    if (timeout != 0u && timeout_at_once(timeout, 210, 55)) {
        finish(ctx, err::kWaitTimeout);
        if (woke) maybe_preempt(ctx);
        return;
    }
    Thread *self = current();
    if (self != nullptr) {
        self->wait_uid = uid;
        self->wait_out_address = address + moved;
        self->wait_extra = size - moved;
        self->wait_count = size - moved;
        self->wait_mode = mode;
        self->wait_result_address = result_address;
        if (result_address != 0u && (result_address & 3u) == 0u && m.contains(result_address, 4u)) m.store32(result_address, 0u);
        begin_timeout(*self, timeout);
    }
    block(ctx, send ? WaitType::MsgPipeSend : WaitType::MsgPipeReceive, 0u);
}

// After callbacks: a waiting transfer tries again with what it still has to move.
bool Kernel::msgpipe_resume(Thread &t, std::uint32_t &result) {
    if (t.wait_count == 0u) {
        result = 0u;
        return true;
    }
    const std::uint32_t address = t.wait_out_address + (t.wait_extra - t.wait_count);
    const std::uint32_t size = t.wait_count;
    bool woke = false, wait = false;
    std::uint32_t moved = 0u;
    // A partial transfer is not kept: either the rest goes through now or the wait goes on.
    const std::uint32_t r = t.wait == WaitType::MsgPipeSend
        ? msgpipe_send(t.wait_uid, address, size, t.wait_mode, 0u, true, woke, wait, moved)
        : msgpipe_receive(t.wait_uid, address, size, t.wait_mode, 0u, true, woke, wait, moved);
    if (r == err::kMppFull || r == err::kMppEmpty) return false;
    auto &m = rt_.memory();
    if (t.wait_result_address != 0u && m.contains(t.wait_result_address, 4u))
        m.store32(t.wait_result_address, m.load32(t.wait_result_address) + moved);
    result = r;
    return true;
}

void Kernel::sceKernelCreateMsgPipe(Ctx &ctx) {
    const std::uint32_t name = ctx.gpr[4], attr = ctx.gpr[6], size = ctx.gpr[7];
    const auto partition = static_cast<std::int32_t>(ctx.gpr[5]);
    if (name == 0u) return finish(ctx, err::kNoMemory);
    if (const std::uint32_t e = partition_error(partition); e != 0u) return finish(ctx, e);
    if ((attr & ~kAttrKnown) >= 0x100u) return finish(ctx, err::kIllegalAttr);
    MsgPipe pipe;
    pipe.name = read_string(name, 31);
    pipe.attr = attr;
    pipe.size = size;
    if (size != 0u) {
        if (size >= 0x80000000u) return finish(ctx, err::kNoMemory);
        pipe.memory_block = allocate_in(partition, "msgpipe:" + pipe.name, (attr & kAttrHighMem) != 0u ? 1u : 0u, size, 0u);
        if (pipe.memory_block < 0) return finish(ctx, err::kNoMemory);
        pipe.buffer = blocks_.at(pipe.memory_block).address;
    }
    const std::int32_t uid = new_uid();
    msgpipes_[uid] = std::move(pipe);
    finish(ctx, static_cast<std::uint32_t>(uid));
}

void Kernel::sceKernelDeleteMsgPipe(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = msgpipes_.find(uid);
    if (it == msgpipes_.end()) return finish(ctx, err::kUnknownMppid);
    for (Thread *t : waiters(WaitType::MsgPipeSend, uid)) wake(*t, err::kWaitDelete);
    for (Thread *t : waiters(WaitType::MsgPipeReceive, uid)) wake(*t, err::kWaitDelete);
    if (it->second.memory_block >= 0) blocks_.erase(it->second.memory_block);
    msgpipes_.erase(it);
    finish(ctx, 0u);
    maybe_preempt(ctx);
}

void Kernel::sceKernelSendMsgPipe(Ctx &ctx) { msgpipe_transfer(ctx, true, false); }
void Kernel::sceKernelTrySendMsgPipe(Ctx &ctx) { msgpipe_transfer(ctx, true, true); }
void Kernel::sceKernelReceiveMsgPipe(Ctx &ctx) { msgpipe_transfer(ctx, false, false); }
void Kernel::sceKernelTryReceiveMsgPipe(Ctx &ctx) { msgpipe_transfer(ctx, false, true); }

void Kernel::sceKernelCancelMsgPipe(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = msgpipes_.find(uid);
    if (it == msgpipes_.end()) return finish(ctx, err::kUnknownMppid);
    auto &m = rt_.memory();
    const auto senders = waiters(WaitType::MsgPipeSend, uid);
    const auto receivers = waiters(WaitType::MsgPipeReceive, uid);
    if (ctx.gpr[5] != 0u && (ctx.gpr[5] & 3u) == 0u && m.contains(ctx.gpr[5], 4u))
        m.store32(ctx.gpr[5], static_cast<std::uint32_t>(senders.size()));
    if (ctx.gpr[6] != 0u && (ctx.gpr[6] & 3u) == 0u && m.contains(ctx.gpr[6], 4u))
        m.store32(ctx.gpr[6], static_cast<std::uint32_t>(receivers.size()));
    for (Thread *t : senders) wake(*t, err::kWaitCancel);
    for (Thread *t : receivers) wake(*t, err::kWaitCancel);
    // The whole buffer is free again.
    it->second.used = 0u;
    finish(ctx, 0u);
    maybe_preempt(ctx);
}

// SceKernelMppInfo: size, name[32], attr, bufSize, freeSize, numSendWaitThreads, numReceiveWaitThreads.
void Kernel::sceKernelReferMsgPipeStatus(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = msgpipes_.find(uid);
    if (it == msgpipes_.end()) return finish(ctx, err::kUnknownMppid);
    auto &m = rt_.memory();
    const std::uint32_t info = ctx.gpr[5];
    if ((info & 3u) != 0u || !m.contains(info, 4u)) return finish(ctx, 0xFFFFFFFFu);
    if (m.load32(info) == 0u) return finish(ctx, 0u);
    const MsgPipe &pipe = it->second;
    m.store32(info, 0x38u);
    write_object_name(info, pipe.name);
    m.store32(info + 0x24u, pipe.attr);
    m.store32(info + 0x28u, pipe.size);
    m.store32(info + 0x2Cu, pipe.size - pipe.used);
    m.store32(info + 0x30u, count_waiters(WaitType::MsgPipeSend, uid));
    m.store32(info + 0x34u, count_waiters(WaitType::MsgPipeReceive, uid));
    finish(ctx, 0u);
}

void Kernel::install_msgpipe() {
    const char *tm = "ThreadManForUser";
    hle(tm, 0x7C0DC2A0u, "sceKernelCreateMsgPipe", &Kernel::sceKernelCreateMsgPipe);
    hle(tm, 0xF0B7DA1Cu, "sceKernelDeleteMsgPipe", &Kernel::sceKernelDeleteMsgPipe);
    hle(tm, 0x876DBFADu, "sceKernelSendMsgPipe", &Kernel::sceKernelSendMsgPipe);
    hle_cb(tm, 0x7C41F2C2u, "sceKernelSendMsgPipeCB", &Kernel::sceKernelSendMsgPipe);
    hle(tm, 0x884C9F90u, "sceKernelTrySendMsgPipe", &Kernel::sceKernelTrySendMsgPipe);
    hle(tm, 0x74829B76u, "sceKernelReceiveMsgPipe", &Kernel::sceKernelReceiveMsgPipe);
    hle_cb(tm, 0xFBFA697Du, "sceKernelReceiveMsgPipeCB", &Kernel::sceKernelReceiveMsgPipe);
    hle(tm, 0xDF52098Fu, "sceKernelTryReceiveMsgPipe", &Kernel::sceKernelTryReceiveMsgPipe);
    hle(tm, 0x349B864Du, "sceKernelCancelMsgPipe", &Kernel::sceKernelCancelMsgPipe);
    hle(tm, 0x33BE4024u, "sceKernelReferMsgPipeStatus", &Kernel::sceKernelReferMsgPipeStatus);
}

} // namespace pspweb
