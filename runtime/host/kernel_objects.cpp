// Variable-size memory pools, thread suspension and the Refer*Status calls for
// semaphores, mailboxes, fixed pools and mutexes.

#include "kernel.hpp"

#include <algorithm>

namespace pspweb {
namespace {

constexpr std::uint32_t kErrorError = 0x80020001u;
constexpr std::uint32_t kErrorIllegalAttr = 0x800200D9u;
constexpr std::uint32_t kErrorIllegalArgument = 0x800200D2u;
constexpr std::uint32_t kErrorNoMemory = 0x80020190u;
constexpr std::uint32_t kErrorIllegalThid = 0x80020197u;
constexpr std::uint32_t kErrorUnknownThread = 0x80020198u;
constexpr std::uint32_t kErrorUnknownSema = 0x80020199u;
constexpr std::uint32_t kErrorUnknownMbx = 0x8002019Bu;
constexpr std::uint32_t kErrorUnknownVpl = 0x8002019Cu;
constexpr std::uint32_t kErrorUnknownFpl = 0x8002019Du;
constexpr std::uint32_t kErrorDormant = 0x800201A2u;
constexpr std::uint32_t kErrorSuspend = 0x800201A3u;
constexpr std::uint32_t kErrorNotSuspend = 0x800201A5u;
constexpr std::uint32_t kErrorWaitDelete = 0x800201B5u;
constexpr std::uint32_t kErrorIllegalMemsize = 0x800201B7u;
constexpr std::uint32_t kErrorIllegalAddr = 0x800200D3u;
constexpr std::uint32_t kErrorUnknownMutex = 0x800201C3u;
constexpr std::uint32_t kVplAttrHighMem = 0x4000u;
constexpr std::uint32_t kVplAttrKnown = 0x4000u | 0x0100u | 0x0200u; // high memory, priority order, ...
constexpr std::uint32_t kVplHeader = 0x20u;   // pool bookkeeping at the start of its block
constexpr std::uint32_t kVplChunkHeader = 8u; // per-allocation header before the returned address

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
        if (t.state == ThreadState::Waiting && t.wait == wait && t.wait_uid == uid) ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Suspension: a suspended thread keeps its state (ready or waiting) but is never picked to run.

// ---------------------------------------------------------------------------
// Refer*Status: the guest sets the size field; the rest of the structure is written here.

void Kernel::sceKernelReferSemaStatus(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = semaphores_.find(uid);
    if (it == semaphores_.end()) {
        finish(ctx, kErrorUnknownSema);
        return;
    }
    auto &m = rt_.memory();
    const std::uint32_t info = ctx.gpr[5];
    if (!begin_refer(ctx, info, 0x38u)) return;
    write_object_name(info, it->second.name);
    m.store32(info + 0x24u, it->second.attr);
    m.store32(info + 0x28u, static_cast<std::uint32_t>(it->second.init));
    m.store32(info + 0x2Cu, static_cast<std::uint32_t>(it->second.count));
    m.store32(info + 0x30u, static_cast<std::uint32_t>(it->second.max));
    m.store32(info + 0x34u, count_waiters(WaitType::Sema, uid));
    finish(ctx, 0u);
}

void Kernel::sceKernelReferMbxStatus(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = mailboxes_.find(uid);
    if (it == mailboxes_.end()) {
        finish(ctx, kErrorUnknownMbx);
        return;
    }
    auto &m = rt_.memory();
    const std::uint32_t info = ctx.gpr[5];
    if (!begin_refer(ctx, info, 0x34u)) return;
    write_object_name(info, it->second.name);
    m.store32(info + 0x24u, it->second.attr);
    m.store32(info + 0x28u, count_waiters(WaitType::Mbx, uid));
    m.store32(info + 0x2Cu, static_cast<std::uint32_t>(it->second.messages.size()));
    m.store32(info + 0x30u, it->second.messages.empty() ? 0u : it->second.messages.front());
    finish(ctx, 0u);
}

void Kernel::sceKernelReferFplStatus(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = fpls_.find(uid);
    if (it == fpls_.end()) {
        finish(ctx, kErrorUnknownFpl);
        return;
    }
    auto &m = rt_.memory();
    const FixedPool &pool = it->second;
    const std::uint32_t info = ctx.gpr[5];
    if (!begin_refer(ctx, info, 0x38u)) return;
    write_object_name(info, pool.name);
    m.store32(info + 0x24u, pool.attr);
    m.store32(info + 0x28u, pool.block_size);
    m.store32(info + 0x2Cu, static_cast<std::uint32_t>(pool.used.size()));
    m.store32(info + 0x30u, static_cast<std::uint32_t>(std::count(pool.used.begin(), pool.used.end(), false)));
    m.store32(info + 0x34u, count_waiters(WaitType::Fpl, uid));
    finish(ctx, 0u);
}

void Kernel::sceKernelReferMutexStatus(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = mutexes_.find(uid);
    if (it == mutexes_.end()) {
        finish(ctx, kErrorUnknownMutex);
        return;
    }
    auto &m = rt_.memory();
    const std::uint32_t info = ctx.gpr[5];
    if (!begin_refer(ctx, info, 0x38u)) return;
    write_object_name(info, it->second.name);
    m.store32(info + 0x24u, it->second.attr);
    m.store32(info + 0x28u, static_cast<std::uint32_t>(it->second.init));
    m.store32(info + 0x2Cu, static_cast<std::uint32_t>(it->second.count));
    m.store32(info + 0x30u, it->second.count > 0 ? static_cast<std::uint32_t>(it->second.owner) : 0xFFFFFFFFu);
    m.store32(info + 0x34u, count_waiters(WaitType::Mutex, uid));
    finish(ctx, 0u);
}

// ---------------------------------------------------------------------------
// Variable-size pools. Allocations take the highest free range, as on the PSP, and each
// carries an 8-byte header so that the pool's free size matches the guest's accounting.

std::uint32_t VariablePool::free_size() const {
    std::uint32_t used = 0u;
    for (const auto &[address, size] : chunks) {
        (void)address;
        used += size;
    }
    return pool_size - used;
}

std::uint32_t VariablePool::allocate(std::uint32_t size) {
    const std::uint32_t need = ((size + 7u) & ~7u) + kVplChunkHeader;
    std::uint32_t end = base + pool_size;
    // chunks is ordered by address; walk from the top down looking for a gap.
    for (auto it = chunks.rbegin();; ++it) {
        const std::uint32_t begin = it == chunks.rend() ? base : it->first + it->second;
        if (end - begin >= need) {
            const std::uint32_t chunk = end - need;
            chunks.emplace(chunk, need);
            return chunk + kVplChunkHeader;
        }
        if (it == chunks.rend()) return 0u;
        end = it->first;
    }
}

bool VariablePool::free(std::uint32_t address) {
    return chunks.erase(address - kVplChunkHeader) != 0u;
}

void Kernel::sceKernelCreateVpl(Ctx &ctx) {
    const std::uint32_t name = ctx.gpr[4], partition = ctx.gpr[5], attr = ctx.gpr[6];
    std::uint32_t size = ctx.gpr[7];
    if (name == 0u) return finish(ctx, kErrorError);
    if (partition < 1u || partition > 6u) return finish(ctx, kErrorIllegalArgument);
    if (((attr & ~kVplAttrKnown) & ~0xFFu) != 0u) return finish(ctx, kErrorIllegalAttr);
    if (size == 0u) return finish(ctx, kErrorIllegalMemsize);
    if (size >= 0x80000000u) return finish(ctx, kErrorNoMemory);
    if (size <= 0x30u) size = 0x1000u;
    size = (size + 7u) & ~7u;
    VariablePool pool;
    pool.name = read_string(name, 32);
    pool.attr = attr;
    pool.memory_block = allocate("vpl:" + pool.name, (attr & kVplAttrHighMem) != 0u ? 1u : 0u, size, 0u);
    if (pool.memory_block < 0) return finish(ctx, kErrorNoMemory);
    pool.base = blocks_.at(pool.memory_block).address + kVplHeader;
    pool.pool_size = size - kVplHeader;
    const std::int32_t uid = new_uid();
    vpls_[uid] = std::move(pool);
    finish(ctx, static_cast<std::uint32_t>(uid));
}

void Kernel::sceKernelDeleteVpl(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = vpls_.find(uid);
    if (it == vpls_.end()) return finish(ctx, kErrorUnknownVpl);
    blocks_.erase(it->second.memory_block);
    vpls_.erase(it);
    for (auto &[tid, t] : threads_) {
        (void)tid;
        if (t.state == ThreadState::Waiting && t.wait == WaitType::Vpl && t.wait_uid == uid) wake(t, kErrorWaitDelete);
    }
    finish(ctx, 0u);
    maybe_preempt(ctx);
}

void Kernel::allocate_vpl(Ctx &ctx, bool wait) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t size = ctx.gpr[5], out = ctx.gpr[6];
    const auto it = vpls_.find(uid);
    if (it == vpls_.end()) return finish(ctx, kErrorUnknownVpl);
    VariablePool &pool = it->second;
    if (size == 0u || size > pool.pool_size) return finish(ctx, kErrorIllegalMemsize);
    // Waiters are served in order, so a new request may not overtake one already queued.
    if (count_waiters(WaitType::Vpl, uid) == 0u) {
        if (const std::uint32_t address = pool.allocate(size); address != 0u) {
            if (out != 0u) rt_.memory().store32(out, address);
            return finish(ctx, 0u);
        }
    }
    if (!wait) return finish(ctx, kErrorNoMemory);
    Thread *self = current();
    if (self != nullptr) {
        self->wait_uid = uid;
        self->wait_count = size;
        self->wait_out_address = out;
        begin_timeout(*self, ctx.gpr[7]);
    }
    block(ctx, WaitType::Vpl, 0u);
}

void Kernel::sceKernelAllocateVpl(Ctx &ctx) { allocate_vpl(ctx, true); }
void Kernel::sceKernelTryAllocateVpl(Ctx &ctx) { allocate_vpl(ctx, false); }

void Kernel::sceKernelFreeVpl(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = vpls_.find(uid);
    if (it == vpls_.end()) return finish(ctx, kErrorUnknownVpl);
    VariablePool &pool = it->second;
    if (!pool.free(ctx.gpr[5])) return finish(ctx, kErrorIllegalAddr);
    // Serve waiters by priority, stopping at the first one that still does not fit.
    std::vector<Thread *> waiters;
    for (auto &[tid, t] : threads_) {
        (void)tid;
        if (t.state == ThreadState::Waiting && t.wait == WaitType::Vpl && t.wait_uid == uid) waiters.push_back(&t);
    }
    std::stable_sort(waiters.begin(), waiters.end(), [](const Thread *a, const Thread *b) {
        return a->priority != b->priority ? a->priority < b->priority : a->ready_sequence < b->ready_sequence;
    });
    for (Thread *t : waiters) {
        const std::uint32_t address = pool.allocate(t->wait_count);
        if (address == 0u) break;
        if (t->wait_out_address != 0u) rt_.memory().store32(t->wait_out_address, address);
        wake(*t, 0u);
    }
    finish(ctx, 0u);
    maybe_preempt(ctx);
}

void Kernel::sceKernelReferVplStatus(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = vpls_.find(uid);
    if (it == vpls_.end()) return finish(ctx, kErrorUnknownVpl);
    auto &m = rt_.memory();
    const std::uint32_t info = ctx.gpr[5];
    if (!begin_refer(ctx, info, 0x34u)) return;
    write_object_name(info, it->second.name);
    m.store32(info + 0x24u, it->second.attr);
    m.store32(info + 0x28u, it->second.pool_size);
    m.store32(info + 0x2Cu, it->second.free_size());
    m.store32(info + 0x30u, count_waiters(WaitType::Vpl, uid));
    finish(ctx, 0u);
}

void Kernel::install_kernel_objects() {
    const char *tm = "ThreadManForUser";
    hle(tm, 0xBC6FEBC5u, "sceKernelReferSemaStatus", &Kernel::sceKernelReferSemaStatus);
    hle(tm, 0xA8E8C846u, "sceKernelReferMbxStatus", &Kernel::sceKernelReferMbxStatus);
    hle(tm, 0xD8199E4Cu, "sceKernelReferFplStatus", &Kernel::sceKernelReferFplStatus);
    hle(tm, 0xA9C2CB9Au, "sceKernelReferMutexStatus", &Kernel::sceKernelReferMutexStatus);
    hle(tm, 0x56C039B5u, "sceKernelCreateVpl", &Kernel::sceKernelCreateVpl);
    hle(tm, 0x89B3D48Cu, "sceKernelDeleteVpl", &Kernel::sceKernelDeleteVpl);
    hle(tm, 0xBED27435u, "sceKernelAllocateVpl", &Kernel::sceKernelAllocateVpl);
    hle(tm, 0xAF36D708u, "sceKernelTryAllocateVpl", &Kernel::sceKernelTryAllocateVpl);
    hle(tm, 0xB736E9FFu, "sceKernelFreeVpl", &Kernel::sceKernelFreeVpl);
    hle(tm, 0x39810265u, "sceKernelReferVplStatus", &Kernel::sceKernelReferVplStatus);
}

} // namespace pspweb
