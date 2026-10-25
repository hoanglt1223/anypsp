R"GLSL(
// Rebuilds a target's pixels from another target sharing the same VRAM bytes in a different
// pixel size: 32-bit source -> 16-bit target (mode 0) or back (GpuReinterpret).
//   u_ri[0] = mode, 16-bit format, byte delta (into - from), row bytes
//   u_ri[1] = source size in PSP pixels, internal scale
SAMPLER(0) highp sampler2D u_src;
PARAMS(ivec4, u_ri, 2);
layout(location = 0) out vec4 o_color;
uint pack16(vec4 c, int f) {
    uvec4 v = uvec4(clamp(c, 0.0, 1.0) * 255.0 + 0.5);
    if (f == 0) return (v.r >> 3) | ((v.g >> 2) << 5) | ((v.b >> 3) << 11);
    if (f == 1) return (v.r >> 3) | ((v.g >> 3) << 5) | ((v.b >> 3) << 10) | ((v.a >> 7) << 15);
    return (v.r >> 4) | ((v.g >> 4) << 4) | ((v.b >> 4) << 8) | ((v.a >> 4) << 12);
}
vec4 unpack16(uint v, int f) {
    if (f == 0) return vec4(float(v & 31u) / 31.0, float((v >> 5) & 63u) / 63.0, float((v >> 11) & 31u) / 31.0, 1.0);
    if (f == 1) return vec4(float(v & 31u) / 31.0, float((v >> 5) & 31u) / 31.0, float((v >> 10) & 31u) / 31.0,
                            float((v >> 15) & 1u));
    return vec4(float(v & 15u), float((v >> 4) & 15u), float((v >> 8) & 15u), float((v >> 12) & 15u)) / 15.0;
}
void main() {
    int mode = u_ri[0].x, format = u_ri[0].y, delta = u_ri[0].z, row_bytes = u_ri[0].w;
    ivec2 src_size = u_ri[1].xy;
    int scale = u_ri[1].z;
    // Work out the byte mapping at native size; at higher internal resolutions
    // each output pixel reads the matching sub-pixel of its source pixel.
    ivec2 scaled = ivec2(gl_FragCoord.xy);
    ivec2 p = scaled / scale;
    ivec2 sub = scaled - p * scale;
    int target_bpp = mode == 0 ? 2 : 4;
    int rel = delta + p.y * row_bytes + p.x * target_bpp;
    if (rel < 0) discard;
    int sy = rel / row_bytes;
    int rem = rel - sy * row_bytes;
    if (sy >= src_size.y) discard;
    if (mode == 0) {
        uvec4 b = uvec4(texelFetch(u_src, ivec2(rem / 4, sy) * scale + sub, 0) * 255.0 + 0.5);
        uint half16 = (rem & 2) == 0 ? (b.r | (b.g << 8)) : (b.b | (b.a << 8));
        o_color = unpack16(half16, format);
    } else {
        int sx = rem / 2;
        uint lo = pack16(texelFetch(u_src, ivec2(sx, sy) * scale + sub, 0), format);
        uint hi = pack16(texelFetch(u_src, ivec2(min(sx + 1, src_size.x - 1), sy) * scale + sub, 0), format);
        o_color = vec4(float(lo & 255u), float(lo >> 8), float(hi & 255u), float(hi >> 8)) / 255.0;
    }
}
)GLSL"
