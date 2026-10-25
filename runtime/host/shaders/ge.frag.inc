R"GLSL(
// The GE's per-pixel work in its integer arithmetic: texture sampling (1/16 texel, 4-bit
// bilinear), texture function, alpha test, fog and dither. The formulas follow PPSSPP's
// software renderer (GPU/Software/Sampler.cpp, DrawPixel.cpp, Rasterizer.cpp; GPL-2.0-or-later),
// which documents them from hardware probes. Parameters (GpuShaderParams):
//   u_p[0] = target size (PSP pixels), drawing origin inside the target
//   u_p[1] = framebuffer format (0..3), -, texture function, flags (GpuShaderParams::Flag)
//   u_p[2] = sampled image size, origin of the PSP texture inside it
//   u_p[3] = PSP texture size when sampling part of a larger target (else 0), clamp u/v (1 = clamp)
//   u_p[4] = environment colour (0..255), alpha test reference | mask << 8 | function << 16
//   u_p[5] = fog colour (0..255), internal scale of the sampled image
//   u_p[6] = source colour scale (fixed blend factor A), internal scale of the target
//   u_p[7] = dither matrix rows (4 signed 4-bit values each)
SAMPLER(0) highp sampler2D u_tex;
PARAMS(vec4, u_p, 8);
VARYING_IN(0) vec2 v_uv;
VARYING_IN(1) float v_q;
VARYING_IN_LINEAR(2) vec4 v_color;
VARYING_IN_LINEAR(3) float v_fog;
layout(location = 0) out vec4 o_color;
bool compare(int f, int a, int b) {
    if (f == 0) return false;
    if (f == 1) return true;
    if (f == 2) return a == b;
    if (f == 3) return a != b;
    if (f == 4) return a < b;
    if (f == 5) return a <= b;
    if (f == 6) return a > b;
    return a >= b;
}
bool flag(int flags, int bit) { return (flags & bit) != 0; }
// What a 16-bit framebuffer keeps of a colour: truncated to its bits, shown expanded back to 8.
vec3 quantize(vec3 c, int format) {
    ivec3 v = ivec3(c);
    ivec3 bits = format == 0 ? ivec3(5, 6, 5) : format == 1 ? ivec3(5) : ivec3(4);
    ivec3 kept = v >> (8 - bits);
    return vec3((kept << (8 - bits)) | (kept >> (2 * bits - 8)));
}
int clamp_uv(int v, int size) {
    if (v >= size - 1) return size - 1;
    if (v >= 511) return 511;
    return v < 0 ? 0 : v;
}
ivec4 texel(ivec2 t, ivec2 size) {
    t.x = u_p[3].z > 0.5 ? clamp_uv(t.x, size.x) : (t.x & (size.x - 1) & 511);
    t.y = u_p[3].w > 0.5 ? clamp_uv(t.y, size.y) : (t.y & (size.y - 1) & 511);
    int scale = int(u_p[5].w);
    ivec2 image = ivec2(u_p[2].xy);
    ivec2 at = min(ivec2(u_p[2].zw) + t, image - 1) * scale + scale / 2;
    return ivec4(texelFetch(u_tex, at, 0) * 255.0 + 0.5);
}
ivec4 sample_texture(int flags) {
    vec2 window = u_p[3].xy;
    vec2 size = window.x > 0.0 ? window : u_p[2].xy;
    // A projected q at or below zero samples the last texel (pspautotests gpu/texmtx/negq).
    vec2 uv = v_q > 0.0 ? v_uv / v_q : size - 0.5;
    if (!flag(flags, 256)) {
        // Sampling an upscaled render target: the GPU's own filter keeps the extra detail.
        if (window.x > 0.0) uv = mix(mod(uv, window), clamp(uv, vec2(0.5), window - 0.5), u_p[3].zw);
        return ivec4(texture(u_tex, (uv + u_p[2].zw) / u_p[2].xy) * 255.0 + 0.5);
    }
    // 1/16 texel, truncated toward zero.
    ivec2 fixed16 = ivec2(clamp(uv * 16.0, vec2(-1073741824.0), vec2(1073741824.0)));
    ivec2 isize = ivec2(size);
    if (!flag(flags, 128)) return texel(fixed16 >> 4, isize);
    fixed16 -= 8;
    ivec2 f = fixed16 & 15;
    ivec2 b = fixed16 >> 4;
    // Horizontal lerps truncated to 8 bits, then the vertical one.
    ivec4 top = (texel(b, isize) * (16 - f.x) + texel(b + ivec2(1, 0), isize) * f.x) >> 4;
    ivec4 bot = (texel(b + ivec2(0, 1), isize) * (16 - f.x) + texel(b + ivec2(1, 1), isize) * f.x) >> 4;
    return (top * (16 - f.y) + bot * f.y) >> 4;
}
void main() {
    int flags = int(u_p[1].w);
    // Gouraud colour: screen-linear, floored per pixel (the epsilon absorbs float error at the vertices).
    ivec4 c = ivec4(floor(v_color * 255.0 + 1.0 / 64.0));
    if (!flag(flags, 16)) {
        if (flag(flags, 1)) {
            ivec4 t = sample_texture(flags);
            if (flag(flags, 32)) t.a = 255; // a 16-bit 565 target has no alpha
            int func = int(u_p[1].z);
            bool rgba = flag(flags, 2), twice = flag(flags, 4);
            ivec3 rgb;
            int a = rgba ? ((c.a + 1) * t.a) / 256 : c.a;
            if (func == 0) {
                rgb = ((c.rgb + 1) * t.rgb * (twice ? 2 : 1)) / 256;
            } else if (func == 1) {
                rgb = rgba ? ((c.rgb + 1) * (255 - t.a) + (t.rgb + 1) * t.a) / (twice ? 128 : 256) : t.rgb * (twice ? 2 : 1);
                a = c.a;
            } else if (func == 2) {
                rgb = ((255 - t.rgb) * c.rgb + t.rgb * ivec3(u_p[4].rgb) + 255) / (twice ? 128 : 256);
            } else if (func == 3) {
                rgb = t.rgb * (twice ? 2 : 1);
                a = rgba ? t.a : c.a;
            } else {
                rgb = (c.rgb + t.rgb) * (twice ? 2 : 1);
            }
            c = clamp(ivec4(rgb, a), 0, 255);
        }
        if (flag(flags, 8)) { // alpha test
            int test = int(u_p[4].w), mask = (test >> 8) & 255;
            if (!compare(test >> 16, c.a & mask, test & 255 & mask)) discard;
        }
        if (flag(flags, 64)) {
            int f = int(floor(v_fog + 1.0 / 64.0));
            c.rgb = (c.rgb * f + ivec3(u_p[5].rgb) * (255 - f) + 255) >> 8;
        }
    }
    vec3 rgb = vec3(c.rgb) * u_p[6].rgb;
    if (flag(flags, 512)) {
        ivec2 p = ivec2(gl_FragCoord.xy) / int(u_p[6].w) - ivec2(u_p[0].zw);
        int row = int(u_p[7][p.y & 3]);
        int d = (row >> ((p.x & 3) * 4)) & 15;
        rgb = clamp(rgb + float(d >= 8 ? d - 16 : d), 0.0, 255.0);
    }
    // Without blending the output is final: store what the PSP's 16-bit framebuffer would hold.
    if (flag(flags, 1024)) rgb = quantize(floor(rgb + 0.5), int(u_p[1].x));
    o_color = vec4(rgb, float(c.a)) / 255.0;
}
)GLSL"
