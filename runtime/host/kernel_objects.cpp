// SPDX-License-Identifier: GPL-2.0-or-later
//
// Fixed-size and variable-size memory pools (sceKernel*Fpl, sceKernel*Vpl), and helpers the
// Refer*Status calls share. A variable pool keeps its free list in guest memory, in the pool's
// own first 32 bytes and an 8-byte header before each block, as the PSP does, so that games and
// tests see the same addresses and free sizes. Semantics and error codes follow PPSSPP
// (Core/HLE/sceKernelMemory.cpp, GPL-2.0-or-later).

#include "kernel.hpp"
#include "kernel_errors.hpp"

#include <algorithm>

namespace pspweb {
namespace {

constexpr std::uint32_t kAttrPriority = 0x0100u;
constexpr std::uint32_t kAttrHighMem = 0x4000u;
constexpr std::uint32_t kFplAttrKnown = kAttrPriority | kAttrHighMem;
constexpr std::uint32_t kVplAttrKnown = kAttrPriority | 0x0200u | kAttrHighMem;
constexpr std::uint32_t kVplOrderMask = 0x0300u;

// SceKernelVplHeader, at the start of the pool's memory block.
constexpr std::uint32_t kVplStart = 0u, kVplStart2 = 4u, kVplSentinel = 8u, kVplSizeMinus8 = 12u, kVplAllocated = 16u,
                        kVplNextFree = 20u, kVplFirstBlock = 24u;

bool illegal_pool_size(std::uint32_t block_size, std::uint32_t count) {
    if (block_size == 0u || count == 0u) return true;
    if (static_cast<std::uint64_t>(block_size) > (0x100000000ull / count) - 4ull) return true;
    return static_cast<std::uint64_t>(count) >= 0x100000000ull / ((static_cast<std::uint64_t>(block_size) + 3ull) & ~3ull);
}

} // namespace

void Kernel::write_object_name(std::uint32_t info, const std::string &name) {
    // 31 characters and a terminator, as the kernel stores them.
    for (std::uint32_t i = 0; i < 32u; ++i)
        rt_.memory().store8(info + 4u + i, i < name.size() && i < 31u ? static_cast<std::uint8_t>(name[i]) : 0u);
}

bool Kernel::begin_refer(Ctx &ctx, std::uint32_t info, std::uint32_t size) {
    if (info == 0u) {
        finish(ctx, 0xFFFFFFFFu);
        return false;
    }
    // A zero size field means the caller wants nothing written.
    if (rt_.memory().load32(info) == 0u) {
        finish(ctx, 0u);
        return false;
    }
    rt_.memory().store32(info, size);
    return true;
}

std::uint32_t Kernel::count_waiters(WaitType wait, std::int32_t uid) const {
    std::uint32_t n = 0u;
    for (const auto &[tid, t] : threads_) {
        (void)tid;
        if (waits_on(t, wait, uid)) ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Fixed-size pools. A waiter keeps where the block address goes in wait_out_address.

std::uint32_t Kernel::fpl_allocate(FixedPool &pool) {
    const std::size_t count = pool.used.size();
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t block = pool.next % count;
        ++pool.next;
        if (pool.used[block]) continue;
        pool.used[block] = true;
        return pool.base + static_cast<std::uint32_t>(block) * pool.aligned_size;
    }
    return 0u;
}

void Kernel::sceKernelCreateFpl(Ctx &ctx) {
    const std::uint32_t name = ctx.gpr[4], attr = ctx.gpr[6], block_size = ctx.gpr[7], count = ctx.gpr[8],
                        options = ctx.gpr[9];
    const auto partition = static_cast<std::int32_t>(ctx.gpr[5]);
    auto &m = rt_.memory();
    if (name == 0u) return finish(ctx, err::kNoMemory);
    if (const std::uint32_t e = partition_error(partition); e != 0u) return finish(ctx, e);
    if (((attr & ~kFplAttrKnown) & ~0xFFu) != 0u) return finish(ctx, err::kIllegalAttr);
    if (illegal_pool_size(block_size, count)) return finish(ctx, err::kIllegalMemsize);
    std::uint32_t alignment = 4u;
    if (options != 0u && m.contains(options, 8u)) {
        if (m.load32(options) >= 4u) alignment = m.load32(options + 4u);
        if ((alignment & (alignment - 1u)) != 0u) return finish(ctx, err::kIllegalArgument);
    }
    alignment = std::max(alignment, 4u);
    const std::uint32_t aligned = (block_size + alignment - 1u) & ~(alignment - 1u);
    if (static_cast<std::uint64_t>(aligned) * count > 0xFFFFFFFFull) return finish(ctx, err::kIllegalMemsize);
    FixedPool pool;
    pool.name = read_string(name, 31);
    pool.attr = attr;
    pool.block_size = block_size;
    pool.aligned_size = aligned;
    pool.memory_block = allocate_in(partition, "fpl:" + pool.name, (attr & kAttrHighMem) != 0u ? 1u : 0u, aligned * count, 0u);
    if (pool.memory_block < 0) return finish(ctx, err::kNoMemory);
    pool.base = blocks_.at(pool.memory_block).address;
    pool.used.assign(count, false);
    const std::int32_t uid = new_uid();
    fpls_[uid] = std::move(pool);
    finish(ctx, static_cast<std::uint32_t>(uid));
}

void Kernel::sceKernelDeleteFpl(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = fpls_.find(uid);
    if (it == fpls_.end()) return finish(ctx, err::kUnknownFplid);
    const auto list = waiters(WaitType::Fpl, uid);
    for (Thread *t : list) wake(*t, err::kWaitDelete);
    blocks_.erase(it->second.memory_block);
    fpls_.erase(it);
    finish(ctx, 0u);
    if (!list.empty()) maybe_preempt(ctx);
}

void Kernel::allocate_fpl(Ctx &ctx, bool wait) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t out = ctx.gpr[5], timeout = ctx.gpr[6];
    const auto it = fpls_.find(uid);
    if (it == fpls_.end()) return finish(ctx, err::kUnknownFplid);
    if (const std::uint32_t address = fpl_allocate(it->second); address != 0u) {
        if (rt_.memory().contains(out, 4u)) rt_.memory().store32(out, address);
        return finish(ctx, 0u);
    }
    if (!wait) return finish(ctx, err::kNoMemory);
    if (timeout_at_once(timeout)) return finish(ctx, err::kWaitTimeout);
    Thread *self = current();
    if (self != nullptr) {
        self->wait_uid = uid;
        self->wait_out_address = out;
        begin_timeout(*self, timeout);
    }
    block(ctx, WaitType::Fpl, 0u);
}

void Kernel::sceKernelAllocateFpl(Ctx &ctx) { allocate_fpl(ctx, true); }
void Kernel::sceKernelTryAllocateFpl(Ctx &ctx) { allocate_fpl(ctx, false); }

void Kernel::sceKernelFreeFpl(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t address = ctx.gpr[5];
    if (address > kUserPartitionEnd) return finish(ctx, err::kIllegalAddr);
    const auto it = fpls_.find(uid);
    if (it == fpls_.end()) return finish(ctx, err::kUnknownFplid);
    FixedPool &pool = it->second;
    const auto block = static_cast<std::int32_t>(address - pool.base) / static_cast<std::int32_t>(pool.aligned_size);
    if (address < pool.base || block < 0 || block >= static_cast<std::int32_t>(pool.used.size()) ||
        !pool.used[static_cast<std::size_t>(block)])
        return finish(ctx, err::kIllegalMemblock);
    pool.used[static_cast<std::size_t>(block)] = false;
    bool woke = false;
    for (Thread *t : waiters(WaitType::Fpl, uid, (pool.attr & kAttrPriority) != 0u)) {
        const std::uint32_t given = fpl_allocate(pool);
        if (given == 0u) break;
        if (rt_.memory().contains(t->wait_out_address, 4u)) rt_.memory().store32(t->wait_out_address, given);
        wake(*t, 0u);
        woke = true;
    }
    finish(ctx, 0u);
    if (woke) maybe_preempt(ctx);
}

void Kernel::sceKernelCancelFpl(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (!fpls_.contains(uid)) return finish(ctx, err::kUnknownFplid);
    const auto list = waiters(WaitType::Fpl, uid);
    if (valid_word(ctx.gpr[5])) rt_.memory().store32(ctx.gpr[5], static_cast<std::uint32_t>(list.size()));
    for (Thread *t : list) wake(*t, err::kWaitCancel);
    finish(ctx, 0u);
    if (!list.empty()) maybe_preempt(ctx);
}

// SceKernelFplInfo: size, name[32], attr, blockSize, numBlocks, numFreeBlocks, numWaitThreads.
void Kernel::sceKernelReferFplStatus(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = fpls_.find(uid);
    if (it == fpls_.end()) return finish(ctx, err::kUnknownFplid);
    auto &m = rt_.memory();
    const FixedPool &pool = it->second;
    const std::uint32_t info = ctx.gpr[5];
    if (!valid_word(info) || m.load32(info) == 0u) return finish(ctx, 0u);
    m.store32(info, 0x38u);
    write_object_name(info, pool.name);
    m.store32(info + 0x24u, pool.attr);
    m.store32(info + 0x28u, pool.block_size);
    m.store32(info + 0x2Cu, static_cast<std::uint32_t>(pool.used.size()));
    m.store32(info + 0x30u, static_cast<std::uint32_t>(std::count(pool.used.begin(), pool.used.end(), false)));
    m.store32(info + 0x34u, count_waiters(WaitType::Fpl, uid));
    finish(ctx, 0u);
}

// ---------------------------------------------------------------------------
// Variable-size pools. Blocks are 8 bytes: a next pointer and a size in blocks. Free blocks form
// a circular list in address order through the last block; allocated ones point at the sentinel.

void Kernel::vpl_init(std::uint32_t header, std::uint32_t size) {
    auto &m = rt_.memory();
    m.store32(header + kVplStart, header);
    m.store32(header + kVplStart2, header);
    m.store32(header + kVplSentinel, header + 7u);
    m.store32(header + kVplSizeMinus8, size - 8u);
    m.store32(header + kVplAllocated, 0u);
    const std::uint32_t first = header + kVplFirstBlock, last = header + size - 8u;
    m.store32(header + kVplNextFree, first);
    m.store32(first, last);
    // The first block includes its own header.
    m.store32(first + 4u, (size - 0x28u) / 8u + 1u);
    m.store32(last, first);
    m.store32(last + 4u, 0u);
}

std::uint32_t Kernel::vpl_allocate(const VariablePool &pool, std::uint32_t size) {
    auto &m = rt_.memory();
    const std::uint32_t h = pool.header;
    const std::uint32_t blocks = (size + 7u) / 8u + 1u;
    const std::uint32_t start_free = m.load32(h + kVplNextFree);
    if (!m.contains(start_free, 8u)) return 0u;
    std::uint32_t prev = start_free;
    do {
        std::uint32_t b = m.load32(prev);
        if (!m.contains(b, 8u)) return 0u;
        if (m.load32(b + 4u) > blocks) {
            // Split: the allocation comes from the end of the free block.
            const std::uint32_t remain = m.load32(b + 4u) - blocks;
            m.store32(b + 4u, remain);
            const std::uint32_t split = b + remain * 8u;
            m.store32(split + 4u, blocks);
            m.store32(split, b);
            b = split;
        }
        if (m.load32(b + 4u) == blocks) {
            m.store32(h + kVplAllocated, m.load32(h + kVplAllocated) + blocks);
            m.store32(prev, m.load32(b));
            m.store32(h + kVplNextFree, prev);
            m.store32(b, h + kVplSentinel);
            return b + 8u;
        }
        prev = b;
    } while (m.contains(prev, 8u) && prev != start_free);
    return 0u;
}

bool Kernel::vpl_free(const VariablePool &pool, std::uint32_t address) {
    auto &m = rt_.memory();
    const std::uint32_t h = pool.header;
    const std::uint32_t first = h + kVplFirstBlock, last = h + m.load32(h + kVplSizeMinus8), sentinel = h + kVplSentinel;
    std::uint32_t b = address - 8u;
    if (!m.contains(b, 8u) || address < first || address >= last) return false;
    if (m.load32(b) != sentinel || m.load32(b + 4u) > m.load32(h + kVplAllocated)) return false;
    std::uint32_t prev = last;
    do {
        const std::uint32_t next = m.load32(prev);
        if (next == b) return false;
        if (next > b) {
            m.store32(h + kVplAllocated, m.load32(h + kVplAllocated) - m.load32(b + 4u));
            m.store32(h + kVplNextFree, prev);
            m.store32(b, next);
            if (b + m.load32(b + 4u) * 8u == next && m.load32(next + 4u) != 0u) {
                m.store32(b + 4u, m.load32(b + 4u) + m.load32(next + 4u));
                m.store32(b, m.load32(next));
            }
            if (prev + m.load32(prev + 4u) * 8u == b) {
                m.store32(prev + 4u, m.load32(prev + 4u) + m.load32(b + 4u));
                m.store32(prev, m.load32(b));
            } else {
                m.store32(prev, b);
            }
            return true;
        }
        prev = next;
    } while (m.contains(prev, 8u) && prev != last);
    return false;
}

std::uint32_t Kernel::vpl_free_size(const VariablePool &pool) const {
    const auto &m = rt_.memory();
    return m.load32(pool.header + kVplSizeMinus8) + 8u - 0x20u - m.load32(pool.header + kVplAllocated) * 8u;
}

void Kernel::sceKernelCreateVpl(Ctx &ctx) {
    const std::uint32_t name = ctx.gpr[4], attr = ctx.gpr[6];
    const auto partition = static_cast<std::int32_t>(ctx.gpr[5]);
    std::uint32_t size = ctx.gpr[7];
    if (name == 0u) return finish(ctx, err::kError);
    if (const std::uint32_t e = partition_error(partition); e != 0u) return finish(ctx, e);
    if (((attr & ~kVplAttrKnown) & ~0xFFu) != 0u) return finish(ctx, err::kIllegalAttr);
    if (size == 0u) return finish(ctx, err::kIllegalMemsize);
    if (size >= 0x80000000u) return finish(ctx, err::kNoMemory);
    // A pool needs room for its bookkeeping.
    if (size <= 0x30u) size = 0x1000u;
    size = (size + 7u) & ~7u;
    VariablePool pool;
    pool.name = read_string(name, 31);
    pool.attr = attr;
    pool.memory_block = allocate_in(partition, "vpl:" + pool.name, (attr & kAttrHighMem) != 0u ? 1u : 0u, size, 0u);
    if (pool.memory_block < 0) return finish(ctx, err::kNoMemory);
    pool.header = blocks_.at(pool.memory_block).address;
    pool.pool_size = size - 0x20u;
    vpl_init(pool.header, size);
    const std::int32_t uid = new_uid();
    vpls_[uid] = std::move(pool);
    finish(ctx, static_cast<std::uint32_t>(uid));
}

void Kernel::sceKernelDeleteVpl(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = vpls_.find(uid);
    if (it == vpls_.end()) return finish(ctx, err::kUnknownVplid);
    const auto list = waiters(WaitType::Vpl, uid);
    for (Thread *t : list) wake(*t, err::kWaitDelete);
    blocks_.erase(it->second.memory_block);
    vpls_.erase(it);
    finish(ctx, 0u);
    if (!list.empty()) maybe_preempt(ctx);
}

void Kernel::allocate_vpl(Ctx &ctx, bool wait) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t size = ctx.gpr[5], out = ctx.gpr[6], timeout = ctx.gpr[7];
    const auto it = vpls_.find(uid);
    if (it == vpls_.end()) return finish(ctx, err::kUnknownVplid);
    VariablePool &pool = it->second;
    if (size == 0u || size > pool.pool_size) return finish(ctx, err::kIllegalMemsize);
    // In FIFO order a request may not overtake one already waiting (sceKernelTryAllocateVpl may).
    const bool blocked = wait && (pool.attr & kVplOrderMask) == 0u && count_waiters(WaitType::Vpl, uid) != 0u;
    if (!blocked) {
        if (const std::uint32_t address = vpl_allocate(pool, size); address != 0u) {
            if (rt_.memory().contains(out, 4u)) rt_.memory().store32(out, address);
            return finish(ctx, 0u);
        }
    }
    if (!wait) return finish(ctx, err::kNoMemory);
    // Allocating does more before the timeout is set, so short ones time out at once more often.
    if (timeout_at_once(timeout, 117, 17)) return finish(ctx, err::kWaitTimeout);
    Thread *self = current();
    if (self != nullptr) {
        self->wait_uid = uid;
        self->wait_count = size;
        self->wait_out_address = out;
        begin_timeout(*self, timeout);
    }
    block(ctx, WaitType::Vpl, 0u);
}

void Kernel::sceKernelAllocateVpl(Ctx &ctx) { allocate_vpl(ctx, true); }
void Kernel::sceKernelTryAllocateVpl(Ctx &ctx) { allocate_vpl(ctx, false); }

// Serves waiters after memory came back: by priority with that attribute, stopping at the first
// that still does not fit in FIFO order.
bool Kernel::vpl_wake(std::int32_t uid) {
    VariablePool &pool = vpls_.at(uid);
    const bool fifo = (pool.attr & kVplOrderMask) == 0u;
    bool woke = false;
    for (Thread *t : waiters(WaitType::Vpl, uid, (pool.attr & kAttrPriority) != 0u)) {
        const std::uint32_t address = vpl_allocate(pool, t->wait_count);
        if (address == 0u) {
            if (fifo) break;
            continue;
        }
        if (rt_.memory().contains(t->wait_out_address, 4u)) rt_.memory().store32(t->wait_out_address, address);
        wake(*t, 0u);
        woke = true;
    }
    return woke;
}

void Kernel::sceKernelFreeVpl(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t address = ctx.gpr[5];
    if (address != 0u && !rt_.memory().contains(address, 1u)) return finish(ctx, err::kIllegalAddr);
    const auto it = vpls_.find(uid);
    if (it == vpls_.end()) return finish(ctx, err::kUnknownVplid);
    if (!vpl_free(it->second, address)) return finish(ctx, err::kIllegalMemblock);
    const bool woke = vpl_wake(uid);
    finish(ctx, 0u);
    if (woke) maybe_preempt(ctx);
}

void Kernel::sceKernelCancelVpl(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (!vpls_.contains(uid)) return finish(ctx, err::kUnknownVplid);
    const auto list = waiters(WaitType::Vpl, uid);
    if (valid_word(ctx.gpr[5])) rt_.memory().store32(ctx.gpr[5], static_cast<std::uint32_t>(list.size()));
    for (Thread *t : list) wake(*t, err::kWaitCancel);
    finish(ctx, 0u);
    if (!list.empty()) maybe_preempt(ctx);
}

// SceKernelVplInfo: size, name[32], attr, poolSize, freeSize, numWaitThreads.
void Kernel::sceKernelReferVplStatus(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = vpls_.find(uid);
    if (it == vpls_.end()) return finish(ctx, err::kUnknownVplid);
    auto &m = rt_.memory();
    const std::uint32_t info = ctx.gpr[5];
    if (!valid_word(info) || m.load32(info) == 0u) return finish(ctx, 0u);
    m.store32(info, 0x34u);
    write_object_name(info, it->second.name);
    m.store32(info + 0x24u, it->second.attr);
    m.store32(info + 0x28u, it->second.pool_size);
    m.store32(info + 0x2Cu, vpl_free_size(it->second));
    m.store32(info + 0x30u, count_waiters(WaitType::Vpl, uid));
    finish(ctx, 0u);
}

void Kernel::install_kernel_objects() {
    const char *tm = "ThreadManForUser";
    hle(tm, 0xC07BB470u, "sceKernelCreateFpl", &Kernel::sceKernelCreateFpl);
    hle(tm, 0xED1410E0u, "sceKernelDeleteFpl", &Kernel::sceKernelDeleteFpl);
    hle(tm, 0xD979E9BFu, "sceKernelAllocateFpl", &Kernel::sceKernelAllocateFpl);
    hle_cb(tm, 0xE7282CB6u, "sceKernelAllocateFplCB", &Kernel::sceKernelAllocateFpl);
    hle(tm, 0x623AE665u, "sceKernelTryAllocateFpl", &Kernel::sceKernelTryAllocateFpl);
    hle(tm, 0xF6414A71u, "sceKernelFreeFpl", &Kernel::sceKernelFreeFpl);
    hle(tm, 0xA8AA591Fu, "sceKernelCancelFpl", &Kernel::sceKernelCancelFpl);
    hle(tm, 0xD8199E4Cu, "sceKernelReferFplStatus", &Kernel::sceKernelReferFplStatus);
    hle(tm, 0x56C039B5u, "sceKernelCreateVpl", &Kernel::sceKernelCreateVpl);
    hle(tm, 0x89B3D48Cu, "sceKernelDeleteVpl", &Kernel::sceKernelDeleteVpl);
    hle(tm, 0xBED27435u, "sceKernelAllocateVpl", &Kernel::sceKernelAllocateVpl);
    hle_cb(tm, 0xEC0A693Fu, "sceKernelAllocateVplCB", &Kernel::sceKernelAllocateVpl);
    hle(tm, 0xAF36D708u, "sceKernelTryAllocateVpl", &Kernel::sceKernelTryAllocateVpl);
    hle(tm, 0xB736E9FFu, "sceKernelFreeVpl", &Kernel::sceKernelFreeVpl);
    hle(tm, 0x1D371B8Au, "sceKernelCancelVpl", &Kernel::sceKernelCancelVpl);
    hle(tm, 0x39810265u, "sceKernelReferVplStatus", &Kernel::sceKernelReferVplStatus);
}

} // namespace pspweb
