$input a_position, a_texcoord0
$output v_worldpos, v_texcoord0

// Point-shadow depth vertex stage for alpha-mask casters (MASKED): the
// world position the fragment distance needs, as in
// shadow_depth_point.vs.sc, plus the texture coordinate the fragment
// stage tests the material's opacity mask at. u_modelMatrix, not
// u_model: bgfx reserves the latter.

#include <bgfx_shader.sh>

uniform mat4 u_lightMVP;
uniform mat4 u_modelMatrix;

void main() {
    vec4 worldPos = mul(u_modelMatrix, vec4(a_position, 1.0));
    v_worldpos = worldPos.xyz;
    v_texcoord0 = a_texcoord0;
    gl_Position = mul(u_lightMVP, worldPos);
}
