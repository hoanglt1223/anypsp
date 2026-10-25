R"GLSL(
// The displayed part of a surface (u_present[0] = size and origin as fractions of it),
// stretched over the output. GL output is stored bottom row first, Vulkan's top row first.
// u_present[1].x is the framebuffer format: 16-bit ones show only the bits they keep.
PARAMS(vec4, u_present, 2);
VARYING_OUT(0) vec2 v_uv;
void main() {
    vec2 p = vec2(float(VERTEX_ID & 1), float(VERTEX_ID >> 1));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    v_uv = u_present[0].zw + vec2(p.x, PRESENT_ROW(p.y)) * u_present[0].xy;
}
)GLSL"
