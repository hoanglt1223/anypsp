#include "psprecomp/interpreter.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/decoder.hpp"

#include "cpu/interp.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace psprecomp {
namespace {

using K = OpcodeKind;

constexpr std::uint32_t kBudget = 1u << 20u;
constexpr std::uint32_t kMaxBlockBytes = 4096u;

struct DynamicBlock {
    std::vector<std::uint32_t> words;
    Runtime::RecompiledFunction function{};
};

std::map<std::pair<std::uint32_t, std::uint64_t>, Runtime::RecompiledFunction> &registered_blocks() {
    static std::map<std::pair<std::uint32_t, std::uint64_t>, Runtime::RecompiledFunction> blocks;
    return blocks;
}

std::unordered_map<std::uint32_t, DynamicBlock> g_blocks;
std::set<std::pair<std::uint32_t, std::uint64_t>> g_captured;
DynamicCodeCapture g_capture = nullptr;

std::size_t block_extent(const GuestMemory &m, std::uint32_t address) {
    std::uint32_t furthest = address;
    for (std::uint32_t pc = address; pc - address < kMaxBlockBytes; pc += 4u) {
        if (!m.contains(pc, 8u)) return (pc - address) / 4u;
        const DecodedInstruction d = decode_allegrex(m.load32(pc));
        std::uint32_t target = 0u;
        if (d.kind == K::J || d.kind == K::Jal) target = ((pc + 4u) & 0xF0000000u) | (d.target << 2u);
        else if (d.has_delay_slot() && d.kind != K::Jr && d.kind != K::Jalr)
            target = pc + 4u + static_cast<std::uint32_t>(static_cast<std::int32_t>(d.immediate) * 4);
        if (d.kind != K::Jal && target > furthest && target - address < kMaxBlockBytes) furthest = target;
        if (d.kind == K::Jr && d.rs == 31u && pc >= furthest) return (pc + 8u - address) / 4u;
    }
    return kMaxBlockBytes / 4u;
}

} // namespace

std::uint64_t dynamic_code_hash(const std::uint32_t *words, std::size_t count) noexcept {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < count; ++i) {
        for (std::uint32_t b = 0; b < 4u; ++b) {
            hash ^= (words[i] >> (b * 8u)) & 0xFFu;
            hash *= 0x100000001B3ull;
        }
    }
    return hash;
}

void register_dynamic_function(std::uint32_t address, std::uint64_t hash, Runtime::RecompiledFunction function) {
    registered_blocks()[{address, hash}] = function;
}

void set_dynamic_code_capture(DynamicCodeCapture capture) noexcept { g_capture = capture; }

void run_dynamic_code(Runtime &rt, AllegrexContext &ctx) {
    auto &m = rt.memory();
    const std::uint32_t address = ctx.pc;
    DynamicBlock &block = g_blocks[address];
    const std::size_t bytes = block.words.size() * 4u;
    const std::uint8_t *live = bytes != 0u ? m.raw_pointer(address, bytes) : nullptr;
    if (live == nullptr || std::memcmp(live, block.words.data(), bytes) != 0) {
        const std::size_t count = block_extent(m, address);
        block.words.resize(count);
        for (std::size_t i = 0; i < count; ++i) block.words[i] = m.load32(address + static_cast<std::uint32_t>(i * 4u));
        const std::uint64_t hash = dynamic_code_hash(block.words.data(), count);
        const auto found = registered_blocks().find({address, hash});
        block.function = found != registered_blocks().end() ? found->second : nullptr;
        if (block.function == nullptr && g_capture != nullptr && count != 0u && g_captured.insert({address, hash}).second)
            g_capture(address, block.words.data(), count);
    }
    if (block.function != nullptr) block.function(rt, ctx);
    else interpret(rt, ctx);
}

void interpret(Runtime &rt, AllegrexContext &ctx) { anypsp::cpu::interpret(rt, ctx, kBudget); }

} // namespace psprecomp
