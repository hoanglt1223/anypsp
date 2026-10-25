R"GLSL(
// A constant colour (alpha fills and the stencil-to-alpha passes write only alpha).
PARAMS(vec4, u_fill, 1);
layout(location = 0) out vec4 o_color;
void main() { o_color = u_fill[0]; }
)GLSL"
