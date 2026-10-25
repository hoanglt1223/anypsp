R"GLSL(
// Stencil emulation: sets one stencil bit wherever that bit of the colour alpha is set
// (drawn with only the depth/stencil attachment; u_st[0].x is the bit).
SAMPLER(0) highp sampler2D u_src;
PARAMS(ivec4, u_st, 1);
layout(location = 0) out vec4 o_color;
void main() {
    int a = int(texelFetch(u_src, ivec2(gl_FragCoord.xy), 0).a * 255.0 + 0.5);
    if (((a >> u_st[0].x) & 1) == 0) discard;
    o_color = vec4(0.0);
}
)GLSL"
