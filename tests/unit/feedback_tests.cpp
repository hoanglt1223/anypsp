// SPDX-License-Identifier: GPL-2.0-or-later
#include "test_harness.hpp"

#include "functions.hpp"

#include "psprecomp/runtime.hpp"

#include <filesystem>

namespace {

void stop_here(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &) { runtime.stop("missing code reached"); }

void jump_to_missing(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.pc = 0x08804100u; }

} // namespace

TEST_CASE("runtime feedback: missing code is recorded once and reads back as seeds") {
    const auto path = std::filesystem::temp_directory_path() / "anypsp_runtime_feedback.txt";
    std::filesystem::remove(path);
    psprecomp::set_runtime_feedback_file(path.string());
    psprecomp::set_runtime_missing_code_handler(&stop_here);

    psprecomp::Runtime runtime;
    runtime.register_function(0x08804000u, &jump_to_missing, "start");
    runtime.run(0x08804000u, 100u);
    psprecomp::Runtime again;
    again.register_function(0x08804000u, &jump_to_missing, "start");
    again.run(0x08804000u, 100u);
    psprecomp::record_missing_code(0x08804200u);

    psprecomp::set_runtime_missing_code_handler(nullptr);
    const auto addresses = anypsp::analysis::read_feedback(path);
    CHECK((addresses == std::vector<std::uint32_t>{0x08804100u, 0x08804200u}));
}
