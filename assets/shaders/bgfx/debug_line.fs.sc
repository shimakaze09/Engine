$input v_color0

// Shades depth-tested debug line vertices (bgfx port of
// debug_line.frag, #138 Phase C).

#include <bgfx_shader.sh>

void main() {
    gl_FragColor = v_color0;
}
