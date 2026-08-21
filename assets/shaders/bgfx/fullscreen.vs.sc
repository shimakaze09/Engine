$input a_position
$output v_texcoord0

// Fullscreen triangle vertex stage (bgfx port of fullscreen.vert, #138
// Phase C). The GL source derives the triangle from gl_VertexID; bgfx
// submits require a vertex stream, so this port reads the triangle
// positions (-1,-1)/(3,-1)/(-1,3) from a three-vertex buffer the pass
// supplies when Phase D ports the post stack.

#include <bgfx_shader.sh>

void main() {
    gl_Position = vec4(a_position.xy, 0.0, 1.0);
    v_texcoord0 = a_position.xy * 0.5 + 0.5;
}
