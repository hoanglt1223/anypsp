// SPDX-License-Identifier: GPL-2.0-or-later
// HLE functions called directly through the runtime's import table, without a guest program.

#include "test_harness.hpp"

#include "kernel.hpp"

#include "psprecomp/runtime.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

constexpr std::uint32_t kScratch = 0x08800000u;
constexpr std::uint32_t kReturn = 0x08900000u;

struct Hle {
    psprecomp::Runtime rt;
    std::unique_ptr<pspweb::Kernel> kernel;

    Hle() : kernel(std::make_unique<pspweb::Kernel>(rt)) { kernel->install({}); }

    std::uint32_t call(const char *library, std::uint32_t nid, std::uint32_t a0 = 0u, std::uint32_t a1 = 0u,
                       std::uint32_t a2 = 0u, std::uint32_t a3 = 0u) {
        psprecomp::AllegrexContext ctx{};
        ctx.gpr[4] = a0;
        ctx.gpr[5] = a1;
        ctx.gpr[6] = a2;
        ctx.gpr[7] = a3;
        ctx.gpr[31] = kReturn;
        rt.invoke_import(library, nid, ctx);
        CHECK(ctx.pc == kReturn);
        return ctx.gpr[2];
    }

    void store_string(std::uint32_t address, const std::string &text) {
        for (std::size_t i = 0; i <= text.size(); ++i)
            rt.memory().store8(address + static_cast<std::uint32_t>(i), i < text.size() ? static_cast<std::uint8_t>(text[i]) : 0u);
    }
};

} // namespace

TEST_CASE("sceRtcTickAddSeconds adds signed whole seconds in microseconds") {
    Hle h;
    auto &m = h.rt.memory();
    m.store32(kScratch, 0x00000010u);
    m.store32(kScratch + 4u, 0x1u);
    CHECK(h.call("sceRtc", 0xF2A4AFE5u, kScratch + 8u, kScratch, 3u, 0u) == 0u);
    const std::uint64_t expected = 0x100000010ull + 3000000ull;
    CHECK(m.load32(kScratch + 8u) == static_cast<std::uint32_t>(expected));
    CHECK(m.load32(kScratch + 12u) == static_cast<std::uint32_t>(expected >> 32u));
    CHECK(h.call("sceRtc", 0xF2A4AFE5u, kScratch + 8u, kScratch, 0xFFFFFFFFu, 0xFFFFFFFFu) == 0u);
    CHECK(m.load32(kScratch + 8u) == 0x10u - 1000000u);
}

TEST_CASE("sceUtilityGetSystemParamString returns a truncated nickname and rejects other ids") {
    Hle h;
    auto &m = h.rt.memory();
    CHECK(h.call("sceUtility", 0x34B78343u, 1u, kScratch, 3u) == 0u);
    CHECK(m.load8(kScratch) == 'P' && m.load8(kScratch + 1u) == 'S' && m.load8(kScratch + 2u) == 0u);
    CHECK(h.call("sceUtility", 0x34B78343u, 2u, kScratch, 16u) == 0x80110103u);
}

TEST_CASE("sceKernelResumeThread reports unknown and not suspended threads") {
    Hle h;
    CHECK(h.call("ThreadManForUser", 0x75156E8Fu, 0x12345u) == 0x80020198u);
}

TEST_CASE("network modules behave as an offline PSP") {
    Hle h;
    auto &m = h.rt.memory();
    CHECK(h.call("sceWlanDrv", 0xD7763699u) == 0u);
    CHECK(h.call("sceNetAdhocctl", 0xE26F226Eu) == 0u);
    m.store32(kScratch, 0xFFu);
    CHECK(h.call("sceNetAdhocctl", 0x75ECD386u, kScratch) == 0u);
    CHECK(m.load32(kScratch) == 0u);
    CHECK(h.call("sceNetAdhocctl", 0x0AD043EDu, kScratch) == 0x80410B03u);
    m.store32(kScratch, 64u);
    CHECK(h.call("sceNetAdhocctl", 0xE162CB14u, kScratch, kScratch + 4u) == 0u);
    CHECK(m.load32(kScratch) == 0u);
    CHECK(h.call("sceNetAdhoc", 0x6F92741Bu) == 0x8041070Bu);
    CHECK(h.call("sceNetAdhocMatching", 0x93EF3843u, 1u) == 0x80410807u);
}

TEST_CASE("netconf dialog runs its status sequence and ends cancelled") {
    Hle h;
    auto &m = h.rt.memory();
    CHECK(h.call("sceUtility", 0x4DB1E739u, kScratch) == 0u);
    CHECK(m.load32(kScratch + 0x1Cu) == 1u);
    CHECK(h.call("sceUtility", 0x6332AA39u) == 1u);
    CHECK(h.call("sceUtility", 0x6332AA39u) == 2u);
    CHECK(h.call("sceUtility", 0x6332AA39u) == 3u);
    CHECK(h.call("sceUtility", 0xF88155F6u) == 0u);
    CHECK(h.call("sceUtility", 0x6332AA39u) == 4u);
    CHECK(h.call("sceUtility", 0x6332AA39u) == 0u);
}

TEST_CASE("__sceSasSetNoise validates voice and frequency") {
    Hle h;
    CHECK(h.call("sceSasCore", 0xB7660A23u, kScratch, 0u, 63u) == 0u);
    CHECK(h.call("sceSasCore", 0xB7660A23u, kScratch, 0u, 64u) == 0x80420011u);
    CHECK(h.call("sceSasCore", 0xB7660A23u, kScratch, 32u, 1u) == 0x80420010u);
}

TEST_CASE("__sceSasCoreWithMix scales the buffer by its volumes and adds the voices at full level") {
    Hle h;
    auto &m = h.rt.memory();
    constexpr std::uint32_t kOut = kScratch + 0x1000u, kGrain = 64u;
    auto fill = [&](std::int16_t value) {
        for (std::uint32_t i = 0; i < kGrain * 2u; ++i) m.store16(kOut + i * 2u, static_cast<std::uint16_t>(value));
    };
    auto at = [&](std::uint32_t i) { return static_cast<std::int16_t>(m.load16(kOut + i * 2u)); };
    CHECK(h.call("sceSasCore", 0x42778A9Fu, kScratch, kGrain, 32u, 0u) == 0u);
    fill(1000);
    CHECK(h.call("sceSasCore", 0x50A14DFCu, kScratch, kOut, 0x800u, 0x1000u) == 0u);
    CHECK(at(0u) == 500);
    CHECK(at(1u) == 1000);

    // A noise voice at full volume with an instant attack: volume 0 drops the old
    // buffer contents but keeps the voice.
    CHECK(h.call("sceSasCore", 0xB7660A23u, kScratch, 0u, 63u) == 0u);
    CHECK(h.call("sceSasCore", 0x440CA7D8u, kScratch, 0u, 0x1000u, 0x1000u) == 0u);
    CHECK(h.call("sceSasCore", 0xCBCD4F79u, kScratch, 0u, 0x000Fu, 0x1FC0u) == 0u);
    CHECK(h.call("sceSasCore", 0x76F01ACAu, kScratch, 0u) == 0u);
    fill(1000);
    CHECK(h.call("sceSasCore", 0x50A14DFCu, kScratch, kOut, 0u, 0u) == 0u);
    bool voice_heard = false, old_kept = true;
    for (std::uint32_t i = kGrain; i < kGrain * 2u; ++i) {
        voice_heard = voice_heard || at(i) != 0;
        old_kept = old_kept && at(i) == 1000;
    }
    CHECK(voice_heard);
    CHECK(!old_kept);
}

TEST_CASE("sceIoRename moves memory stick files and refuses existing targets") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "anypsp-hle-rename";
    fs::remove_all(root);
    fs::create_directories(root / "SAVE");
    std::ofstream(root / "SAVE" / "A.BIN") << "a";
    std::ofstream(root / "SAVE" / "C.BIN") << "c";
    Hle h;
    h.kernel->set_roots(root / "disc", root);
    h.store_string(kScratch, "ms0:/SAVE/A.BIN");
    h.store_string(kScratch + 0x100u, "B.BIN");
    CHECK(h.call("IoFileMgrForUser", 0x779103A0u, kScratch, kScratch + 0x100u) == 0u);
    CHECK(fs::exists(root / "SAVE" / "B.BIN") && !fs::exists(root / "SAVE" / "A.BIN"));
    h.store_string(kScratch, "ms0:/SAVE/B.BIN");
    h.store_string(kScratch + 0x100u, "ms0:/SAVE/C.BIN");
    CHECK(h.call("IoFileMgrForUser", 0x779103A0u, kScratch, kScratch + 0x100u) == 0x80010011u);
    h.store_string(kScratch, "ms0:/SAVE/MISSING.BIN");
    CHECK(h.call("IoFileMgrForUser", 0x779103A0u, kScratch, kScratch + 0x100u) == 0x80010002u);
    h.store_string(kScratch, "ms0:/SAVE/B.BIN");
    h.store_string(kScratch + 0x100u, "disc0:/B.BIN");
    CHECK(h.call("IoFileMgrForUser", 0x779103A0u, kScratch, kScratch + 0x100u) == 0x80020322u);
    fs::remove_all(root);
}

namespace {

constexpr const char *kCtrl = "sceCtrl";
constexpr std::uint32_t kPeekPositive = 0x3A622550u, kPeekNegative = 0xC152080Au, kReadPositive = 0x1F803938u,
                        kReadNegative = 0x60B81F86u, kPeekLatch = 0xB1D0E5CDu, kReadLatch = 0x0B588501u;

void sample(Hle &h, std::uint32_t buttons, std::uint8_t lx = 128u, std::uint8_t ly = 128u) {
    h.kernel->set_pad(buttons, lx, ly);
    h.kernel->sample_pad();
}

std::uint32_t buttons_at(Hle &h, std::uint32_t index) { return h.rt.memory().load32(kScratch + index * 16u + 4u); }

} // namespace

TEST_CASE("sceCtrlReadBuffer returns the newest unread samples in order") {
    Hle h;
    using namespace pspweb::pad;
    sample(h, kUp);
    sample(h, kUp | kCircle);
    sample(h, kCross);
    CHECK(h.call(kCtrl, kReadPositive, kScratch, 2u) == 2u);
    CHECK(buttons_at(h, 0u) == (kUp | kCircle));
    CHECK(buttons_at(h, 1u) == kCross);
    sample(h, kSquare);
    CHECK(h.call(kCtrl, kReadPositive, kScratch, 8u) == 1u);
    CHECK(buttons_at(h, 0u) == kSquare);
}

TEST_CASE("sceCtrlPeekBuffer returns the newest samples without consuming them") {
    Hle h;
    using namespace pspweb::pad;
    sample(h, kUp);
    sample(h, kCircle);
    CHECK(h.call(kCtrl, kPeekPositive, kScratch, 2u) == 2u);
    CHECK(buttons_at(h, 0u) == kUp);
    CHECK(buttons_at(h, 1u) == kCircle);
    CHECK(h.call(kCtrl, kPeekPositive, kScratch, 1u) == 1u);
    CHECK(buttons_at(h, 0u) == kCircle);
    CHECK(h.call(kCtrl, kReadPositive, kScratch, 4u) == 2u);
}

TEST_CASE("sceCtrl negative reads invert the buttons") {
    Hle h;
    using namespace pspweb::pad;
    sample(h, kCross);
    CHECK(h.call(kCtrl, kPeekNegative, kScratch, 1u) == 1u);
    CHECK(buttons_at(h, 0u) == ~kCross);
    CHECK(h.call(kCtrl, kReadNegative, kScratch, 1u) == 1u);
    CHECK(buttons_at(h, 0u) == ~kCross);
}

TEST_CASE("sceCtrl buffers reject more than 64 samples and keep at most 63 unread") {
    Hle h;
    CHECK(h.call(kCtrl, kPeekPositive, kScratch, 65u) == 0x80000104u);
    CHECK(h.call(kCtrl, kReadPositive, kScratch, 65u) == 0x80000104u);
    for (std::uint32_t i = 0; i < 100u; ++i) sample(h, i);
    CHECK(h.call(kCtrl, kReadPositive, kScratch, 64u) == 63u);
    CHECK(buttons_at(h, 0u) == 37u);
    CHECK(buttons_at(h, 62u) == 99u);
}

TEST_CASE("sceCtrl latch tracks make, break, press, release and the sample count") {
    Hle h;
    using namespace pspweb::pad;
    auto &m = h.rt.memory();
    sample(h, kCross);
    sample(h, kCross | kCircle);
    sample(h, kCircle);
    CHECK(h.call(kCtrl, kPeekLatch, kScratch) == 3u);
    CHECK(m.load32(kScratch) == (kCross | kCircle));
    CHECK(m.load32(kScratch + 4u) == kCross);
    CHECK(m.load32(kScratch + 8u) == (kCross | kCircle));
    CHECK(m.load32(kScratch + 12u) == ~0u);
    CHECK(h.call(kCtrl, kReadLatch, kScratch) == 3u);
    CHECK(h.call(kCtrl, kReadLatch, kScratch) == 0u);
    CHECK(m.load32(kScratch) == 0u);
    sample(h, kCircle);
    CHECK(h.call(kCtrl, kReadLatch, kScratch) == 1u);
    CHECK(m.load32(kScratch) == 0u);
    CHECK(m.load32(kScratch + 8u) == kCircle);
    CHECK(m.load32(kScratch + 12u) == ~kCircle);
}

TEST_CASE("sceCtrl sampling mode, cycle and idle threshold are validated") {
    Hle h;
    auto &m = h.rt.memory();
    sample(h, 0u, 10u, 250u);
    CHECK(h.call(kCtrl, kPeekPositive, kScratch, 1u) == 1u);
    CHECK(m.load8(kScratch + 8u) == 128u);
    CHECK(h.call(kCtrl, 0x1F4011E6u, 1u) == 0u);
    CHECK(h.call(kCtrl, 0x1F4011E6u, 2u) == 0x800001FEu);
    sample(h, 0u, 10u, 250u);
    CHECK(h.call(kCtrl, kPeekPositive, kScratch, 1u) == 1u);
    CHECK(m.load8(kScratch + 8u) == 10u);
    CHECK(m.load8(kScratch + 9u) == 250u);

    CHECK(h.call(kCtrl, 0x6A2774F3u, 100u) == 0x800001FEu);
    CHECK(h.call(kCtrl, 0x6A2774F3u, 16666u) == 0u);
    CHECK(h.call(kCtrl, 0x6A2774F3u, 0u) == 16666u);
    CHECK(h.call(kCtrl, 0x02BAAD91u, kScratch) == 0u);
    CHECK(m.load32(kScratch) == 0u);

    CHECK(h.call(kCtrl, 0xA7144800u, 129u, 0u) == 0x800001FEu);
    CHECK(h.call(kCtrl, 0xA7144800u, 40u, 0xFFFFFFFFu) == 0u);
    CHECK(h.call(kCtrl, 0x687660FAu, kScratch, kScratch + 4u) == 0u);
    CHECK(m.load32(kScratch) == 40u);
    CHECK(m.load32(kScratch + 4u) == 0xFFFFFFFFu);
}
