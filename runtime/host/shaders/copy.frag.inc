R"GLSL(
// Copies the same pixel of another image (Vulkan MSAA: single-sampled colour back into the
// multisampled one after the CPU or a copy changed it).
SAMPLER(0) highp sampler2D u_src;
layout(location = 0) out vec4 o_color;
void main() { o_color = texelFetch(u_src, ivec2(gl_FragCoord.xy), 0); }
)GLSL"
