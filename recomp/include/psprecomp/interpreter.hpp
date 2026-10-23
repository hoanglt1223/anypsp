#pragma once

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/runtime.hpp"

#include <cstddef>
#include <cstdint>

namespace psprecomp {

using DynamicCodeCapture = void (*)(std::uint32_t address, const std::uint32_t *words, std::size_t count);

void interpret(Runtime &runtime, AllegrexContext &ctx);
[[nodiscard]] std::uint64_t dynamic_code_hash(const std::uint32_t *words, std::size_t count) noexcept;
void register_dynamic_function(std::uint32_t address, std::uint64_t hash, Runtime::RecompiledFunction function);
void set_dynamic_code_capture(DynamicCodeCapture capture) noexcept;
void run_dynamic_code(Runtime &runtime, AllegrexContext &ctx);

} // namespace psprecomp
