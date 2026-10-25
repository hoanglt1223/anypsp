R"GLSL(
// GE vertices arrive in drawing-space pixels (Ge front end): x, y, depth 0..65535, clip w.
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
layout(location = 3) in float a_q;
layout(location = 4) in float a_fog;
// u_p[0] = target size, origin. Fields: ge.frag.inc.
PARAMS(vec4, u_p, 8);
VARYING_OUT(0) vec2 v_uv;
VARYING_OUT(1) float v_q;
// The GE interpolates colour and fog screen-linearly (PPSSPP's hardware probes), but noperspective
// varyings break up large geometry that is clipped at the near plane (UNH 1's intro and fades), so
// both are interpolated with perspective here.
VARYING_OUT(2) vec4 v_color;
VARYING_OUT(3) float v_fog; // fog factor, 0..255 per vertex
void main() {
    float w = a_pos.w;
    v_fog = a_fog;
    vec2 ndc = (a_pos.xy + u_p[0].zw) / u_p[0].xy * 2.0 - 1.0;
    float z = DEPTH_NDC(clamp(a_pos.z / 65535.0, 0.0, 1.0));
    gl_Position = vec4(ndc * w, z * w, w);
    v_uv = a_uv;
    v_q = a_q;
    v_color = a_color;
}
)GLSL"
