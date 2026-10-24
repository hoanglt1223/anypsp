// SPDX-License-Identifier: GPL-2.0-or-later
#include "test_harness.hpp"

#include "psprecomp/guest_memory.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

using psprecomp::GuestMemory;

namespace {

struct HookLog {
    GuestMemory *memory{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> calls;
    bool clear_on_call{true};
};

void record(void *context, std::uint32_t offset, std::uint32_t length) {
    auto &log = *static_cast<HookLog *>(context);
    log.calls.emplace_back(offset, length);
    // Like the renderer: after the download the page is clean but still watched.
    if (log.clear_on_call) log.memory->set_vram_page_state(offset >> GuestMemory::kVramPageShift, GuestMemory::kVramWatched);
}

constexpr std::uint32_t kPage3 = 3u << GuestMemory::kVramPageShift;

} // namespace

TEST_CASE("vram tracking: untracked memory never calls out") {
    auto memory = std::make_unique<GuestMemory>();
    memory->set_vram_page_state(3u, GuestMemory::kVramGpuDirty | GuestMemory::kVramWatched);
    memory->store32(0x04000000u + kPage3, 1u);
    CHECK_EQ(memory->load32(0x04000000u + kPage3), 1u);
    std::array<bool, GuestMemory::kVramPages> written{};
    CHECK(!memory->take_vram_cpu_writes(written));
}

TEST_CASE("vram tracking: reads of GPU-dirty pages call the hook once, then run free") {
    auto memory = std::make_unique<GuestMemory>();
    HookLog log{memory.get()};
    memory->set_vram_read_hook(&record, &log);
    memory->set_vram_page_state(3u, GuestMemory::kVramGpuDirty | GuestMemory::kVramWatched);
    (void)memory->aot_load32(0x44000000u + kPage3 + 8u); // uncached alias
    CHECK_EQ(log.calls.size(), 1u);
    CHECK_EQ(log.calls[0].first, kPage3 + 8u);
    CHECK_EQ(log.calls[0].second, 4u);
    (void)memory->load8(0x04000000u + kPage3 + 9u);
    (void)memory->aot_load16(0x04000000u + kPage3 + 10u);
    CHECK_EQ(log.calls.size(), 1u);
    (void)memory->aot_load32(0x04000000u + 0x1000u); // clean, unwatched page
    CHECK_EQ(log.calls.size(), 1u);
    std::array<bool, GuestMemory::kVramPages> written{};
    CHECK(!memory->take_vram_cpu_writes(written));
}

TEST_CASE("vram tracking: writes download dirty pages first and mark watched pages written") {
    auto memory = std::make_unique<GuestMemory>();
    HookLog log{memory.get()};
    memory->set_vram_read_hook(&record, &log);
    memory->set_vram_page_state(3u, GuestMemory::kVramGpuDirty | GuestMemory::kVramWatched);
    memory->aot_store32(0x04000000u + kPage3, 0x12345678u);
    CHECK_EQ(log.calls.size(), 1u);
    memory->aot_store8(0x04000000u + 0x8000u, 1u); // unwatched
    std::array<bool, GuestMemory::kVramPages> written{};
    CHECK(memory->take_vram_cpu_writes(written));
    CHECK(written[3]);
    CHECK(!written[8]);
    CHECK_EQ(memory->vram_page_state(3u), GuestMemory::kVramWatched);
    CHECK(!memory->take_vram_cpu_writes(written));
    // set_vram_page_state keeps a pending write.
    memory->store16(0x04000000u + kPage3 + 2u, 7u);
    memory->set_vram_page_state(3u, GuestMemory::kVramWatched);
    CHECK(memory->take_vram_cpu_writes(written));
    CHECK(written[3]);
}

TEST_CASE("vram tracking: bulk and raw accesses are observed, RAM and the scratchpad are not") {
    auto memory = std::make_unique<GuestMemory>();
    HookLog log{memory.get()};
    memory->set_vram_read_hook(&record, &log);
    memory->set_vram_page_state(3u, GuestMemory::kVramGpuDirty | GuestMemory::kVramWatched);
    std::array<std::uint8_t, 16> bytes{};
    memory->copy_out(0x04000000u + kPage3 - 8u, bytes); // straddles pages 2 and 3
    CHECK_EQ(log.calls.size(), 1u);
    CHECK_EQ(log.calls[0].first, kPage3 - 8u);
    CHECK_EQ(log.calls[0].second, 16u);
    memory->set_vram_page_state(3u, GuestMemory::kVramGpuDirty | GuestMemory::kVramWatched);
    memory->note_raw_access(0x08800000u, 64u, true);
    memory->note_raw_access(0x00010000u, 64u, false);
    CHECK_EQ(log.calls.size(), 1u);
    memory->note_raw_access(0x04200000u + kPage3, 64u, false); // VRAM mirror
    CHECK_EQ(log.calls.size(), 2u);
    memory->zero(0x04000000u + kPage3, 32u);
    std::array<bool, GuestMemory::kVramPages> written{};
    CHECK(memory->take_vram_cpu_writes(written));
    CHECK(written[3]);
}
