// Device-side HLE for the web profile: controller, audio channels, UMD,
// system utility dialogs and the remaining kernel queries titles poll during
// boot.  Dialogs complete immediately (no on-screen UI yet).

#include "kernel.hpp"

#include <cstring>

#include <algorithm>
#include <bit>
#include <chrono>
#include <ctime>
#include <iostream>

namespace pspweb {
namespace {

constexpr std::uint32_t kErrorNoMemory = 0x80020190u;
constexpr std::uint32_t kErrorUnknownUid = 0x800200CBu;
constexpr std::uint32_t kErrorUnknownThread = 0x80020198u;
constexpr std::uint32_t kErrorUnknownEventFlag = 0x8002019Au;
constexpr std::uint32_t kErrorAudioChannelNotReserved = 0x80260008u;
constexpr std::uint32_t kErrorAudioNoChannel = 0x80260002u;
constexpr std::uint32_t kErrorAudioChannelBusy = 0x80260002u;
constexpr std::uint32_t kErrorInvalidSystemParam = 0x80110103u;
constexpr std::uint32_t kErrorUnknownFpl = 0x8002019Du;
constexpr std::uint32_t kErrorIllegalMemblock = 0x800201B6u;
constexpr std::uint32_t kAudioRate = 44100u;
constexpr std::uint32_t kErrorInvalidSize = 0x80000104u;
constexpr std::uint32_t kErrorInvalidValue = 0x800001FEu;

std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

} // namespace

void Kernel::set_pad(std::uint32_t buttons, std::uint8_t lx, std::uint8_t ly) {
    pad_buttons_ = buttons;
    pad_lx_ = lx;
    pad_ly_ = ly;
}

void Kernel::sample_pad() {
    const std::uint32_t buttons = pad_buttons_;
    const std::uint32_t changed = buttons ^ pad_sampled_buttons_;
    latch_make_ |= buttons & changed;
    latch_break_ |= pad_sampled_buttons_ & changed;
    latch_press_ |= buttons;
    latch_release_ |= ~buttons;
    ++latch_samples_;
    pad_sampled_buttons_ = buttons;

    // Digital mode reports a centred stick.
    const bool analog = pad_mode_ != 0u;
    pad_ring_[pad_written_ % kPadSamples] = {static_cast<std::uint32_t>(now_us()), buttons,
                                             analog ? pad_lx_ : std::uint8_t{128u}, analog ? pad_ly_ : std::uint8_t{128u}};
    ++pad_written_;
    // A full ring overwrites the oldest unread sample.
    if (pad_written_ - pad_read_ >= kPadSamples) pad_read_ = pad_written_ - (kPadSamples - 1u);
}

void Kernel::return_one(Ctx &ctx) { finish(ctx, 1u); }

void Kernel::install_devices() {
    const char *ctrl = "sceCtrl";
    hle(ctrl, 0x3A622550u, "sceCtrlPeekBufferPositive", &Kernel::sceCtrlPeekBufferPositive);
    hle(ctrl, 0xC152080Au, "sceCtrlPeekBufferNegative", &Kernel::sceCtrlPeekBufferNegative);
    hle(ctrl, 0x1F803938u, "sceCtrlReadBufferPositive", &Kernel::sceCtrlReadBufferPositive);
    hle(ctrl, 0x60B81F86u, "sceCtrlReadBufferNegative", &Kernel::sceCtrlReadBufferNegative);
    hle(ctrl, 0x0B588501u, "sceCtrlReadLatch", &Kernel::sceCtrlReadLatch);
    hle(ctrl, 0xB1D0E5CDu, "sceCtrlPeekLatch", &Kernel::sceCtrlPeekLatch);
    hle(ctrl, 0x1F4011E6u, "sceCtrlSetSamplingMode", &Kernel::sceCtrlSetSamplingMode);
    hle(ctrl, 0xDA6B76A1u, "sceCtrlGetSamplingMode", &Kernel::sceCtrlGetSamplingMode);
    hle(ctrl, 0x6A2774F3u, "sceCtrlSetSamplingCycle", &Kernel::sceCtrlSetSamplingCycle);
    hle(ctrl, 0x02BAAD91u, "sceCtrlGetSamplingCycle", &Kernel::sceCtrlGetSamplingCycle);
    hle(ctrl, 0xA7144800u, "sceCtrlSetIdleCancelThreshold", &Kernel::sceCtrlSetIdleCancelThreshold);
    hle(ctrl, 0x687660FAu, "sceCtrlGetIdleCancelThreshold", &Kernel::sceCtrlGetIdleCancelThreshold);

    const char *audio = "sceAudio";
    hle(audio, 0x5EC81C55u, "sceAudioChReserve", &Kernel::sceAudioChReserve);
    hle(audio, 0x6FC46853u, "sceAudioChRelease", &Kernel::sceAudioChRelease);
    hle(audio, 0x136CAF51u, "sceAudioOutputBlocking", &Kernel::sceAudioOutputBlocking);
    hle(audio, 0x13F592BCu, "sceAudioOutputPannedBlocking", &Kernel::sceAudioOutputPannedBlocking);
    hle(audio, 0xE2D56B2Du, "sceAudioOutputPanned", &Kernel::sceAudioOutputPanned);
    hle(audio, 0xCB2E439Eu, "sceAudioSetChannelDataLen", &Kernel::sceAudioSetChannelDataLen);
    hle(audio, 0xB011922Fu, "sceAudioGetChannelRestLength", &Kernel::sceAudioGetChannelRestLength);
    hle(audio, 0x647CEF33u, "sceAudioGetChannelRestLen", &Kernel::sceAudioGetChannelRestLength);
    hle(audio, 0x95FD0C2Du, "sceAudioChangeChannelConfig", &Kernel::return_zero);
    hle(audio, 0xB7E1D8E7u, "sceAudioChangeChannelVolume", &Kernel::return_zero);
    hle(audio, 0x01562BA3u, "sceAudioOutput2Reserve", &Kernel::sceAudioOutput2Reserve);
    hle(audio, 0x2D53F36Eu, "sceAudioOutput2OutputBlocking", &Kernel::sceAudioOutput2OutputBlocking);
    hle(audio, 0x43196845u, "sceAudioOutput2Release", &Kernel::return_zero);
    hle(audio, 0x63F2889Cu, "sceAudioOutput2ChangeLength", &Kernel::sceAudioOutput2ChangeLength);

    hle("sceUmdUser", 0x46EBB729u, "sceUmdCheckMedium", &Kernel::sceUmdCheckMedium);
    hle("sceUmdUser", 0x6B4A146Cu, "sceUmdGetDriveStat", &Kernel::sceUmdGetDriveStat);
    hle("sceUmdUser", 0xC6183D47u, "sceUmdActivate", &Kernel::return_zero);
    hle("sceUmdUser", 0x56202973u, "sceUmdWaitDriveStatWithTimer", &Kernel::return_zero);
    hle("sceUmdUser", 0x8EF08FCEu, "sceUmdWaitDriveStat", &Kernel::return_zero);
    hle("sceUmdUser", 0x4A9E5E29u, "sceUmdWaitDriveStatCB", &Kernel::return_zero);
    hle("sceUmdUser", 0x6AF9B50Au, "sceUmdCancelWaitDriveStat", &Kernel::return_zero);
    hle("sceImpose", 0x24FD7BCFu, "sceImposeGetLanguageMode", &Kernel::sceImposeGetLanguageMode);
    hle("sceImpose", 0x36AA6E91u, "sceImposeSetLanguageMode", &Kernel::return_zero);
    hle("sceDisplay", 0xDBA6C4C4u, "sceDisplayGetFramePerSec", &Kernel::sceDisplayGetFramePerSec);
    hle("UtilsForUser", 0x91E4F6A7u, "sceKernelLibcClock", &Kernel::sceKernelLibcClock);
    hle("Kernel_Library", 0xB55249D2u, "sceKernelIsCpuIntrEnable", &Kernel::sceKernelIsCpuIntrEnable);

    const char *util = "sceUtility";
    hle(util, 0xA5DA2406u, "sceUtilityGetSystemParamInt", &Kernel::sceUtilityGetSystemParamInt);
    hle(util, 0x34B78343u, "sceUtilityGetSystemParamString", &Kernel::sceUtilityGetSystemParamString);
    hle(util, 0x2AD8E239u, "sceUtilityMsgDialogInitStart", &Kernel::sceUtilityMsgDialogInitStart);
    hle(util, 0x9A1C91D7u, "sceUtilityMsgDialogGetStatus", &Kernel::sceUtilityMsgDialogGetStatus);
    hle(util, 0x95FC253Bu, "sceUtilityMsgDialogUpdate", &Kernel::return_zero);
    hle(util, 0x67AF3428u, "sceUtilityMsgDialogShutdownStart", &Kernel::sceUtilityMsgDialogShutdownStart);

    const char *tm = "ThreadManForUser";

    const char *suspend = "sceSuspendForUser";
    hle(suspend, 0x3E0271D3u, "sceKernelVolatileMemLock", &Kernel::sceKernelVolatileMemLock);
    hle(suspend, 0xA14F40B2u, "sceKernelVolatileMemTryLock", &Kernel::sceKernelVolatileMemLock);
    hle(suspend, 0xA569E425u, "sceKernelVolatileMemUnlock", &Kernel::return_zero);
    hle(suspend, 0xEADB1BD7u, "sceKernelPowerLock", &Kernel::return_zero);
    hle(suspend, 0x3AEE7261u, "sceKernelPowerUnlock", &Kernel::return_zero);
    hle(suspend, 0x090CCB3Fu, "sceKernelPowerTick", &Kernel::return_zero);
    // Memory stick callbacks and folders need no work: the memory stick is a host folder.
    hle("IoFileMgrForUser", 0x54F5FB11u, "sceIoDevctl", &Kernel::sceIoDevctl);
    hle("IoFileMgrForUser", 0x779103A0u, "sceIoRename", &Kernel::sceIoRename);
    hle("IoFileMgrForUser", 0xF27A9C51u, "sceIoRemove", &Kernel::sceIoRemove);
    hle("IoFileMgrForUser", 0x1117C65Fu, "sceIoRmdir", &Kernel::sceIoRmdir);
    // Firmware modules are all high-level emulated, so loading one is a no-op.
    hle("sceUtility", 0x2A2B3DE0u, "sceUtilityLoadModule", &Kernel::return_zero);
    hle("sceUtility", 0xE49BFE92u, "sceUtilityUnloadModule", &Kernel::return_zero);
    // Keys for downloadable content; disc files use sceIoIoctl instead.
    hle("scePspNpDrm_user", 0xA1336091u, "sceNpDrmSetLicenseeKey", &Kernel::return_zero);
    hle("scePspNpDrm_user", 0x08D98894u, "sceNpDrmEdataSetupKey", &Kernel::return_zero);
}

// The 4 MiB "volatile" block in the kernel partition, lent to games that ask.
void Kernel::sceKernelVolatileMemLock(Ctx &ctx) {
    if (ctx.gpr[5] != 0u) rt_.memory().store32(ctx.gpr[5], 0x08400000u);
    if (ctx.gpr[6] != 0u) rt_.memory().store32(ctx.gpr[6], 0x00400000u);
    finish(ctx, 0u);
}

// The "emulator:" device test programs use to report (pspautotests common.c).
// Other devices have nothing to configure here.
void Kernel::sceIoDevctl(Ctx &ctx) {
    auto &memory = rt_.memory();
    const std::string device = memory.read_c_string(ctx.gpr[4], 32u);
    if (device != "emulator:" && device != "kemulator:") {
        finish(ctx, 0u);
        return;
    }
    const std::uint32_t command = ctx.gpr[5], input = ctx.gpr[6], input_length = ctx.gpr[7], output = ctx.gpr[8];
    switch (command) {
    case 1u:  // has display: headless
        if (output != 0u && memory.contains(output, 4u)) memory.store32(output, 0u);
        break;
    case 2u:  // send output
        if (input_length != 0u && memory.contains(input, input_length)) {
            std::string text(input_length, '\0');
            memory.copy_out(input, std::span(reinterpret_cast<std::uint8_t *>(text.data()), text.size()));
            if (test_output_) test_output_(text);
        }
        break;
    case 3u:  // is emulator
        if (output != 0u && memory.contains(output, 4u)) memory.store32(output, 1u);
        break;
    case 0x20u:  // emit screenshot
        if (screenshot_sink_) screenshot_sink_();
        break;
    default:
        break;
    }
    finish(ctx, 0u);
}

// ---------------------------------------------------------------------------
// sceCtrl

void Kernel::sceCtrlPeekBufferPositive(Ctx &ctx) { peek_pad(ctx, false); }
void Kernel::sceCtrlPeekBufferNegative(Ctx &ctx) { peek_pad(ctx, true); }
void Kernel::sceCtrlReadBufferPositive(Ctx &ctx) { read_pad(ctx, false); }
void Kernel::sceCtrlReadBufferNegative(Ctx &ctx) { read_pad(ctx, true); }

void Kernel::peek_pad(Ctx &ctx, bool negative) {
    const std::uint32_t count = std::max(ctx.gpr[5], 1u);
    if (count > kPadSamples) return finish(ctx, kErrorInvalidSize);
    finish(ctx, copy_pad(ctx.gpr[4], count, negative, true));
}

// Returns the newest unread samples, or blocks until the next vblank sample.
void Kernel::read_pad(Ctx &ctx, bool negative) {
    const std::uint32_t count = std::max(ctx.gpr[5], 1u);
    if (count > kPadSamples) return finish(ctx, kErrorInvalidSize);
    if (const std::uint32_t copied = copy_pad(ctx.gpr[4], count, negative, false); copied != 0u)
        return finish(ctx, copied);
    Thread *t = current();
    t->wait_out_address = ctx.gpr[4];
    t->wait_count = count;
    t->wait_mode = negative ? 1u : 0u;
    block(ctx, WaitType::CtrlRead, 0u);
}

std::uint32_t Kernel::copy_pad(std::uint32_t data, std::uint32_t count, bool negative, bool peek) {
    const std::uint32_t copied =
        peek ? count : static_cast<std::uint32_t>(std::min<std::uint64_t>(count, pad_written_ - pad_read_));
    auto &m = rt_.memory();
    for (std::uint32_t i = 0; i < copied; ++i) {
        // Unsigned wrap keeps the index right before the ring has filled (2^64 is a multiple of 64).
        const PadSample &s = pad_ring_[(pad_written_ - copied + i) % kPadSamples];
        const std::uint32_t entry = data + i * 16u;
        m.store32(entry + 0u, s.timestamp);
        m.store32(entry + 4u, negative ? ~s.buttons : s.buttons);
        m.store8(entry + 8u, s.lx);
        m.store8(entry + 9u, s.ly);
        for (std::uint32_t j = 10u; j < 16u; ++j) m.store8(entry + j, 0u);
    }
    if (!peek) pad_read_ = pad_written_;
    return copied;
}

void Kernel::sceCtrlSetSamplingMode(Ctx &ctx) {
    if (ctx.gpr[4] > 1u) return finish(ctx, kErrorInvalidValue);
    const std::uint32_t previous = pad_mode_;
    pad_mode_ = ctx.gpr[4];
    finish(ctx, previous);
}

// Samples are always taken at vblank; the cycle is only validated and reported.
void Kernel::sceCtrlSetSamplingCycle(Ctx &ctx) {
    const std::uint32_t cycle = ctx.gpr[4];
    if ((cycle > 0u && cycle < 5555u) || cycle > 20000u) return finish(ctx, kErrorInvalidValue);
    const std::uint32_t previous = pad_cycle_;
    pad_cycle_ = cycle;
    finish(ctx, previous);
}

void Kernel::sceCtrlGetSamplingCycle(Ctx &ctx) {
    if (ctx.gpr[4] != 0u) rt_.memory().store32(ctx.gpr[4], pad_cycle_);
    finish(ctx, 0u);
}

void Kernel::sceCtrlSetIdleCancelThreshold(Ctx &ctx) {
    const auto reset = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto back = static_cast<std::int32_t>(ctx.gpr[5]);
    if (reset < -1 || reset > 128 || back < -1 || back > 128) return finish(ctx, kErrorInvalidValue);
    idle_reset_ = reset;
    idle_back_ = back;
    finish(ctx, 0u);
}

void Kernel::sceCtrlGetIdleCancelThreshold(Ctx &ctx) {
    if (ctx.gpr[4] != 0u) rt_.memory().store32(ctx.gpr[4], static_cast<std::uint32_t>(idle_reset_));
    if (ctx.gpr[5] != 0u) rt_.memory().store32(ctx.gpr[5], static_cast<std::uint32_t>(idle_back_));
    finish(ctx, 0u);
}

void Kernel::sceCtrlGetSamplingMode(Ctx &ctx) {
    if (ctx.gpr[4] != 0u) rt_.memory().store32(ctx.gpr[4], pad_mode_);
    finish(ctx, 0u);
}

// ---------------------------------------------------------------------------
// sceAudio.  Blocking output waits until the previous buffer of the channel
// has played, which paces audio threads at the real 44.1 kHz rate.

void Kernel::audio_output(Ctx &ctx, AudioChannel &channel, std::uint32_t buffer,
                          std::uint32_t lvol, std::uint32_t rvol, bool blocking) {
    const std::uint32_t frames = channel.samples;
    // A buffer starts when the channel's previous one ends, or now if it is idle.
    const std::uint64_t now = now_us();
    const std::uint64_t now_sample = now * kAudioRate / 1000000u;
    const std::uint64_t start = std::max(now_sample, channel.next_sample);
    if (!blocking && start > now_sample) {
        finish(ctx, kErrorAudioChannelBusy); // the previous buffer has not started playing yet
        return;
    }
    channel.next_sample = start + frames;
    channel.busy_until_us = channel.next_sample * 1000000u / kAudioRate;
    if (frames != 0u && buffer != 0u) {
        const bool mono = channel.format == 0x10u;
        const std::uint8_t *raw = rt_.memory().raw_pointer(buffer, frames * (mono ? 2u : 4u));
        if (raw != nullptr) {
            mix_out_.resize(static_cast<std::size_t>(frames) * 2u);
            for (std::uint32_t i = 0; i < frames; ++i) {
                std::int16_t left, right;
                std::memcpy(&left, raw + (mono ? i * 2u : i * 4u), 2u);
                if (mono) right = left;
                else std::memcpy(&right, raw + i * 4u + 2u, 2u);
                mix_out_[i * 2u] = left;
                mix_out_[i * 2u + 1u] = right;
            }
            mix_audio(start, mix_out_.data(), frames, static_cast<std::int32_t>(lvol & 0xFFFFu),
                      static_cast<std::int32_t>(rvol & 0xFFFFu));
        }
    }
    // Blocking output returns once the previous buffer has finished playing.
    const std::uint64_t start_us = start * 1000000u / kAudioRate;
    Thread *self = current();
    if (start_us <= now || self == nullptr) {
        finish(ctx, frames);
        return;
    }
    self->wake_time_us = start_us;
    self->wait_extra = 0u;
    block(ctx, WaitType::Delay, frames);
}

namespace {
constexpr std::uint64_t kMixRingFrames = 1u << 17; // ~3 s
} // namespace

void Kernel::mix_audio(std::uint64_t start_sample, const std::int16_t *stereo, std::uint32_t frames,
                       std::int32_t left_volume, std::int32_t right_volume) {
    if (mix_ring_.empty()) mix_ring_.assign(kMixRingFrames * 2u, 0);
    if (mix_read_ == 0u) mix_read_ = start_sample;
    for (std::uint32_t i = 0; i < frames; ++i) {
        const std::uint64_t at = start_sample + i;
        if (at < mix_read_ || at >= mix_read_ + kMixRingFrames) continue; // already played, or too far ahead
        const std::size_t slot = static_cast<std::size_t>(at % kMixRingFrames) * 2u;
        mix_ring_[slot] += stereo[i * 2u] * left_volume / 0x8000;
        mix_ring_[slot + 1u] += stereo[i * 2u + 1u] * right_volume / 0x8000;
    }
}

void Kernel::drain_audio(std::uint64_t until_us) {
    const std::uint64_t until = until_us * kAudioRate / 1000000u;
    if (mix_ring_.empty() || mix_read_ == 0u || until <= mix_read_) return;
    const auto frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(until - mix_read_, kMixRingFrames));
    mix_out_.resize(static_cast<std::size_t>(frames) * 2u);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const std::size_t slot = static_cast<std::size_t>((mix_read_ + i) % kMixRingFrames) * 2u;
        mix_out_[i * 2u] = static_cast<std::int16_t>(std::clamp(mix_ring_[slot], -32768, 32767));
        mix_out_[i * 2u + 1u] = static_cast<std::int16_t>(std::clamp(mix_ring_[slot + 1u], -32768, 32767));
        mix_ring_[slot] = 0;
        mix_ring_[slot + 1u] = 0;
    }
    mix_read_ = until;
    if (audio_sink_ != nullptr) audio_sink_(mix_out_.data(), frames);
}

void Kernel::sceAudioChReserve(Ctx &ctx) {
    std::int32_t channel = static_cast<std::int32_t>(ctx.gpr[4]);
    if (channel < 0) {
        for (std::size_t i = 0; i < audio_channels_.size(); ++i)
            if (!audio_channels_[i].reserved) {
                channel = static_cast<std::int32_t>(i);
                break;
            }
    }
    if (channel < 0 || channel >= static_cast<std::int32_t>(audio_channels_.size()) ||
        audio_channels_[static_cast<std::size_t>(channel)].reserved) {
        finish(ctx, kErrorAudioNoChannel);
        return;
    }
    auto &c = audio_channels_[static_cast<std::size_t>(channel)];
    c = AudioChannel{true, ctx.gpr[5], ctx.gpr[6], 0u};
    finish(ctx, static_cast<std::uint32_t>(channel));
}

void Kernel::sceAudioChRelease(Ctx &ctx) {
    const std::uint32_t channel = ctx.gpr[4];
    if (channel < audio_channels_.size()) audio_channels_[channel].reserved = false;
    finish(ctx, 0u);
}

void Kernel::sceAudioOutputBlocking(Ctx &ctx) {
    const std::uint32_t channel = ctx.gpr[4];
    if (channel >= audio_channels_.size() || !audio_channels_[channel].reserved) {
        finish(ctx, kErrorAudioChannelNotReserved);
        return;
    }
    audio_output(ctx, audio_channels_[channel], ctx.gpr[6], ctx.gpr[5], ctx.gpr[5]);
}

void Kernel::sceAudioOutputPannedBlocking(Ctx &ctx) {
    const std::uint32_t channel = ctx.gpr[4];
    if (channel >= audio_channels_.size() || !audio_channels_[channel].reserved) {
        finish(ctx, kErrorAudioChannelNotReserved);
        return;
    }
    audio_output(ctx, audio_channels_[channel], ctx.gpr[7], ctx.gpr[5], ctx.gpr[6]);
}

void Kernel::sceAudioOutputPanned(Ctx &ctx) {
    const std::uint32_t channel = ctx.gpr[4];
    if (channel >= audio_channels_.size() || !audio_channels_[channel].reserved) {
        finish(ctx, kErrorAudioChannelNotReserved);
        return;
    }
    audio_output(ctx, audio_channels_[channel], ctx.gpr[7], ctx.gpr[5], ctx.gpr[6], false);
}

void Kernel::sceAudioSetChannelDataLen(Ctx &ctx) {
    const std::uint32_t channel = ctx.gpr[4];
    if (channel < audio_channels_.size()) audio_channels_[channel].samples = ctx.gpr[5];
    finish(ctx, 0u);
}

void Kernel::sceAudioGetChannelRestLength(Ctx &ctx) {
    const std::uint32_t channel = ctx.gpr[4];
    if (channel >= audio_channels_.size()) {
        finish(ctx, kErrorAudioChannelNotReserved);
        return;
    }
    const std::uint64_t now_sample = now_us() * kAudioRate / 1000000u, next = audio_channels_[channel].next_sample;
    finish(ctx, next > now_sample ? static_cast<std::uint32_t>(next - now_sample) : 0u);
}

void Kernel::sceAudioOutput2Reserve(Ctx &ctx) {
    audio_output2_ = AudioChannel{true, ctx.gpr[4], 0u, 0u, 0u};
    finish(ctx, 0u);
}

void Kernel::sceAudioOutput2OutputBlocking(Ctx &ctx) {
    audio_output(ctx, audio_output2_, ctx.gpr[5], ctx.gpr[4], ctx.gpr[4]);
}

void Kernel::sceAudioOutput2ChangeLength(Ctx &ctx) {
    audio_output2_.samples = ctx.gpr[4];
    finish(ctx, 0u);
}

// ---------------------------------------------------------------------------
// UMD, impose, display, clocks

void Kernel::sceUmdCheckMedium(Ctx &ctx) { finish(ctx, 1u); }
void Kernel::sceUmdGetDriveStat(Ctx &ctx) { finish(ctx, 0x32u); } // present, ready, readable

void Kernel::sceUtilityGetSystemParamInt(Ctx &ctx) {
    std::uint32_t value = 0u;
    switch (ctx.gpr[4]) {
    case 2: case 3: case 4: case 5: case 6: case 7: break; // ad hoc channel, power save, date/time format, zone, DST
    case 8: value = 1u; break;  // language: English
    case 9: value = 1u; break;  // cross confirms
    default:
        finish(ctx, kErrorInvalidSystemParam);
        return;
    }
    if (ctx.gpr[5] != 0u) rt_.memory().store32(ctx.gpr[5], value);
    finish(ctx, 0u);
}

void Kernel::sceUtilityGetSystemParamString(Ctx &ctx) {
    if (ctx.gpr[4] != 1u) { // only the nickname is a string parameter
        finish(ctx, kErrorInvalidSystemParam);
        return;
    }
    const std::string_view nickname = "PSP";
    const std::uint32_t buffer = ctx.gpr[5], size = ctx.gpr[6];
    if (buffer != 0u && size != 0u) {
        const auto n = std::min<std::uint32_t>(static_cast<std::uint32_t>(nickname.size()), size - 1u);
        for (std::uint32_t i = 0; i < n; ++i) rt_.memory().store8(buffer + i, static_cast<std::uint8_t>(nickname[i]));
        rt_.memory().store8(buffer + n, 0u);
    }
    finish(ctx, 0u);
}

// Both return the number of samples taken since the last sceCtrlReadLatch.
void Kernel::sceCtrlPeekLatch(Ctx &ctx) {
    if (ctx.gpr[4] != 0u) {
        auto &m = rt_.memory();
        m.store32(ctx.gpr[4], latch_make_);
        m.store32(ctx.gpr[4] + 4u, latch_break_);
        m.store32(ctx.gpr[4] + 8u, latch_press_);
        m.store32(ctx.gpr[4] + 12u, latch_release_);
    }
    finish(ctx, latch_samples_);
}

void Kernel::sceCtrlReadLatch(Ctx &ctx) {
    sceCtrlPeekLatch(ctx);
    latch_make_ = latch_break_ = latch_press_ = latch_release_ = latch_samples_ = 0u;
}

void Kernel::sceImposeGetLanguageMode(Ctx &ctx) {
    if (ctx.gpr[4] != 0u) rt_.memory().store32(ctx.gpr[4], 1u); // English
    if (ctx.gpr[5] != 0u) rt_.memory().store32(ctx.gpr[5], 1u); // Cross confirms
    finish(ctx, 0u);
}

void Kernel::sceDisplayGetFramePerSec(Ctx &ctx) {
    ctx.fpr[0] = 59.9400599f;
    finish(ctx, std::bit_cast<std::uint32_t>(ctx.fpr[0]));
}

void Kernel::sceKernelLibcClock(Ctx &ctx) { finish(ctx, static_cast<std::uint32_t>(now_us())); }

// ---------------------------------------------------------------------------
// sceUtility dialogs: answered at once, or shown by the host when interactive.

void Kernel::dialog_get_status(Ctx &ctx, UtilityDialog &dialog) {
    const std::uint32_t status = dialog.status;
    if (dialog.status == 1u) dialog.status = 2u;
    else if (dialog.status == 2u && !dialog.waiting) dialog.status = 3u;
    else if (dialog.status == 4u) dialog.status = 0u;
    finish(ctx, status);
}

void Kernel::open_dialog(SystemDialog dialog, DialogOwner owner) {
    dialog_ = std::move(dialog);
    dialog_.serial = ++dialog_serial_;
    dialog_owner_ = owner;
    dialog_close_at_us_ = 0u;
}

void Kernel::dialog_move(int delta) {
    if (dialog_owner_ == DialogOwner::None || delta == 0) return;
    dialog_focus(std::clamp(dialog_.focus + delta, 0, dialog_choices() - 1));
}

void Kernel::dialog_focus(int index) {
    if (dialog_owner_ == DialogOwner::None) return;
    if (index >= 0 && index < dialog_choices()) dialog_.focus = index;
}

// What focus can point at: the saves of a list, the keys of a keyboard, else the buttons.
int Kernel::dialog_choices() const {
    if (dialog_.kind == SystemDialog::Kind::SaveList) return static_cast<int>(dialog_.items.size());
    if (dialog_.kind == SystemDialog::Kind::Keyboard) return kOskKeyCount;
    return dialog_.buttons == SystemDialog::Buttons::YesNo ? 2 : 1;
}

void Kernel::dialog_confirm() { dialog_answer(true); }
void Kernel::dialog_back() { dialog_answer(false); }

void Kernel::dialog_answer(bool confirmed) {
    switch (dialog_owner_) {
    case DialogOwner::Message: msg_dialog_answer(confirmed); break;
    case DialogOwner::Savedata: savedata_answer(confirmed); break;
    case DialogOwner::Keyboard: osk_answer(confirmed); break;
    case DialogOwner::None: break;
    }
}

void Kernel::dialog_tick() {
    if (dialog_owner_ != DialogOwner::None && dialog_close_at_us_ != 0u && now_us() >= dialog_close_at_us_) dialog_confirm();
}

namespace {
// pspUtilityMsgDialogParams: the common header (size at 0, buttonSwap at 8, result at 0x1C), then these.
constexpr std::uint32_t kMsgResult = 0x30u, kMsgType = 0x34u, kMsgErrorNum = 0x38u, kMsgText = 0x3Cu, kMsgOptions = 0x23Cu,
                        kMsgButtonPressed = 0x240u;
constexpr std::uint32_t kMsgSizeV2 = 580u, kMsgSizeV3 = 708u;
constexpr std::uint32_t kMsgOptionYesNo = 0x10u, kMsgOptionOk = 0x20u, kMsgOptionDefaultNo = 0x100u;
} // namespace

void Kernel::sceUtilityMsgDialogInitStart(Ctx &ctx) {
    const std::uint32_t params = ctx.gpr[4];
    auto &m = rt_.memory();
    std::cerr << "[utility] message dialog: \"" << read_string(params + kMsgText, 512) << "\"\n";
    if (!interactive_dialogs_) {
        m.store32(params + 0x1Cu, 0u);   // result
        m.store32(params + kMsgButtonPressed, 1u);  // YES
        msg_dialog_ = UtilityDialog{1u, params};
        finish(ctx, 0u);
        return;
    }
    SystemDialog d;
    if (m.load32(params + kMsgType) == 0u) {
        d.text_id = SystemDialog::Text::Error;
        d.error = m.load32(params + kMsgErrorNum);
    } else {
        d.text = read_string(params + kMsgText, 512);
    }
    const std::uint32_t options = m.load32(params + kMsgOptions);
    if ((options & kMsgOptionYesNo) != 0u) {
        d.buttons = SystemDialog::Buttons::YesNo;
        d.focus = (options & kMsgOptionDefaultNo) != 0u ? 1 : 0;
    }
    d.cross_confirms = m.load32(params + 8u) == 1u;
    msg_dialog_ = UtilityDialog{1u, params, true};
    open_dialog(std::move(d), DialogOwner::Message);
    finish(ctx, 0u);
}

// buttonPressed as the PSP reports it: Yes 1, No 2. Going back is 3 in the V3 request and in
// the V2 request with Yes/No, else 0; a plain message has no confirm button on the PSP, so
// confirming one reports what going back does, except an OK button (V3 option) reports 1.
void Kernel::msg_dialog_answer(bool confirmed) {
    auto &m = rt_.memory();
    const std::uint32_t p = msg_dialog_.params, size = m.load32(p), options = m.load32(p + kMsgOptions);
    const bool yes_no = dialog_.buttons == SystemDialog::Buttons::YesNo;
    std::uint32_t pressed = size == kMsgSizeV3 || (size == kMsgSizeV2 && yes_no) ? 3u : 0u;
    if (confirmed && yes_no) pressed = dialog_.focus == 0 ? 1u : 2u;
    else if (confirmed && size == kMsgSizeV3 && (options & kMsgOptionOk) != 0u) pressed = 1u;
    m.store32(p + 0x1Cu, 0u);
    m.store32(p + kMsgResult, 0u);
    if (size >= kMsgButtonPressed + 4u) m.store32(p + kMsgButtonPressed, pressed);
    std::cerr << "[utility] message dialog answered: button " << pressed << "\n";
    msg_dialog_.waiting = false;
    if (msg_dialog_.status == 2u) msg_dialog_.status = 3u;
    close_dialog();
}

void Kernel::sceUtilityMsgDialogGetStatus(Ctx &ctx) { dialog_get_status(ctx, msg_dialog_); }

void Kernel::sceUtilityMsgDialogShutdownStart(Ctx &ctx) {
    msg_dialog_.status = 4u;
    msg_dialog_.waiting = false;
    if (dialog_owner_ == DialogOwner::Message) close_dialog();
    finish(ctx, 0u);
}

// ---------------------------------------------------------------------------
// Fixed-size memory pools

// ---------------------------------------------------------------------------
// Status queries

} // namespace pspweb
