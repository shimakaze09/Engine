$input a_position

// Shadow depth vertex stage (bgfx port of shadow_depth.vert, #138):
// u_lightMVP already contains the model matrix (light VP x model on the
// CPU). The SKINNED variant lands with the skinning unit.

#include <bgfx_shader.sh>

uniform mat4 u_lightMVP;

void main() {
    gl_Position = mul(u_lightMVP, vec4(a_position, 1.0));
}
