$input a_position, a_normal
$output v_normal

// Default vertex stage (bgfx port of default.vert, #138). Uniform names
// match the GL source so flush parameter lookups resolve unchanged.

#include <bgfx_shader.sh>

uniform mat4 u_mvp;
uniform mat3 u_normalMatrix;

void main() {
    gl_Position = mul(u_mvp, vec4(a_position, 1.0));
    v_normal = mul(u_normalMatrix, a_normal);
}
