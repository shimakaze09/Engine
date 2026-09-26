$input v_worldpos, v_texcoord0

// Point-shadow linear-distance depth for alpha-mask casters (MASKED):
// discards where the material's opacity mask falls below its cutoff, as
// shadow_depth_masked.fs.sc does, and writes the normalized distance
// shadow_depth_point.fs.sc writes elsewhere. Scalar uniforms are vec4
// read through .x/.xy/.xyz.

#include <bgfx_shader.sh>

SAMPLER2D(s_opacityMask, 0);
uniform vec4 u_alphaCutoff; // .x
uniform vec4 u_uvTiling;    // .xy
uniform vec4 u_uvOffset;    // .xy
uniform vec4 u_lightPos;    // .xyz
uniform vec4 u_farPlane;    // .x

void main() {
    vec2 uv = v_texcoord0 * u_uvTiling.xy + u_uvOffset.xy;
    if (texture2D(s_opacityMask, uv).r < u_alphaCutoff.x) {
        discard;
    }
    float dist = length(v_worldpos - u_lightPos.xyz);
    gl_FragDepth = dist / u_farPlane.x;
}
