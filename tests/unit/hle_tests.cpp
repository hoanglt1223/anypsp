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
