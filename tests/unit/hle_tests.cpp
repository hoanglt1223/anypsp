// SPDX-License-Identifier: GPL-2.0-or-later
// HLE functions called directly through the runtime's import table, without a guest program.

#include "test_harness.hpp"

#include "kernel.hpp"

#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kScratch = 0x08800000u;
constexpr std::uint32_t kReturn = 0x08900000u;

struct Hle {
    psprecomp::Runtime rt;
    std::unique_ptr<pspweb::Kernel> kernel;

    Hle() : kernel(std::make_unique<pspweb::Kernel>(rt)) { kernel->install({}); }

    std::uint32_t call(const char *library, std::uint32_t nid, std::uint32_t a0 = 0u, std::uint32_t a1 = 0u,
                       std::uint32_t a2 = 0u, std::uint32_t a3 = 0u, std::uint32_t t0 = 0u, std::uint32_t t1 = 0u,
                       std::uint32_t t2 = 0u) {
        psprecomp::AllegrexContext ctx{};
        ctx.gpr[4] = a0;
        ctx.gpr[5] = a1;
        ctx.gpr[6] = a2;
        ctx.gpr[7] = a3;
        ctx.gpr[8] = t0;
        ctx.gpr[9] = t1;
        ctx.gpr[10] = t2;
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

namespace {

constexpr const char *kUtility = "sceUtility";
constexpr std::uint32_t kMsgInit = 0x2AD8E239u, kMsgStatus = 0x9A1C91D7u, kMsgShutdown = 0x67AF3428u;
constexpr std::uint32_t kSaveInit = 0x50C4CD57u, kSaveStatus = 0x8874DBE0u, kSaveShutdown = 0x9790B33Cu;
constexpr std::uint32_t kParams = kScratch, kNames = kScratch + 0x1000u, kData = kScratch + 0x2000u;
using Dialog = pspweb::SystemDialog;

// A message dialog request of `size` bytes (572, 580 or 708) with the Western button layout.
void message_request(Hle &h, std::uint32_t size, std::uint32_t type, std::uint32_t options, const std::string &text,
                     std::uint32_t error = 0u) {
    auto &m = h.rt.memory();
    for (std::uint32_t i = 0; i < 0x300u; i += 4u) m.store32(kParams + i, 0u);
    m.store32(kParams, size);
    m.store32(kParams + 8u, 1u);
    m.store32(kParams + 0x34u, type);
    m.store32(kParams + 0x38u, error);
    h.store_string(kParams + 0x3Cu, text);
    m.store32(kParams + 0x23Cu, options);
    m.store32(kParams + 0x240u, 0xFFu);
}

// A savedata request for game ULUS10001 and DATA.BIN; `names` fills saveNameList.
void save_request(Hle &h, std::uint32_t mode, const std::string &name, const std::vector<std::string> &names = {}) {
    auto &m = h.rt.memory();
    for (std::uint32_t i = 0; i < 0x600u; i += 4u) m.store32(kParams + i, 0u);
    m.store32(kParams, 0x5F8u);
    m.store32(kParams + 8u, 1u);
    m.store32(kParams + 0x30u, mode);
    h.store_string(kParams + 0x3Cu, "ULUS10001");
    h.store_string(kParams + 0x4Cu, name);
    if (!names.empty()) {
        for (std::size_t i = 0; i <= names.size(); ++i)
            h.store_string(kNames + static_cast<std::uint32_t>(i) * 20u, i < names.size() ? names[i] : "");
        m.store32(kParams + 0x60u, kNames);
    }
    h.store_string(kParams + 0x64u, "DATA.BIN");
    m.store32(kParams + 0x74u, kData);
    m.store32(kParams + 0x78u, 16u);
    m.store32(kParams + 0x7Cu, 4u);
    h.store_string(kParams + 0x80u, "Test Quest");
    h.store_string(kParams + 0x100u, "Chapter 1");
}

std::filesystem::path memstick(Hle &h, const std::string &name) {
    const auto root = std::filesystem::temp_directory_path() / ("anypsp-hle-" + name);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    h.kernel->set_roots(root / "disc", root);
    return root;
}

} // namespace

TEST_CASE("message dialog without interactive dialogs answers Yes at once") {
    Hle h;
    message_request(h, 580u, 1u, 0x10u, "Save data?");
    CHECK(h.call(kUtility, kMsgInit, kParams) == 0u);
    CHECK(h.kernel->system_dialog() == nullptr);
    CHECK(h.rt.memory().load32(kParams + 0x240u) == 1u);
    CHECK(h.call(kUtility, kMsgStatus) == 1u);
    CHECK(h.call(kUtility, kMsgStatus) == 2u);
    CHECK(h.call(kUtility, kMsgStatus) == 3u);
}

TEST_CASE("interactive message dialog stays visible until answered and reports the button") {
    Hle h;
    auto &m = h.rt.memory();
    h.kernel->set_interactive_dialogs(true);
    // Yes/No starting on No; Yes chosen.
    message_request(h, 580u, 1u, 0x110u, "Save data?");
    CHECK(h.call(kUtility, kMsgInit, kParams) == 0u);
    const Dialog *d = h.kernel->system_dialog();
    CHECK(d != nullptr && d->kind == Dialog::Kind::Message && d->text == "Save data?");
    CHECK(d->buttons == Dialog::Buttons::YesNo && d->focus == 1 && d->cross_confirms);
    CHECK(h.call(kUtility, kMsgStatus) == 1u);
    CHECK(h.call(kUtility, kMsgStatus) == 2u);
    CHECK(h.call(kUtility, kMsgStatus) == 2u);
    h.kernel->dialog_move(-1);
    CHECK(h.kernel->system_dialog()->focus == 0);
    h.kernel->dialog_move(-1);
    CHECK(h.kernel->system_dialog()->focus == 0);
    h.kernel->dialog_confirm();
    CHECK(h.kernel->system_dialog() == nullptr);
    CHECK(m.load32(kParams + 0x240u) == 1u);
    CHECK(m.load32(kParams + 0x30u) == 0u && m.load32(kParams + 0x1Cu) == 0u);
    CHECK(h.call(kUtility, kMsgStatus) == 3u);
    CHECK(h.call(kUtility, kMsgShutdown) == 0u);
    CHECK(h.call(kUtility, kMsgStatus) == 4u);
    CHECK(h.call(kUtility, kMsgStatus) == 0u);

    // No, then going back.
    message_request(h, 580u, 1u, 0x10u, "Quit?");
    h.call(kUtility, kMsgInit, kParams);
    CHECK(h.kernel->system_dialog()->focus == 0);
    h.kernel->dialog_focus(1);
    h.kernel->dialog_confirm();
    CHECK(m.load32(kParams + 0x240u) == 2u);
    message_request(h, 580u, 1u, 0x10u, "Quit?");
    h.call(kUtility, kMsgInit, kParams);
    h.kernel->dialog_back();
    CHECK(m.load32(kParams + 0x240u) == 3u);

    // An error code; a plain V2 message reports 0 however it closes, a V3 OK button 1.
    message_request(h, 580u, 0u, 0u, "", 0x80110307u);
    h.call(kUtility, kMsgInit, kParams);
    d = h.kernel->system_dialog();
    CHECK(d->text_id == Dialog::Text::Error && d->error == 0x80110307u && d->buttons == Dialog::Buttons::Ok);
    h.kernel->dialog_confirm();
    CHECK(m.load32(kParams + 0x240u) == 0u);
    message_request(h, 708u, 1u, 0x20u, "Done.");
    h.call(kUtility, kMsgInit, kParams);
    h.kernel->dialog_confirm();
    CHECK(m.load32(kParams + 0x240u) == 1u);
    // V1 requests end before buttonPressed.
    message_request(h, 572u, 1u, 0u, "Hello");
    h.call(kUtility, kMsgInit, kParams);
    h.kernel->dialog_back();
    CHECK(m.load32(kParams + 0x240u) == 0xFFu);
    // Shutting down closes a dialog the player has not answered.
    message_request(h, 580u, 1u, 0u, "Hello");
    h.call(kUtility, kMsgInit, kParams);
    h.call(kUtility, kMsgShutdown);
    CHECK(h.kernel->system_dialog() == nullptr);
}

TEST_CASE("savedata list load with no saves: no dialog when not interactive, else \"There is no data\"") {
    Hle h;
    auto &m = h.rt.memory();
    const auto root = memstick(h, "nodata");
    save_request(h, 4u, "");
    CHECK(h.call(kUtility, kSaveInit, kParams) == 0u);
    CHECK(h.kernel->system_dialog() == nullptr);
    CHECK(m.load32(kParams + 0x1Cu) == 0x80110307u);
    CHECK(h.call(kUtility, kSaveStatus) == 1u);
    CHECK(h.call(kUtility, kSaveStatus) == 2u);
    CHECK(h.call(kUtility, kSaveStatus) == 3u);
    CHECK(h.call(kUtility, kSaveShutdown) == 0u);
    CHECK(h.call(kUtility, kSaveStatus) == 4u);
    CHECK(h.call(kUtility, kSaveStatus) == 0u);

    h.kernel->set_interactive_dialogs(true);
    for (const std::uint32_t mode : {4u, 6u}) {
        save_request(h, mode, "");
        m.store32(kParams + 0x1Cu, 0xFFu);
        CHECK(h.call(kUtility, kSaveInit, kParams) == 0u);
        const Dialog *d = h.kernel->system_dialog();
        CHECK(d != nullptr && d->kind == Dialog::Kind::Message && d->text_id == Dialog::Text::NoData);
        CHECK(d->heading == (mode == 4u ? Dialog::Heading::Load : Dialog::Heading::Delete));
        CHECK(d->buttons == Dialog::Buttons::Ok);
        CHECK(h.call(kUtility, kSaveStatus) == 1u);
        CHECK(h.call(kUtility, kSaveStatus) == 2u);
        CHECK(h.call(kUtility, kSaveStatus) == 2u);
        CHECK(m.load32(kParams + 0x1Cu) == 0xFFu);
        h.kernel->dialog_confirm();
        CHECK(h.kernel->system_dialog() == nullptr);
        CHECK(m.load32(kParams + 0x1Cu) == (mode == 4u ? 0x80110307u : 0x80110347u));
        CHECK(h.call(kUtility, kSaveStatus) == 3u);
        h.call(kUtility, kSaveShutdown);
        CHECK(h.call(kUtility, kSaveStatus) == 4u);
        CHECK(h.call(kUtility, kSaveStatus) == 0u);
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("interactive savedata list: new data saves at once, overwriting asks, No goes back, then completed") {
    namespace fs = std::filesystem;
    Hle h;
    auto &m = h.rt.memory();
    const auto root = memstick(h, "listsave");
    h.kernel->set_interactive_dialogs(true);
    m.store32(kData, 0x44434241u);  // "ABCD"

    save_request(h, 5u, "0000", {"0000", "0001"});
    CHECK(h.call(kUtility, kSaveInit, kParams) == 0u);
    const Dialog *d = h.kernel->system_dialog();
    CHECK(d != nullptr && d->kind == Dialog::Kind::SaveList && d->heading == Dialog::Heading::Save);
    CHECK(d->items.size() == 2u && !d->items[0].exists && !d->items[1].exists && d->focus == 0);
    CHECK(h.call(kUtility, kSaveStatus) == 1u);
    h.kernel->dialog_move(1);
    h.kernel->dialog_move(1);
    CHECK(h.kernel->system_dialog()->focus == 1);
    h.kernel->dialog_confirm();
    d = h.kernel->system_dialog();
    CHECK(d != nullptr && d->text_id == Dialog::Text::SaveDone);
    CHECK(fs::exists(root / "PSP" / "SAVEDATA" / "ULUS100010001" / "DATA.BIN"));
    CHECK(h.call(kUtility, kSaveStatus) == 2u);
    h.kernel->dialog_confirm();
    CHECK(h.kernel->system_dialog() == nullptr);
    CHECK(m.load32(kParams + 0x1Cu) == 0u);
    CHECK(h.call(kUtility, kSaveStatus) == 3u);
    h.call(kUtility, kSaveShutdown);
    CHECK(h.call(kUtility, kSaveStatus) == 4u);
    CHECK(h.call(kUtility, kSaveStatus) == 0u);

    // Over the existing save: the question starts on No, which goes back to the list.
    save_request(h, 5u, "0000", {"0000", "0001"});
    h.call(kUtility, kSaveInit, kParams);
    d = h.kernel->system_dialog();
    CHECK(d->items[1].exists && d->items[1].title == "Test Quest" && d->items[1].savedata_title == "Chapter 1");
    h.kernel->dialog_focus(1);
    h.kernel->dialog_confirm();
    d = h.kernel->system_dialog();
    CHECK(d->kind == Dialog::Kind::Message && d->text_id == Dialog::Text::ConfirmOverwrite);
    CHECK(d->buttons == Dialog::Buttons::YesNo && d->focus == 1 && d->items.size() == 1u);
    h.kernel->dialog_confirm();
    d = h.kernel->system_dialog();
    CHECK(d->kind == Dialog::Kind::SaveList && d->focus == 1);
    h.kernel->dialog_confirm();
    h.kernel->dialog_move(-1);
    h.kernel->dialog_confirm();
    CHECK(h.kernel->system_dialog()->text_id == Dialog::Text::SaveDone);
    h.kernel->dialog_back();
    CHECK(m.load32(kParams + 0x1Cu) == 0u);
    CHECK(m.load32(kParams + 0x4Cu) == 0x31303030u);  // saveName = "0001"

    // Cancelling the list.
    h.call(kUtility, kSaveShutdown);
    h.call(kUtility, kSaveStatus);
    save_request(h, 4u, "");
    h.call(kUtility, kSaveInit, kParams);
    d = h.kernel->system_dialog();
    CHECK(d->kind == Dialog::Kind::SaveList && d->heading == Dialog::Heading::Load && d->items.size() == 1u);
    h.kernel->dialog_back();
    CHECK(h.kernel->system_dialog() == nullptr);
    CHECK(m.load32(kParams + 0x1Cu) == 1u);

    // Loading from the list: no question, then "Load completed." with the data in the buffer.
    save_request(h, 4u, "");
    m.store32(kData, 0u);
    h.call(kUtility, kSaveInit, kParams);
    h.kernel->dialog_confirm();
    CHECK(h.kernel->system_dialog()->text_id == Dialog::Text::LoadDone);
    CHECK(m.load32(kData) == 0x44434241u);
    h.kernel->dialog_confirm();
    CHECK(m.load32(kParams + 0x1Cu) == 0u);

    // Deleting from the list asks first, starting on No.
    save_request(h, 6u, "");
    h.call(kUtility, kSaveInit, kParams);
    h.kernel->dialog_confirm();
    d = h.kernel->system_dialog();
    CHECK(d->text_id == Dialog::Text::ConfirmDelete && d->focus == 1);
    h.kernel->dialog_move(-1);
    h.kernel->dialog_confirm();
    CHECK(h.kernel->system_dialog()->text_id == Dialog::Text::DeleteDone);
    CHECK(!fs::exists(root / "PSP" / "SAVEDATA" / "ULUS100010001"));
    h.kernel->dialog_confirm();
    CHECK(m.load32(kParams + 0x1Cu) == 0u);
    fs::remove_all(root);
}

TEST_CASE("interactive savedata load, save and delete of one slot confirm first") {
    namespace fs = std::filesystem;
    Hle h;
    auto &m = h.rt.memory();
    const auto root = memstick(h, "single");
    h.kernel->set_interactive_dialogs(true);

    // Loading a missing save: "There is no data."
    save_request(h, 2u, "0000");
    h.call(kUtility, kSaveInit, kParams);
    CHECK(h.kernel->system_dialog()->text_id == Dialog::Text::NoData);
    h.kernel->dialog_confirm();
    CHECK(m.load32(kParams + 0x1Cu) == 0x80110307u);

    // Saving: Yes first; No cancels.
    save_request(h, 3u, "0000");
    h.call(kUtility, kSaveInit, kParams);
    const Dialog *d = h.kernel->system_dialog();
    CHECK(d->text_id == Dialog::Text::ConfirmSave && d->focus == 0 && d->heading == Dialog::Heading::Save);
    h.kernel->dialog_move(1);
    h.kernel->dialog_confirm();
    CHECK(h.kernel->system_dialog() == nullptr && m.load32(kParams + 0x1Cu) == 1u);
    CHECK(!fs::exists(root / "PSP" / "SAVEDATA" / "ULUS100010000"));
    save_request(h, 3u, "0000");
    h.call(kUtility, kSaveInit, kParams);
    h.kernel->dialog_confirm();
    CHECK(h.kernel->system_dialog()->text_id == Dialog::Text::SaveDone);
    h.kernel->dialog_confirm();
    CHECK(m.load32(kParams + 0x1Cu) == 0u && fs::exists(root / "PSP" / "SAVEDATA" / "ULUS100010000" / "PARAM.SFO"));

    // Saving again asks to overwrite; loading asks to load.
    save_request(h, 3u, "0000");
    h.call(kUtility, kSaveInit, kParams);
    CHECK(h.kernel->system_dialog()->text_id == Dialog::Text::ConfirmOverwrite && h.kernel->system_dialog()->focus == 1);
    h.kernel->dialog_back();
    CHECK(m.load32(kParams + 0x1Cu) == 1u);
    save_request(h, 2u, "0000");
    h.call(kUtility, kSaveInit, kParams);
    CHECK(h.kernel->system_dialog()->text_id == Dialog::Text::ConfirmLoad);
    h.kernel->dialog_confirm();
    CHECK(h.kernel->system_dialog()->text_id == Dialog::Text::LoadDone);
    h.kernel->dialog_confirm();
    CHECK(m.load32(kParams + 0x1Cu) == 0u);

    // Auto modes stay silent.
    save_request(h, 0u, "0000");
    h.call(kUtility, kSaveInit, kParams);
    CHECK(h.kernel->system_dialog() == nullptr && m.load32(kParams + 0x1Cu) == 0u);

    // Deleting asks, starting on No.
    save_request(h, 10u, "0000");
    h.call(kUtility, kSaveInit, kParams);
    CHECK(h.kernel->system_dialog()->text_id == Dialog::Text::ConfirmDelete && h.kernel->system_dialog()->focus == 1);
    h.kernel->dialog_focus(0);
    h.kernel->dialog_confirm();
    CHECK(h.kernel->system_dialog()->text_id == Dialog::Text::DeleteDone);
    h.kernel->dialog_confirm();
    CHECK(m.load32(kParams + 0x1Cu) == 0u && !fs::exists(root / "PSP" / "SAVEDATA" / "ULUS100010000"));
    fs::remove_all(root);
}

TEST_CASE("interactive savedata completed message closes by itself after about a second") {
    Hle h;
    auto &m = h.rt.memory();
    const auto root = memstick(h, "autoclose");
    h.kernel->set_interactive_dialogs(true);
    save_request(h, 3u, "0000");
    h.call(kUtility, kSaveInit, kParams);
    CHECK(h.call(kUtility, kSaveStatus) == 1u);
    h.kernel->dialog_confirm();
    CHECK(h.kernel->system_dialog()->text_id == Dialog::Text::SaveDone);
    for (int vblank = 0; vblank < 30; ++vblank) h.kernel->frame(0.0);
    CHECK(h.call(kUtility, kSaveStatus) == 2u);
    CHECK(h.kernel->system_dialog() != nullptr);
    for (int vblank = 0; vblank < 32; ++vblank) h.kernel->frame(0.0);
    CHECK(h.call(kUtility, kSaveStatus) == 3u);
    CHECK(h.kernel->system_dialog() == nullptr && m.load32(kParams + 0x1Cu) == 0u);
    std::filesystem::remove_all(root);
}

TEST_CASE("interactive savedata list delete goes back to the list while saves remain") {
    namespace fs = std::filesystem;
    Hle h;
    auto &m = h.rt.memory();
    const auto root = memstick(h, "listdelete");
    for (const char *name : {"0000", "0001", "0002"}) {
        save_request(h, 1u, name);  // AUTOSAVE: no UI
        h.call(kUtility, kSaveInit, kParams);
    }
    h.kernel->set_interactive_dialogs(true);
    save_request(h, 6u, "");
    h.call(kUtility, kSaveInit, kParams);
    CHECK(h.call(kUtility, kSaveStatus) == 1u);
    CHECK(h.kernel->system_dialog()->items.size() == 3u);
    h.kernel->dialog_focus(2);
    h.kernel->dialog_confirm();
    h.kernel->dialog_focus(0);
    h.kernel->dialog_confirm();
    CHECK(h.kernel->system_dialog()->text_id == Dialog::Text::DeleteDone);
    CHECK(!fs::exists(root / "PSP" / "SAVEDATA" / "ULUS100010002"));
    m.store32(kParams + 0x1Cu, 0xFFu);
    h.kernel->dialog_confirm();
    // Back in the list with the two saves left, focus on the last one.
    const Dialog *d = h.kernel->system_dialog();
    CHECK(d != nullptr && d->kind == Dialog::Kind::SaveList && d->items.size() == 2u && d->focus == 1);
    CHECK(h.call(kUtility, kSaveStatus) == 2u);
    CHECK(m.load32(kParams + 0x1Cu) == 0xFFu);
    // Backing out reports the delete.
    h.kernel->dialog_back();
    CHECK(h.kernel->system_dialog() == nullptr && m.load32(kParams + 0x1Cu) == 0u);
    CHECK(h.call(kUtility, kSaveStatus) == 3u);
    h.call(kUtility, kSaveShutdown);
    h.call(kUtility, kSaveStatus);

    // Deleting the last two: the dialog closes when the list is empty.
    save_request(h, 6u, "");
    h.call(kUtility, kSaveInit, kParams);
    for (int left = 2; left > 0; --left) {
        h.kernel->dialog_confirm();
        h.kernel->dialog_move(-1);
        h.kernel->dialog_confirm();
        CHECK(h.kernel->system_dialog()->text_id == Dialog::Text::DeleteDone);
        h.kernel->dialog_confirm();
    }
    CHECK(h.kernel->system_dialog() == nullptr && m.load32(kParams + 0x1Cu) == 0u);
    CHECK(!fs::exists(root / "PSP" / "SAVEDATA" / "ULUS100010000") && !fs::exists(root / "PSP" / "SAVEDATA" / "ULUS100010001"));

    // Backing out without deleting is a cancel.
    for (const char *name : {"0000"}) {
        save_request(h, 1u, name);
        h.call(kUtility, kSaveInit, kParams);
    }
    save_request(h, 6u, "");
    h.call(kUtility, kSaveInit, kParams);
    h.kernel->dialog_back();
    CHECK(m.load32(kParams + 0x1Cu) == 1u);
    fs::remove_all(root);
}

extern const unsigned char anypsp_font_sans_jp[], anypsp_font_sans_kr[];
extern const std::size_t anypsp_font_sans_jp_size, anypsp_font_sans_kr_size;

namespace {

// sceLibFont through the HLE table, with flash0/font in a temporary folder.
struct FontLib {
    static constexpr const char *kLib = "sceLibFont";
    static constexpr std::uint32_t kError = kScratch + 0x10u, kStyle = kScratch + 0x100u, kInfo = kScratch + 0x200u,
                                   kChar = kScratch + 0x400u, kImage = kScratch + 0x500u, kPixels = kScratch + 0x1000u;
    Hle h;
    std::uint32_t lib{};

    explicit FontLib(const std::filesystem::path &flash, const std::filesystem::path &user = {}) {
        h.kernel->set_flash_root(flash);
        h.kernel->set_user_font_dir(user);
        lib = h.call(kLib, 0x67F17ED7u, kScratch, kError);
        CHECK(lib != 0u);
    }

    std::uint32_t find(std::uint16_t family, std::uint16_t style, std::uint16_t language) {
        auto &m = h.rt.memory();
        for (std::uint32_t i = 0; i < 168u; i += 4u) m.store32(kStyle + i, 0u);
        m.store32(kStyle, std::bit_cast<std::uint32_t>(15.0f));
        m.store32(kStyle + 4u, std::bit_cast<std::uint32_t>(15.0f));
        m.store16(kStyle + 20u, family);
        m.store16(kStyle + 22u, style);
        m.store16(kStyle + 26u, language);
        const std::uint32_t index = h.call(kLib, 0x099EF33Cu, lib, kStyle, kError);
        CHECK_EQ(m.load32(kError), 0u);
        return index;
    }

    std::uint32_t open(std::uint32_t index) {
        const std::uint32_t font = h.call(kLib, 0xA834319Du, lib, index, 0u, kError);
        CHECK_EQ(h.rt.memory().load32(kError), 0u);
        return font;
    }

    std::string file_of(std::uint32_t index) {
        CHECK_EQ(h.call(kLib, 0xBC75D85Bu, lib, kStyle, index + 1u), 0u);
        std::string name;
        for (std::uint32_t at = kStyle + index * 168u + 96u; char c = static_cast<char>(h.rt.memory().load8(at)); ++at) name += c;
        return name;
    }

    std::int32_t char_info(std::uint32_t font, std::uint32_t code, std::uint32_t field) {
        CHECK_EQ(h.call(kLib, 0xDCC80C2Fu, font, code, kChar), 0u);
        return static_cast<std::int32_t>(h.rt.memory().load32(kChar + field * 4u));
    }

    // Draws `code` at the origin of a cleared 32x32 image and returns its pixels (0-255).
    std::vector<std::uint8_t> draw(std::uint32_t font, std::uint32_t code, std::uint32_t format, std::int32_t clip_w = -1) {
        auto &m = h.rt.memory();
        constexpr std::uint32_t kSize = 32u;
        const std::uint32_t stride = format == 0u ? kSize / 2u : kSize;
        for (std::uint32_t i = 0; i < stride * kSize; ++i) m.store8(kPixels + i, 0u);
        m.store32(kImage, format);
        m.store32(kImage + 4u, 0u);
        m.store32(kImage + 8u, 0u);
        m.store16(kImage + 12u, kSize);
        m.store16(kImage + 14u, kSize);
        m.store16(kImage + 16u, static_cast<std::uint16_t>(stride));
        m.store32(kImage + 20u, kPixels);
        if (clip_w < 0) CHECK_EQ(h.call(kLib, 0x980F4895u, font, code, kImage), 0u);
        else CHECK_EQ(h.call(kLib, 0xCA1E6945u, font, code, kImage, 0u, 0u, static_cast<std::uint32_t>(clip_w), kSize), 0u);
        std::vector<std::uint8_t> pixels(kSize * kSize);
        for (std::uint32_t y = 0; y < kSize; ++y)
            for (std::uint32_t x = 0; x < kSize; ++x) {
                const std::uint8_t byte = m.load8(kPixels + y * stride + (format == 0u ? x / 2u : x));
                pixels[y * kSize + x] = format == 0u ? static_cast<std::uint8_t>(((x & 1u) ? byte >> 4u : byte & 0x0Fu) * 17u) : byte;
            }
        return pixels;
    }
};

std::filesystem::path empty_flash(const char *name) {
    const auto root = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "font");
    return root;
}

} // namespace

TEST_CASE("sceLibFont serves the bundled fonts when flash0/font is empty") {
    FontLib f(empty_flash("anypsp-hle-font-empty"));
    auto &m = f.h.rt.memory();
    CHECK_EQ(f.h.call(FontLib::kLib, 0x27F6E642u, f.lib, FontLib::kError), 18u);
    CHECK_EQ(m.load32(FontLib::kError), 0u);

    const std::uint32_t latin = f.find(1u, 1u, 2u);
    CHECK(f.file_of(latin) == "ltn0.pgf");
    CHECK(f.file_of(f.find(2u, 5u, 2u)) == "ltn5.pgf");
    CHECK(f.file_of(f.find(0u, 0u, 1u)) == "jpn0.pgf");
    CHECK(f.file_of(f.find(0u, 0u, 3u)) == "kr0.pgf");

    const std::uint32_t font = f.open(latin);
    CHECK_EQ(f.h.call(FontLib::kLib, 0x0DA7535Eu, font, FontLib::kInfo), 0u);
    const std::uint32_t style = FontLib::kInfo + 92u;
    CHECK_EQ(std::bit_cast<float>(m.load32(style)), 10.125f); // 0x288 / 64 points, as the PSP's ltn0
    CHECK_EQ(std::bit_cast<float>(m.load32(style + 8u)), 128.0f);
    CHECK_EQ(m.load16(style + 20u), 1u);
    CHECK_EQ(m.load16(style + 26u), 2u);
    CHECK_EQ(m.load32(FontLib::kInfo + 260u), 4u); // 4 bits per pixel
    const auto ascender = static_cast<std::int32_t>(m.load32(FontLib::kInfo + 8u));
    const auto descender = static_cast<std::int32_t>(m.load32(FontLib::kInfo + 12u));
    CHECK(ascender >= 14 * 64 && ascender <= 22 * 64);
    CHECK(descender < 0 && descender > -8 * 64);
    CHECK(m.load16(FontLib::kInfo + 82u) <= 32u); // max glyph height in pixels
    CHECK(m.load32(FontLib::kInfo + 84u) > 300u);  // glyph count

    // 'A' sits on the baseline: its bitmap top is its height, its descender 0.
    const std::int32_t width = f.char_info(font, 'A', 0u), height = f.char_info(font, 'A', 1u), top = f.char_info(font, 'A', 3u);
    CHECK(width >= 9 && width <= 15);
    CHECK(height >= 11 && height <= 15);
    CHECK_EQ(top, height);
    CHECK_EQ(f.char_info(font, 'A', 4u), width * 64);
    CHECK_EQ(f.char_info(font, 'A', 5u), height * 64);
    CHECK_EQ(f.char_info(font, 'A', 6u), top * 64);
    CHECK_EQ(f.char_info(font, 'A', 7u), 0);
    CHECK_EQ(f.char_info(font, 'A', 9u), top * 64);
    const std::int32_t advance = f.char_info(font, 'A', 12u);
    CHECK(advance >= width * 64 && advance % 64 == 0);
    CHECK(f.char_info(font, 'g', 1u) > f.char_info(font, 'g', 3u)); // reaches below the baseline
    CHECK(f.char_info(font, ' ', 0u) == 0 && f.char_info(font, ' ', 12u) > 0);

    // A character the font lacks is measured and drawn as the alternate character ('_' by default).
    CHECK_EQ(f.char_info(font, 0x4E00u, 0u), f.char_info(font, '_', 0u));
    CHECK_EQ(f.char_info(font, 0x4E00u, 12u), f.char_info(font, '_', 12u));
    CHECK(f.draw(font, 0x4E00u, 2u) == f.draw(font, '_', 2u));
    CHECK_EQ(f.h.call(FontLib::kLib, 0xEE232411u, f.lib, '-'), 0u);
    CHECK_EQ(f.char_info(font, 0x4E00u, 0u), f.char_info(font, '-', 0u));

    // The 8-bit image covers exactly the glyph's bitmap at the origin, with full-intensity pixels.
    const auto bytes = f.draw(font, 'A', 2u);
    std::int32_t x0 = 32, y0 = 32, x1 = -1, y1 = -1;
    bool solid = false;
    for (std::int32_t y = 0; y < 32; ++y)
        for (std::int32_t x = 0; x < 32; ++x)
            if (const std::uint8_t v = bytes[static_cast<std::size_t>(y * 32 + x)]; v != 0u) {
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
                solid = solid || v == 255u;
            }
    CHECK(x0 == 0 && y0 == 0 && x1 == width - 1 && y1 == height - 1);
    CHECK(solid);

    // 4-bit images hold the same pixels at 4-bit precision; clipping leaves the rest untouched.
    const auto nibbles = f.draw(font, 'A', 0u);
    for (std::size_t i = 0; i < bytes.size(); ++i) CHECK_EQ(nibbles[i], static_cast<std::uint8_t>(bytes[i] / 17u * 17u));
    const auto clipped = f.draw(font, 'A', 2u, 4);
    for (std::size_t i = 0; i < bytes.size(); ++i) CHECK_EQ(clipped[i], i % 32u < 4u ? bytes[i] : 0u);

    CHECK_EQ(f.h.call(FontLib::kLib, 0x3AEA8CB6u, font), 0u);
    CHECK_EQ(f.h.call(FontLib::kLib, 0x574B6FBCu, f.lib), 0u);
}

TEST_CASE("sceLibFont's bundled Japanese and Korean fonts cover kana, JIS X 0208 kanji and hangul") {
    FontLib f(empty_flash("anypsp-hle-font-cjk"));
    const std::uint32_t jp = f.open(f.find(0u, 0u, 1u));
    const std::uint32_t kr = f.open(f.find(0u, 0u, 3u));
    for (const std::uint32_t code : {0x3042u, 0x30CAu, 0x706Bu, 0x5F71u, 0x5F0Cu, 0x41u}) // あ ナ 火 影 弌 (level 2)
        CHECK(f.char_info(jp, code, 0u) > 4 && f.char_info(jp, code, 0u) != f.char_info(jp, '_', 0u));
    for (const std::uint32_t code : {0xB098u, 0xD55Cu, 0x41u}) // 나 한
        CHECK(f.char_info(kr, code, 0u) > 4 && f.char_info(kr, code, 0u) != f.char_info(kr, '_', 0u));
}

TEST_CASE("sceLibFont looks in flash0/font, then the per-user font folder, then uses the bundled font") {
    const auto flash = empty_flash("anypsp-hle-font-file");
    const auto user = flash / "user";
    std::filesystem::create_directories(user);
    // Any valid PGF works as a stand-in for a real font: the bundled Japanese and Korean ones under Latin names.
    auto save = [](const std::filesystem::path &path, const unsigned char *data, std::size_t size) {
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    };
    save(flash / "font" / "ltn0.pgf", anypsp_font_sans_jp, anypsp_font_sans_jp_size);
    save(user / "ltn0.pgf", anypsp_font_sans_kr, anypsp_font_sans_kr_size);
    save(user / "ltn4.pgf", anypsp_font_sans_kr, anypsp_font_sans_kr_size);
    std::ofstream(flash / "font" / "ltn1.pgf", std::ios::binary) << "not a font";
    save(user / "ltn1.pgf", anypsp_font_sans_kr, anypsp_font_sans_kr_size);
    std::ofstream(flash / "font" / "ltn2.pgf", std::ios::binary) << "not a font";

    FontLib f(flash, user);
    auto has = [&](std::uint32_t font, std::uint32_t code) { return f.draw(font, code, 2u) != f.draw(font, '_', 2u); };
    const std::uint32_t game = f.open(f.find(1u, 1u, 2u));      // ltn0: in both folders, the game's wins
    const std::uint32_t per_user = f.open(f.find(1u, 5u, 2u));  // ltn4: only in the per-user folder
    const std::uint32_t skipped = f.open(f.find(2u, 1u, 2u));   // ltn1: unreadable in the game's, read from the user's
    const std::uint32_t bundled = f.open(f.find(2u, 5u, 2u));   // ltn5: in neither
    const std::uint32_t broken = f.open(f.find(1u, 2u, 2u));    // ltn2: unreadable and absent: bundled
    CHECK(has(game, 0x3042u) && !has(game, 0xD55Cu));
    CHECK(has(per_user, 0xD55Cu) && !has(per_user, 0x3042u));
    CHECK(has(skipped, 0xD55Cu));
    CHECK(!has(bundled, 0x3042u) && !has(bundled, 0xD55Cu) && has(bundled, 0xE9u));
    CHECK(has(broken, 'A') && !has(broken, 0xD55Cu));
}
