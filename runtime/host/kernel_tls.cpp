// SPDX-License-Identifier: GPL-2.0-or-later
//
// Thread-local storage pools (sceKernel*Tlspl): a pool of equal blocks, at most one per thread.
// A thread's block is cleared when handed out and when freed, and goes back to the pool when the
// thread ends. Semantics and error codes follow PPSSPP (Core/HLE/sceKernelMemory.cpp,
// GPL-2.0-or-later). A thread waiting for a block keeps in wait_out_address where the address
// goes (sceKernelGetTlsAddr: 0, the address is its result).

#include "kernel.hpp"
#include "kernel_errors.hpp"

#include <algorithm>

namespace pspweb {
namespace {

constexpr std::uint32_t kAttrPriority = 0x0100u;
constexpr std::uint32_t kAttrHighMem = 0x4000u;
constexpr std::uint32_t kAttrKnown = kAttrPriority | kAttrHighMem;
constexpr std::uint32_t kIndexes = 16u;

} // namespace

std::uint32_t Kernel::tlspl_block_address(const Tlspl &pool, std::size_t block) const {
    const std::uint32_t aligned = (pool.block_size + pool.alignment - 1u) & ~(pool.alignment - 1u);
    return pool.base + static_cast<std::uint32_t>(block) * aligned;
}

// The thread's block, handed out (and cleared) first if it has none; 0 when the pool is full.
std::uint32_t Kernel::tlspl_allocate(Tlspl &pool, std::int32_t thread_uid) {
    const auto owned = std::find(pool.owners.begin(), pool.owners.end(), thread_uid);
    if (owned != pool.owners.end()) return tlspl_block_address(pool, static_cast<std::size_t>(owned - pool.owners.begin()));
    const std::size_t count = pool.owners.size();
    for (std::size_t tried = 0; tried < count; ++tried) {
        // The PSP does not give the same block out twice in a row, even if it was freed.
        const std::size_t block = pool.next;
        pool.next = (pool.next + 1u) % count;
        if (pool.owners[block] != 0) continue;
        pool.owners[block] = thread_uid;
        const std::uint32_t address = tlspl_block_address(pool, block);
        rt_.memory().zero(address, pool.block_size);
        return address;
    }
    return 0u;
}

// Frees the thread's block; a waiting thread gets it at once.
void Kernel::tlspl_free(std::int32_t uid, std::int32_t thread_uid) {
    Tlspl &pool = tlspls_.at(uid);
    const auto owned = std::find(pool.owners.begin(), pool.owners.end(), thread_uid);
    if (owned == pool.owners.end()) return;
    const std::uint32_t address = tlspl_block_address(pool, static_cast<std::size_t>(owned - pool.owners.begin()));
    rt_.memory().zero(address, pool.block_size);
    const auto list = waiters(WaitType::Tlspl, uid, (pool.attr & kAttrPriority) != 0u);
    if (list.empty()) {
        *owned = 0;
        return;
    }
    Thread &next = *list.front();
    *owned = next.uid;
    if (next.wait_out_address != 0u) {
        rt_.memory().store32(next.wait_out_address, address);
        wake(next, 0u);
    } else {
        wake(next, address);
    }
}

void Kernel::tlspl_thread_end(std::int32_t thread_uid) {
    for (auto &[uid, pool] : tlspls_)
        if (std::find(pool.owners.begin(), pool.owners.end(), thread_uid) != pool.owners.end()) tlspl_free(uid, thread_uid);
}

void Kernel::sceKernelCreateTlspl(Ctx &ctx) {
    const std::uint32_t name = ctx.gpr[4], attr = ctx.gpr[6], block_size = ctx.gpr[7], count = ctx.gpr[8],
                        options = ctx.gpr[9];
    const auto partition = static_cast<std::int32_t>(ctx.gpr[5]);
    auto &m = rt_.memory();
    if (name == 0u) return finish(ctx, err::kNoMemory);
    if ((attr & ~kAttrKnown) >= 0x100u) return finish(ctx, err::kIllegalAttr);
    if (const std::uint32_t e = partition_error(partition); e != 0u) return finish(ctx, e);
    bool illegal = block_size == 0u || count == 0u;
    if (!illegal && static_cast<std::uint64_t>(block_size) > (0x100000000ull / count) - 4ull) illegal = true;
    if (!illegal && static_cast<std::uint64_t>(count) >= 0x100000000ull / ((static_cast<std::uint64_t>(block_size) + 3ull) & ~3ull))
        illegal = true;
    if (illegal) return finish(ctx, err::kIllegalMemsize);
    std::uint32_t index = kIndexes;
    for (std::uint32_t i = 0; i < kIndexes && index == kIndexes; ++i) {
        const bool used = std::any_of(tlspls_.begin(), tlspls_.end(), [i](const auto &entry) { return entry.second.index == i; });
        if (!used) index = i;
    }
    if (index == kIndexes) return finish(ctx, err::kTooManyTlspl);
    // Blocks are word aligned unless the options ask for more.
    std::uint32_t alignment = 4u;
    if (options != 0u && m.contains(options, 8u)) {
        if (m.load32(options) >= 8u) alignment = m.load32(options + 4u);
        if ((alignment & (alignment - 1u)) != 0u) return finish(ctx, err::kIllegalArgument);
        alignment = std::max(alignment, 4u);
    }
    const std::uint32_t aligned = (block_size + alignment - 1u) & ~(alignment - 1u);
    if (static_cast<std::uint64_t>(aligned) * count > 0xFFFFFFFFull) return finish(ctx, err::kIllegalMemsize);
    Tlspl pool;
    pool.name = read_string(name, 31);
    pool.attr = attr;
    pool.index = index;
    pool.block_size = block_size;
    pool.alignment = alignment;
    pool.memory_block = allocate_in(partition, "tlspl:" + pool.name, (attr & kAttrHighMem) != 0u ? 1u : 0u, aligned * count, 0u);
    if (pool.memory_block < 0) return finish(ctx, err::kNoMemory);
    pool.base = blocks_.at(pool.memory_block).address;
    pool.owners.assign(count, 0);
    const std::int32_t uid = new_uid();
    tlspls_[uid] = std::move(pool);
    finish(ctx, static_cast<std::uint32_t>(uid));
}

void Kernel::sceKernelDeleteTlspl(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = tlspls_.find(uid);
    if (it == tlspls_.end()) return finish(ctx, err::kUnknownTlsplId);
    for (const std::int32_t owner : it->second.owners)
        if (owner != 0 && owner != current_uid_) return finish(ctx, err::kTlsplInUse);
    // sceKernelGetTlsAddr gets a null address, _sceKernelAllocateTlspl an error.
    for (Thread *t : waiters(WaitType::Tlspl, uid)) wake(*t, t->wait_out_address != 0u ? err::kWaitDelete : 0u);
    blocks_.erase(it->second.memory_block);
    tlspls_.erase(it);
    finish(ctx, 0u);
    maybe_preempt(ctx);
}

void Kernel::sceKernelFreeTlspl(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (!tlspls_.contains(uid)) return finish(ctx, err::kUnknownTlsplId);
    tlspl_free(uid, current_uid_);
    finish(ctx, 0u);
    maybe_preempt(ctx);
}

void Kernel::sceKernelGetTlsAddr(Ctx &ctx) {
    auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (!dispatch_enabled_) return finish(ctx, 0u);
    if (!tlspls_.contains(uid)) {
        // An unknown id is looked up by the pool index in its bits 3-6.
        const auto index = static_cast<std::uint32_t>(uid >> 3) & 15u;
        const auto found = std::find_if(tlspls_.begin(), tlspls_.end(), [index](const auto &entry) { return entry.second.index == index; });
        if (uid < 0 || found == tlspls_.end()) return finish(ctx, 0u);
        uid = found->first;
    }
    if (const std::uint32_t address = tlspl_allocate(tlspls_.at(uid), current_uid_); address != 0u) return finish(ctx, address);
    Thread *self = current();
    if (self != nullptr) {
        self->wait_uid = uid;
        self->wait_out_address = 0u;
        self->timeout_address = 0u;
        self->wake_time_us = 0u;
    }
    block(ctx, WaitType::Tlspl, 0u);
}

void Kernel::sceKernelAllocateTlspl(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t out = ctx.gpr[5], timeout = ctx.gpr[6];
    auto &m = rt_.memory();
    const auto bad = [&m](std::uint32_t pointer) {
        return ((pointer | (pointer + 4u)) & 0x80000000u) != 0u || (pointer != 0u && !m.contains(pointer, 4u));
    };
    if (out == 0u || bad(out) || bad(timeout)) return finish(ctx, err::kIllegalAddr);
    if (!dispatch_enabled_) return finish(ctx, err::kCanNotWait);
    const auto it = tlspls_.find(uid);
    if (it == tlspls_.end()) return finish(ctx, err::kUnknownTlsplId);
    if (const std::uint32_t address = tlspl_allocate(it->second, current_uid_); address != 0u) {
        m.store32(out, address);
        return finish(ctx, 0u);
    }
    if (timeout_at_once(timeout)) return finish(ctx, err::kWaitTimeout);
    Thread *self = current();
    if (self != nullptr) {
        self->wait_uid = uid;
        self->wait_out_address = out;
        begin_timeout(*self, timeout);
    }
    block(ctx, WaitType::Tlspl, 0u);
}

// SceKernelTlsplInfo: size, name[32], attr, index, blockSize, totalBlocks, freeBlocks, numWaitThreads.
void Kernel::sceKernelReferTlsplStatus(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = tlspls_.find(uid);
    if (it == tlspls_.end()) return finish(ctx, err::kUnknownTlsplId);
    auto &m = rt_.memory();
    const std::uint32_t info = ctx.gpr[5];
    if (info == 0u || (info & 3u) != 0u || !m.contains(info, 4u) || m.load32(info) == 0u) return finish(ctx, 0u);
    const Tlspl &pool = it->second;
    m.store32(info, 0x3Cu);
    write_object_name(info, pool.name);
    m.store32(info + 0x24u, pool.attr);
    m.store32(info + 0x28u, pool.index);
    m.store32(info + 0x2Cu, pool.block_size);
    m.store32(info + 0x30u, static_cast<std::uint32_t>(pool.owners.size()));
    m.store32(info + 0x34u, static_cast<std::uint32_t>(std::count(pool.owners.begin(), pool.owners.end(), 0)));
    m.store32(info + 0x38u, count_waiters(WaitType::Tlspl, uid));
    finish(ctx, 0u);
}

void Kernel::install_tlspl() {
    const char *tm = "ThreadManForUser";
    hle(tm, 0x8DAFF657u, "sceKernelCreateTlspl", &Kernel::sceKernelCreateTlspl);
    hle(tm, 0x32BF938Eu, "sceKernelDeleteTlspl", &Kernel::sceKernelDeleteTlspl);
    hle(tm, 0x721067F3u, "sceKernelReferTlsplStatus", &Kernel::sceKernelReferTlsplStatus);
    hle(tm, 0x4A719FB2u, "sceKernelFreeTlspl", &Kernel::sceKernelFreeTlspl);
    hle(tm, 0x65F54FFBu, "_sceKernelAllocateTlspl", &Kernel::sceKernelAllocateTlspl);
    hle("Kernel_Library", 0xFA835CDEu, "sceKernelGetTlsAddr", &Kernel::sceKernelGetTlsAddr);
}

} // namespace pspweb
