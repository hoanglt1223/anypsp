// SPDX-License-Identifier: GPL-2.0-or-later
//
// scePower: the console runs on AC power with a full battery, and power callbacks hear so as soon
// as they register. The clock calls keep and report the frequencies a game sets, in the PSP's
// steps; they do not change how fast guest code runs. Behaviour follows PPSSPP
// (Core/HLE/scePower.cpp, GPL-2.0-or-later).

#include "kernel.hpp"

#include <algorithm>

namespace pspweb {
namespace {

constexpr std::uint32_t kErrorTakenSlot = 0x80000020u;
constexpr std::uint32_t kErrorSlotsFull = 0x80000022u;
constexpr std::uint32_t kErrorPrivRequired = 0x80000023u;
constexpr std::uint32_t kErrorEmptySlot = 0x80000025u;
constexpr std::uint32_t kErrorInvalidCallback = 0x80000100u;
constexpr std::uint32_t kErrorInvalidSlot = 0x80000102u;
constexpr std::uint32_t kErrorInvalidValue = 0x800001FEu;
constexpr std::uint32_t kErrorInvalidMode = 0x80000107u;

// What a power callback is told: on AC power, battery present and full.
constexpr std::uint32_t kCallbackAcPower = 0x1000u, kCallbackBatteryExist = 0x80u, kCallbackBatteryFull = 0x64u;
constexpr int kUserSlots = 16, kAllSlots = 32;

std::uint32_t pll_hz(std::uint32_t mhz) {
    if (mhz <= 190u) return 190285721u;
    if (mhz <= 222u) return 222000000u;
    if (mhz <= 266u) return 266399994u;
    if (mhz <= 333u) return 333000000u;
    return mhz * 1000000u;
}

std::uint32_t bus_hz(std::uint32_t mhz) {
    if (mhz <= 95u) return 95142860u;
    if (mhz <= 111u) return 111000000u;
    if (mhz <= 133u) return 133199997u;
    if (mhz <= 166u) return 166500000u;
    return mhz * 1000000u;
}

// The CPU clock is the PLL divided in 511 steps, except 222 and 333 MHz which are exact.
std::uint32_t cpu_hz(std::uint32_t mhz, std::uint32_t pll) {
    if (pll >= 333000000u && mhz == 333u) return 333000000u;
    if (pll >= 222000000u && mhz == 222u) return 222000000u;
    const double target = mhz * 1000000.0, step = pll / 511.0;
    double freq = 0.0;
    while (freq + step < target) freq += step;
    return static_cast<std::uint32_t>(static_cast<float>(freq / 1000000.0f) * 1000000);
}

} // namespace

void Kernel::install_power() {
    const char *pw = "scePower";
    const auto value = [this, pw](std::uint32_t nid, const char *name, std::uint32_t result) { hle_value(pw, nid, name, result); };
    value(0x87440F5Eu, "scePowerIsPowerOnline", 1u);
    value(0x0AFD0D8Bu, "scePowerIsBatteryExist", 1u);
    value(0x1E490401u, "scePowerIsBatteryCharging", 0u);
    value(0xB4432BC8u, "scePowerGetBatteryChargingStatus", 0u);
    value(0xD3075926u, "scePowerIsLowBattery", 0u);
    value(0x78A1A796u, "scePowerIsSuspendRequired", 0u);
    value(0x2085D15Du, "scePowerGetBatteryLifePercent", 100u);
    value(0x8EFB3FA2u, "scePowerGetBatteryLifeTime", 0u); // 0: on AC power
    value(0x28E12023u, "scePowerGetBatteryTemp", 28u);
    value(0x483CE86Bu, "scePowerGetBatteryVolt", 4150u);
    value(0x862AE1A6u, "scePowerGetBatteryElec", 0u);
    value(0x94F5A53Fu, "scePowerGetBatteryRemainCapacity", 1800u);
    value(0xFD18A0FFu, "scePowerGetBatteryFullCapacity", 1800u);
    value(0xCB49F5CEu, "scePowerGetBatteryChargeCycle", 0u);
    value(0x27F3292Cu, "scePowerBatteryUpdateInfo", 0u);
    value(0xEFD3C963u, "scePowerTick", 0u);
    value(0xDB62C9CFu, "scePowerCancelRequest", 0u);
    value(0x2B7C7CF4u, "scePowerRequestStandby", 0u);
    value(0xAC32C9CCu, "scePowerRequestSuspend", 0u);
    value(0xA85880D0u, "scePowerCheckWlanCoexistenceClock", 0u);
    value(0x0074EF9Bu, "scePowerGetResumeCount", 0u);
    value(0xEDC13FE5u, "scePowerGetIdleTimer", 0u);
    value(0x7F30B3B1u, "scePowerIdleTimerEnable", 0u);
    value(0x972CE941u, "scePowerIdleTimerDisable", 0u);
    hle(pw, 0x04B7766Eu, "scePowerRegisterCallback", &Kernel::scePowerRegisterCallback);
    hle(pw, 0xDFA8BAF8u, "scePowerUnregisterCallback", &Kernel::scePowerUnregisterCallback);
    hle(pw, 0xDB9D28DDu, "scePowerUnregitserCallback", &Kernel::scePowerUnregisterCallback);
    hle(pw, 0xD6D016EFu, "scePowerLock", &Kernel::scePowerLock);
    hle(pw, 0xCA3D34C1u, "scePowerUnlock", &Kernel::scePowerLock);
    hle(pw, 0x737486F2u, "scePowerSetClockFrequency", &Kernel::scePowerSetClockFrequency);
    hle(pw, 0xEBD177D6u, "scePowerSetClockFrequency350", &Kernel::scePowerSetClockFrequency);
    hle(pw, 0x469989ADu, "scePowerSetClockFrequency630", &Kernel::scePowerSetClockFrequency);
    hle(pw, 0x843FBF43u, "scePowerSetCpuClockFrequency", &Kernel::scePowerSetCpuClockFrequency);
    hle(pw, 0xB8D7B3FBu, "scePowerSetBusClockFrequency", &Kernel::scePowerSetBusClockFrequency);
    hle(pw, 0xFEE03A2Fu, "scePowerGetCpuClockFrequency", &Kernel::scePowerGetCpuClockFrequencyInt);
    hle(pw, 0xFDB5BFE9u, "scePowerGetCpuClockFrequencyInt", &Kernel::scePowerGetCpuClockFrequencyInt);
    hle(pw, 0x478FE6F5u, "scePowerGetBusClockFrequency", &Kernel::scePowerGetBusClockFrequencyInt);
    hle(pw, 0xBD681969u, "scePowerGetBusClockFrequencyInt", &Kernel::scePowerGetBusClockFrequencyInt);
    hle(pw, 0x34F9C463u, "scePowerGetPllClockFrequencyInt", &Kernel::scePowerGetPllClockFrequencyInt);
    hle(pw, 0xB1A52C83u, "scePowerGetCpuClockFrequencyFloat", &Kernel::scePowerGetCpuClockFrequencyFloat);
    hle(pw, 0x9BADB3EBu, "scePowerGetBusClockFrequencyFloat", &Kernel::scePowerGetBusClockFrequencyFloat);
    hle(pw, 0xEA382A27u, "scePowerGetPllClockFrequencyFloat", &Kernel::scePowerGetPllClockFrequencyFloat);
    hle(pw, 0x23C31FFEu, "scePowerVolatileMemLock", &Kernel::sceKernelVolatileMemLock);
    hle(pw, 0xFA97A599u, "scePowerVolatileMemTryLock", &Kernel::sceKernelVolatileMemLock);
    hle(pw, 0xB3EDD801u, "scePowerVolatileMemUnlock", &Kernel::return_zero);
}

void Kernel::scePowerRegisterCallback(Ctx &ctx) {
    const auto slot = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto callback = static_cast<std::int32_t>(ctx.gpr[5]);
    if (slot < -1 || slot >= kAllSlots) return finish(ctx, kErrorInvalidSlot);
    if (slot >= kUserSlots) return finish(ctx, kErrorPrivRequired);
    if (callback == 0) return finish(ctx, kErrorInvalidCallback);
    std::int32_t result = -1;
    if (slot == -1) {
        for (int i = 0; i < kUserSlots && result < 0; ++i)
            if (power_callbacks_[static_cast<std::size_t>(i)] == 0) result = i;
        if (result < 0) return finish(ctx, kErrorSlotsFull);
        power_callbacks_[static_cast<std::size_t>(result)] = callback;
    } else {
        if (power_callbacks_[static_cast<std::size_t>(slot)] != 0) return finish(ctx, kErrorTakenSlot);
        power_callbacks_[static_cast<std::size_t>(slot)] = callback;
        result = 0;
    }
    finish(ctx, static_cast<std::uint32_t>(result));
    // The callback hears the power state at once.
    notify_callback(ctx, callback, static_cast<std::int32_t>(kCallbackAcPower | kCallbackBatteryExist | kCallbackBatteryFull));
}

void Kernel::scePowerUnregisterCallback(Ctx &ctx) {
    const auto slot = static_cast<std::int32_t>(ctx.gpr[4]);
    if (slot < 0 || slot >= kAllSlots) return finish(ctx, kErrorInvalidSlot);
    if (slot >= kUserSlots) return finish(ctx, kErrorPrivRequired);
    if (power_callbacks_[static_cast<std::size_t>(slot)] == 0) return finish(ctx, kErrorEmptySlot);
    power_callbacks_[static_cast<std::size_t>(slot)] = 0;
    finish(ctx, 0u);
}

void Kernel::scePowerLock(Ctx &ctx) { finish(ctx, ctx.gpr[4] == 0u ? 0u : kErrorInvalidMode); }

void Kernel::scePowerSetClockFrequency(Ctx &ctx) {
    const std::uint32_t pll = ctx.gpr[4], cpu = ctx.gpr[5], bus = ctx.gpr[6];
    if (pll < 19u || pll < cpu || pll > 333u) return finish(ctx, kErrorInvalidValue);
    if (cpu == 0u || cpu > 333u) return finish(ctx, kErrorInvalidValue);
    if (bus == 0u || bus > 166u) return finish(ctx, kErrorInvalidValue);
    if (pll_hz_ != pll_hz(pll)) {
        pll_hz_ = pll_hz(pll);
        bus_hz_ = bus_hz(pll_hz_ / 2000000u);
    }
    cpu_hz_ = cpu_hz(cpu, pll_hz_);
    finish(ctx, 0u);
}

void Kernel::scePowerSetCpuClockFrequency(Ctx &ctx) {
    const std::uint32_t cpu = ctx.gpr[4];
    if (cpu == 0u || cpu > 333u || static_cast<std::uint64_t>(cpu) * 1000000u > pll_hz_) return finish(ctx, kErrorInvalidValue);
    cpu_hz_ = cpu_hz(std::min(cpu, pll_hz_ / 1000000u), pll_hz_);
    finish(ctx, 0u);
}

void Kernel::scePowerSetBusClockFrequency(Ctx &ctx) {
    const std::uint32_t bus = ctx.gpr[4];
    if (bus == 0u || bus > 111u) return finish(ctx, kErrorInvalidValue);
    // The value is checked but the PLL decides the result.
    const std::uint32_t pll = pll_hz_ / 1000000u;
    bus_hz_ = pll <= 190u ? 94956673u : pll <= 222u ? 111000000u : pll <= 266u ? 132939331u : pll <= 333u ? 165848343u : pll_hz_ / 2u;
    finish(ctx, 0u);
}

void Kernel::scePowerGetCpuClockFrequencyInt(Ctx &ctx) { finish(ctx, cpu_hz_ / 1000000u); }
void Kernel::scePowerGetBusClockFrequencyInt(Ctx &ctx) { finish(ctx, bus_hz_ / 1000000u); }
void Kernel::scePowerGetPllClockFrequencyInt(Ctx &ctx) { finish(ctx, pll_hz_ / 1000000u); }

void Kernel::finish_float(Ctx &ctx, float value) {
    ctx.fpr[0] = value;
    ctx.pc = ctx.gpr[31];
}

void Kernel::scePowerGetCpuClockFrequencyFloat(Ctx &ctx) { finish_float(ctx, static_cast<float>(cpu_hz_) / 1000000.0f); }
void Kernel::scePowerGetBusClockFrequencyFloat(Ctx &ctx) { finish_float(ctx, static_cast<float>(bus_hz_) / 1000000.0f); }
void Kernel::scePowerGetPllClockFrequencyFloat(Ctx &ctx) { finish_float(ctx, static_cast<float>(pll_hz_) / 1000000.0f); }

} // namespace pspweb
