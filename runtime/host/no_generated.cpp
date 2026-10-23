// SPDX-License-Identifier: GPL-2.0-or-later
//
// Linked into the interpreter-only runner (anypsp-interp): no translated code,
// every guest instruction runs in the reference interpreter.

#include "psprecomp/runtime.hpp"

namespace psprecomp {

void register_generated_functions(Runtime &) {}

} // namespace psprecomp
