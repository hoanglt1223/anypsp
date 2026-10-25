// SPDX-License-Identifier: GPL-2.0-or-later
// HLE functions called directly through the runtime's import table, without a guest program.

#include "test_harness.hpp"

#include "kernel.hpp"

#include "psprecomp/runtime.hpp"

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
