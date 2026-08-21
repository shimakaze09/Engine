$input a_position, a_normal, a_texcoord0
$output v_worldpos, v_normal, v_texcoord0

// G-buffer vertex stage (bgfx port of gbuffer.vert, #138): the
// non-instanced, non-skinned path incl. foliage wind. Instanced and
// SKINNED variants land with the instancing and skinning units (the
// flush gates both on this backend until then). Scalar GL uniforms
// become vec4 read through .x.

#include <bgfx_shader.sh>

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat3 uNormalMatrix;
uniform vec4 uTime;                 // .x: seconds
uniform vec4 uFoliageWindStrength;  // .x
uniform vec4 uFoliageWindFrequency; // .x
uniform vec4 uFoliagePhase;         // .x

void main() {
    vec4 worldPos = mul(uModel, vec4(a_position, 1.0));
    float windStrength = uFoliageWindStrength.x;
    if (windStrength > 0.0) {
        float heightFactor = clamp(a_position.y * 2.0, 0.0, 1.0);
        float bend = heightFactor * heightFactor;
        float waveArg = ((worldPos.x + worldPos.z) *
                         uFoliageWindFrequency.x) +
                        uTime.x + uFoliagePhase.x;
        float sway = sin(waveArg) * windStrength * bend;
        worldPos.x += sway;
        worldPos.z += cos(waveArg * 0.73) * windStrength * 0.35 * bend;
    }
    v_worldpos = worldPos.xyz;
    v_normal = normalize(mul(uNormalMatrix, a_normal));
    v_texcoord0 = a_texcoord0;
    gl_Position = mul(uProjection, mul(uView, worldPos));
}
