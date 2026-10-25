# Turns one of runtime/host/shaders/*.inc (GLSL wrapped in a C++ raw string, shared with the
# GL backend) into Vulkan GLSL 4.50 for glslangValidator:
#   cmake -DIN=<shader.inc> -DOUT=<shader.glsl> -P vk-shader-source.cmake
file(READ "${IN}" body)
string(REGEX REPLACE "^R\"GLSL\\(\n" "" body "${body}")
string(REGEX REPLACE "\\)GLSL\"\n?$" "" body "${body}")
set(prelude [=[
#version 450
precision highp float;
precision highp int;
#define VERTEX_ID gl_VertexIndex
#define VARYING_IN(n) layout(location = n) in
#define VARYING_OUT(n) layout(location = n) out
#define VARYING_IN_LINEAR(n) layout(location = n) noperspective in
#define VARYING_OUT_LINEAR(n) layout(location = n) noperspective out
#define SAMPLER(n) layout(set = 0, binding = n) uniform
#define PARAMS(type, name, count) layout(push_constant) uniform Params { type name[count]; }
#define DEPTH_NDC(z) (z)
#define PRESENT_ROW(y) (y)
]=])
file(WRITE "${OUT}.tmp" "${prelude}${body}")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
