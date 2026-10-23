// SPDX-License-Identifier: GPL-2.0-or-later

#include "gl_api.hpp"

#define ANYPSP_GL_DEFINE(upper, name) PFNGL##upper##PROC anypsp_##name = nullptr;
ANYPSP_GL_FUNCTIONS(ANYPSP_GL_DEFINE)
#undef ANYPSP_GL_DEFINE

namespace anypsp::gpu {

const char *load_gl(GlProc (*resolve)(const char *name)) {
#define ANYPSP_GL_LOAD(upper, name)                                                                                    \
    anypsp_##name = reinterpret_cast<PFNGL##upper##PROC>(resolve(#name));                                             \
    if (anypsp_##name == nullptr) return #name;
    ANYPSP_GL_FUNCTIONS(ANYPSP_GL_LOAD)
#undef ANYPSP_GL_LOAD
    return nullptr;
}

} // namespace anypsp::gpu
