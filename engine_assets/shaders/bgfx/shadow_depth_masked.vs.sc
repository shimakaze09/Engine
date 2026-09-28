$input a_position, a_texcoord0
$output v_texcoord0

// Shadow depth vertex stage for alpha-mask casters (MASKED): the
// CPU-combined light MVP, as in shadow_depth.vs.sc, plus the texture
// coordinate the fragment stage tests the material's opacity mask at.
// Separate source because shaderc cannot guard $input lines.

#include <bgfx_shader.sh>

uniform mat4 u_lightMVP;

void main() {
    v_texcoord0 = a_texcoord0;
    gl_Position = mul(u_lightMVP, vec4(a_position, 1.0));
}
