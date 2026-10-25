// SPDX-License-Identifier: GPL-2.0-or-later
//
// Virtual timers (sceKernel*VTimer*): stopwatches counting microseconds of system time while
// started, each with an optional handler that runs, like an interrupt, once the timer reaches its
// schedule. The handler returns the time to its next run, or 0 to stop. Semantics and error codes
// follow PPSSPP (Core/HLE/sceKernelVTimer.cpp, GPL-2.0-or-later).

#include "kernel.hpp"
#include "kernel_errors.hpp"

#include <algorithm>

namespace pspweb {
namespace {

// The soonest a handler runs after it is scheduled.
constexpr std::uint64_t kMinScheduleUs = 250u;
// Stack the handler's two 64-bit arguments take below its stack pointer.
constexpr std::uint32_t kHandlerStackSpace = 48u;

} // namespace

std::uint64_t Kernel::vtimer_time(const VTimer &timer) const {
    return timer.current + (timer.active ? now_us() - timer.base : 0u);
}

void Kernel::vtimer_schedule(VTimer &timer, std::uint64_t schedule) {
    timer.schedule = schedule;
    timer.fire_at = 0u;
    if (!timer.active || timer.handler == 0u) return;
    // The schedule counts in timer time, which started at `base` minus what had accumulated.
    const std::uint64_t now = now_us();
    const auto goal = static_cast<std::int64_t>(timer.base + std::max(schedule, kMinScheduleUs)) -
                      static_cast<std::int64_t>(timer.current);
    timer.fire_at = goal < static_cast<std::int64_t>(now + kMinScheduleUs) ? now + kMinScheduleUs
                                                                            : static_cast<std::uint64_t>(goal);
}

void Kernel::run_vtimers() {
    if (!interrupts_enabled_) return;
    const std::uint64_t now = now_us();
    for (auto &[uid, timer] : vtimers_) {
        if (timer.fire_at == 0u || now < timer.fire_at || timer.running != 0) continue;
        timer.fire_at = 0u;
        const std::int32_t id = uid;
        const std::int32_t thread_uid = start_handler(timer.handler, timer.gp, static_cast<std::uint32_t>(uid), 0u,
                                                      [this, id](std::uint32_t result) {
            const auto it = vtimers_.find(id);
            if (running_vtimer_ == id) running_vtimer_ = 0;
            if (it == vtimers_.end()) return 0u;
            it->second.running = 0;
            if (result == 0u) {
                it->second.handler = 0u;
                it->second.fire_at = 0u;
            } else {
                vtimer_schedule(it->second, it->second.schedule + result);
            }
            return 0u;
        });
        if (thread_uid == 0) return;
        Thread &t = threads_.at(thread_uid);
        auto &m = rt_.memory();
        const std::uint32_t args = t.ctx.gpr[29];
        t.ctx.gpr[29] -= kHandlerStackSpace;
        m.store32(args - 16u, static_cast<std::uint32_t>(timer.schedule));
        m.store32(args - 12u, static_cast<std::uint32_t>(timer.schedule >> 32u));
        const std::uint64_t time = vtimer_time(timer);
        m.store32(args - 8u, static_cast<std::uint32_t>(time));
        m.store32(args - 4u, static_cast<std::uint32_t>(time >> 32u));
        t.ctx.gpr[5] = args - 16u;
        t.ctx.gpr[6] = args - 8u;
        t.ctx.gpr[7] = timer.common;
        timer.running = thread_uid;
        running_vtimer_ = id;
    }
}

std::uint64_t Kernel::earliest_vtimer() const {
    std::uint64_t earliest = 0u;
    for (const auto &[uid, timer] : vtimers_) {
        (void)uid;
        if (timer.fire_at != 0u && timer.running == 0 && (earliest == 0u || timer.fire_at < earliest)) earliest = timer.fire_at;
    }
    return earliest;
}

void Kernel::sceKernelCreateVTimer(Ctx &ctx) {
    if (in_interrupt()) return finish(ctx, err::kIllegalContext);
    if (ctx.gpr[4] == 0u) return finish(ctx, err::kError);
    const std::int32_t uid = new_uid();
    VTimer timer;
    timer.name = read_string(ctx.gpr[4], 31);
    timer.gp = ctx.gpr[28];
    vtimers_[uid] = std::move(timer);
    finish(ctx, static_cast<std::uint32_t>(uid));
}

void Kernel::sceKernelDeleteVTimer(Ctx &ctx) {
    if (in_interrupt()) return finish(ctx, err::kIllegalContext);
    finish(ctx, vtimers_.erase(static_cast<std::int32_t>(ctx.gpr[4])) != 0u ? 0u : err::kUnknownVtid);
}

VTimer *Kernel::find_vtimer(std::int32_t uid) {
    const auto it = vtimers_.find(uid);
    return it == vtimers_.end() ? nullptr : &it->second;
}

void Kernel::sceKernelGetVTimerBase(Ctx &ctx) {
    VTimer *timer = find_vtimer(static_cast<std::int32_t>(ctx.gpr[4]));
    if (timer == nullptr) return finish(ctx, err::kUnknownVtid);
    store64(ctx.gpr[5], timer->base);
    finish(ctx, 0u);
}

void Kernel::sceKernelGetVTimerBaseWide(Ctx &ctx) {
    VTimer *timer = find_vtimer(static_cast<std::int32_t>(ctx.gpr[4]));
    finish64(ctx, timer == nullptr ? 0xFFFFFFFFFFFFFFFFull : timer->base);
}

void Kernel::sceKernelGetVTimerTime(Ctx &ctx) {
    VTimer *timer = find_vtimer(static_cast<std::int32_t>(ctx.gpr[4]));
    if (timer == nullptr) return finish(ctx, err::kUnknownVtid);
    store64(ctx.gpr[5], vtimer_time(*timer));
    finish(ctx, 0u);
}

void Kernel::sceKernelGetVTimerTimeWide(Ctx &ctx) {
    VTimer *timer = find_vtimer(static_cast<std::int32_t>(ctx.gpr[4]));
    finish64(ctx, timer == nullptr ? 0xFFFFFFFFFFFFFFFFull : vtimer_time(*timer));
}

// Sets the timer's time and returns what it was; the handler is rescheduled against the new time.
std::uint64_t Kernel::vtimer_set(VTimer &timer, std::uint64_t time) {
    const std::uint64_t previous = vtimer_time(timer);
    timer.current = time - (timer.active ? now_us() - timer.base : 0u);
    vtimer_schedule(timer, timer.schedule);
    return previous;
}

void Kernel::sceKernelSetVTimerTime(Ctx &ctx) {
    VTimer *timer = find_vtimer(static_cast<std::int32_t>(ctx.gpr[4]));
    if (timer == nullptr) return finish(ctx, err::kUnknownVtid);
    const std::uint32_t clock = ctx.gpr[5];
    auto &m = rt_.memory();
    if ((clock & 3u) == 0u && m.contains(clock, 8u)) {
        const std::uint64_t time = m.load32(clock) | static_cast<std::uint64_t>(m.load32(clock + 4u)) << 32u;
        store64(clock, vtimer_set(*timer, time));
    }
    finish(ctx, 0u);
}

void Kernel::sceKernelSetVTimerTimeWide(Ctx &ctx) {
    if (in_interrupt()) return finish64(ctx, 0xFFFFFFFFFFFFFFFFull);
    VTimer *timer = find_vtimer(static_cast<std::int32_t>(ctx.gpr[4]));
    if (timer == nullptr) return finish64(ctx, 0xFFFFFFFFFFFFFFFFull);
    // A 64-bit argument after a 32-bit one goes in a2:a3.
    finish64(ctx, vtimer_set(*timer, ctx.gpr[6] | static_cast<std::uint64_t>(ctx.gpr[7]) << 32u));
}

void Kernel::sceKernelStartVTimer(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (uid == running_vtimer_) return finish(ctx, err::kIllegalVtid);
    VTimer *timer = find_vtimer(uid);
    if (timer == nullptr) return finish(ctx, err::kUnknownVtid);
    if (timer->active) return finish(ctx, 1u);
    timer->active = true;
    timer->base = now_us();
    if (timer->handler != 0u) vtimer_schedule(*timer, timer->schedule);
    finish(ctx, 0u);
}

void Kernel::sceKernelStopVTimer(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (uid == running_vtimer_) return finish(ctx, err::kIllegalVtid);
    VTimer *timer = find_vtimer(uid);
    if (timer == nullptr) return finish(ctx, err::kUnknownVtid);
    if (!timer->active) return finish(ctx, 0u);
    timer->current = vtimer_time(*timer);
    timer->active = false;
    timer->base = 0u;
    finish(ctx, 1u);
}

void Kernel::set_vtimer_handler(Ctx &ctx, std::uint64_t schedule, std::uint32_t handler, std::uint32_t common) {
    VTimer *timer = find_vtimer(static_cast<std::int32_t>(ctx.gpr[4]));
    if (timer == nullptr) return finish(ctx, err::kUnknownVtid);
    timer->handler = handler;
    if (handler != 0u) {
        timer->common = common;
        timer->gp = ctx.gpr[28];
        vtimer_schedule(*timer, schedule);
    } else {
        vtimer_schedule(*timer, timer->schedule);
    }
    finish(ctx, 0u);
}

void Kernel::sceKernelSetVTimerHandler(Ctx &ctx) {
    if (static_cast<std::int32_t>(ctx.gpr[4]) == running_vtimer_) return finish(ctx, err::kIllegalVtid);
    auto &m = rt_.memory();
    const std::uint32_t at = ctx.gpr[5];
    const std::uint64_t schedule = m.contains(at, 8u) ? m.load32(at) | static_cast<std::uint64_t>(m.load32(at + 4u)) << 32u : 0u;
    set_vtimer_handler(ctx, schedule, ctx.gpr[6], ctx.gpr[7]);
}

void Kernel::sceKernelSetVTimerHandlerWide(Ctx &ctx) {
    if (static_cast<std::int32_t>(ctx.gpr[4]) == running_vtimer_) return finish(ctx, err::kIllegalVtid);
    // (uid, u64 schedule in a2:a3, handler in t0, common in t1)
    set_vtimer_handler(ctx, ctx.gpr[6] | static_cast<std::uint64_t>(ctx.gpr[7]) << 32u, ctx.gpr[8], ctx.gpr[9]);
}

void Kernel::sceKernelCancelVTimerHandler(Ctx &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (uid == running_vtimer_) return finish(ctx, err::kIllegalVtid);
    VTimer *timer = find_vtimer(uid);
    if (timer == nullptr) return finish(ctx, err::kUnknownVtid);
    timer->handler = 0u;
    timer->fire_at = 0u;
    finish(ctx, 0u);
}

// SceKernelVTimerInfo: size, name[32], active, base (8), current (8), schedule (8), handler, common.
void Kernel::sceKernelReferVTimerStatus(Ctx &ctx) {
    VTimer *timer = find_vtimer(static_cast<std::int32_t>(ctx.gpr[4]));
    if (timer == nullptr) return finish(ctx, err::kUnknownVtid);
    auto &m = rt_.memory();
    const std::uint32_t info = ctx.gpr[5];
    if ((info & 3u) != 0u || !m.contains(info, 4u)) return finish(ctx, 0u);
    std::uint8_t record[0x48]{};
    const auto put32 = [&record](std::size_t at, std::uint32_t value) {
        for (std::size_t i = 0; i < 4u; ++i) record[at + i] = static_cast<std::uint8_t>(value >> (8u * i));
    };
    const auto put64 = [&put32](std::size_t at, std::uint64_t value) {
        put32(at, static_cast<std::uint32_t>(value));
        put32(at + 4u, static_cast<std::uint32_t>(value >> 32u));
    };
    put32(0x00, 0x48u);
    for (std::size_t i = 0; i < 31u && i < timer->name.size(); ++i) record[4 + i] = static_cast<std::uint8_t>(timer->name[i]);
    put32(0x24, timer->active ? 1u : 0u);
    put64(0x28, timer->base);
    put64(0x30, vtimer_time(*timer));
    put64(0x38, timer->schedule);
    put32(0x40, timer->handler);
    put32(0x44, timer->common);
    const std::uint32_t length = std::min<std::uint32_t>(m.load32(info), sizeof record);
    if (length != 0u) m.copy_in(info, std::span<const std::uint8_t>(record, length));
    finish(ctx, 0u);
}

void Kernel::store64(std::uint32_t address, std::uint64_t value) {
    auto &m = rt_.memory();
    if ((address & 3u) != 0u || !m.contains(address, 8u)) return;
    m.store32(address, static_cast<std::uint32_t>(value));
    m.store32(address + 4u, static_cast<std::uint32_t>(value >> 32u));
}

void Kernel::install_vtimer() {
    const char *tm = "ThreadManForUser";
    hle(tm, 0x20FFF560u, "sceKernelCreateVTimer", &Kernel::sceKernelCreateVTimer);
    hle(tm, 0x328F9E52u, "sceKernelDeleteVTimer", &Kernel::sceKernelDeleteVTimer);
    hle(tm, 0xC68D9437u, "sceKernelStartVTimer", &Kernel::sceKernelStartVTimer);
    hle(tm, 0xD0AEEE87u, "sceKernelStopVTimer", &Kernel::sceKernelStopVTimer);
    hle(tm, 0xD2D615EFu, "sceKernelCancelVTimerHandler", &Kernel::sceKernelCancelVTimerHandler);
    hle(tm, 0xB3A59970u, "sceKernelGetVTimerBase", &Kernel::sceKernelGetVTimerBase);
    hle(tm, 0xB7C18B77u, "sceKernelGetVTimerBaseWide", &Kernel::sceKernelGetVTimerBaseWide);
    hle(tm, 0x034A921Fu, "sceKernelGetVTimerTime", &Kernel::sceKernelGetVTimerTime);
    hle(tm, 0xC0B3FFD2u, "sceKernelGetVTimerTimeWide", &Kernel::sceKernelGetVTimerTimeWide);
    hle(tm, 0x5F32BEAAu, "sceKernelReferVTimerStatus", &Kernel::sceKernelReferVTimerStatus);
    hle(tm, 0x542AD630u, "sceKernelSetVTimerTime", &Kernel::sceKernelSetVTimerTime);
    hle(tm, 0xFB6425C3u, "sceKernelSetVTimerTimeWide", &Kernel::sceKernelSetVTimerTimeWide);
    hle(tm, 0xD8B299AEu, "sceKernelSetVTimerHandler", &Kernel::sceKernelSetVTimerHandler);
    hle(tm, 0x53B00E9Au, "sceKernelSetVTimerHandlerWide", &Kernel::sceKernelSetVTimerHandlerWide);
}

} // namespace pspweb
