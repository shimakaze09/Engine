$input v_texcoord0

// Shadow depth fragment stage for alpha-mask casters (MASKED): discards
// where the material's opacity mask (R channel, at the material's UV
// tiling and offset) falls below its cutoff, the test the lit passes
// apply, so a cut-out surface casts a cut-out shadow. Depth-only target,
// no color output. Scalar uniforms are vec4 read through .x/.xy.

#include <bgfx_shader.sh>

SAMPLER2D(s_opacityMask, 0);
uniform vec4 u_alphaCutoff; // .x
uniform vec4 u_uvTiling;    // .xy
uniform vec4 u_uvOffset;    // .xy

void main() {
    vec2 uv = v_texcoord0 * u_uvTiling.xy + u_uvOffset.xy;
    if (texture2D(s_opacityMask, uv).r < u_alphaCutoff.x) {
        discard;
    }
}
