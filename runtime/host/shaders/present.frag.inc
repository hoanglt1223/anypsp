R"GLSL(
SAMPLER(0) highp sampler2D u_tex;
PARAMS(vec4, u_present, 2);
VARYING_IN(0) vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec3 c = texture(u_tex, v_uv).rgb;
    int format = int(u_present[1].x);
    if (format < 3) {
        ivec3 v = ivec3(c * 255.0 + 0.5);
        ivec3 bits = format == 0 ? ivec3(5, 6, 5) : format == 1 ? ivec3(5) : ivec3(4);
        ivec3 kept = v >> (8 - bits);
        c = vec3((kept << (8 - bits)) | (kept >> (2 * bits - 8))) / 255.0;
    }
    o_color = vec4(c, 1.0);
}
)GLSL"
