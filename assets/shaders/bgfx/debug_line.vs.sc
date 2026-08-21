$input a_position, a_color0
$output v_color0

// Transforms depth-tested debug line vertices (bgfx port of
// debug_line.vert, #138 Phase C). Uniform names match the GL source so
// the flush code's parameter lookups resolve unchanged.

#include <bgfx_shader.sh>

uniform mat4 uViewProjection;

void main() {
    v_color0 = a_color0;
    gl_Position = mul(uViewProjection, vec4(a_position, 1.0));
}
