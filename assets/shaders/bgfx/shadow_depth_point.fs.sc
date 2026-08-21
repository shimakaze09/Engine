$input v_worldpos

// Point-shadow linear-distance depth (bgfx port of
// shadow_depth_point.frag, #138). Scalar GL uniforms become vec4 read
// through .x/.xyz.

#include <bgfx_shader.sh>

uniform vec4 u_lightPos; // .xyz
uniform vec4 u_farPlane; // .x

void main() {
    float dist = length(v_worldpos - u_lightPos.xyz);
    gl_FragDepth = dist / u_farPlane.x;
}
