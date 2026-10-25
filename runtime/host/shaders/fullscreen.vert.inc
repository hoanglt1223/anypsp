R"GLSL(
// One triangle covering the viewport; the scissor picks the area.
void main() {
    vec2 p = vec2(float((VERTEX_ID << 1) & 2), float(VERTEX_ID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL"
