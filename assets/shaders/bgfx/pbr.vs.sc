$input a_position, a_normal, a_texcoord0
$output v_worldpos, v_normal, v_texcoord0

// PBR forward vertex stage (bgfx port of pbr.vert, #138): the
// non-instanced path incl. foliage wind (roots pinned at local y = 0,
// quadratic bend toward the tips). Instanced foliage rendering under
// bgfx lands with the deferred/instancing unit, so this port carries no
// instance stream. Scalar GL uniforms become vec4 read through .x.

#include <bgfx_shader.sh>

uniform mat4 u_modelMatrix;
uniform mat4 u_mvp;
uniform mat4 u_viewProjection;
uniform mat3 u_normalMatrix;
uniform vec4 u_time;                 // .x: seconds
uniform vec4 uFoliageWindStrength;   // .x
uniform vec4 uFoliageWindFrequency;  // .x
uniform vec4 uFoliagePhase;          // .x

void main() {
    vec4 worldPos = mul(u_modelMatrix, vec4(a_position, 1.0));
    float windStrength = uFoliageWindStrength.x;
    if (windStrength > 0.0) {
        float heightFactor = clamp(a_position.y * 2.0, 0.0, 1.0);
        float bend = heightFactor * heightFactor;
        float waveArg = ((worldPos.x + worldPos.z) *
                         uFoliageWindFrequency.x) +
                        u_time.x + uFoliagePhase.x;
        float sway = sin(waveArg) * windStrength * bend;
        worldPos.x += sway;
        worldPos.z += cos(waveArg * 0.73) * windStrength * 0.35 * bend;
    }
    v_worldpos = worldPos.xyz;
    v_normal = normalize(mul(u_normalMatrix, a_normal));
    v_texcoord0 = a_texcoord0;
    gl_Position = (windStrength > 0.0)
        ? mul(u_viewProjection, worldPos)
        : mul(u_mvp, vec4(a_position, 1.0));
}
