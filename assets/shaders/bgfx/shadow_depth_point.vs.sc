$input a_position
$output v_worldpos

// Point-shadow depth vertex stage (bgfx port of shadow_depth_point.vert,
// #138). u_modelMatrix, not u_model — bgfx reserves the latter.

#include <bgfx_shader.sh>

uniform mat4 u_lightMVP;
uniform mat4 u_modelMatrix;

void main() {
    vec4 worldPos = mul(u_modelMatrix, vec4(a_position, 1.0));
    v_worldpos = worldPos.xyz;
    gl_Position = mul(u_lightMVP, worldPos);
}
