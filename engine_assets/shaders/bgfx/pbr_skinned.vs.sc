$input a_position, a_normal, a_texcoord0, a_indices, a_weight
$output v_worldpos, v_normal, v_texcoord0

// SKINNED forward vertex stage: four-joint linear-blend skinning from the
// u_bones mat4 array (uploaded via set_param_mat4_array), then pbr.vs.sc's
// non-instanced path incl. foliage wind. Cooked as pbr.vert's SKINNED
// variants, one per shading model, so a Toon, Unlit or transparent skinned
// surface is posed as the G-buffer and shadow passes pose it; shaderc
// cannot guard $input lines, hence the separate source. The palette has
// its own name rather than the G-buffer's uBones so the two passes' upload
// caches never describe each other's value.

#include <bgfx_shader.sh>

uniform mat4 u_modelMatrix;
uniform mat4 u_mvp;
uniform mat4 u_viewProjection;
uniform mat3 u_normalMatrix;
uniform vec4 u_time;                 // .x: seconds
uniform vec4 uFoliageWindStrength;   // .x
uniform vec4 uFoliageWindFrequency;  // .x
uniform vec4 uFoliagePhase;          // .x
uniform mat4 u_bones[128];

void main() {
    vec3 localPosition = a_position;
    vec3 localNormal = a_normal;
    float weightSum = dot(a_weight, vec4_splat(1.0));
    if (weightSum > 0.0) {
        mat4 skin = a_weight.x * u_bones[int(a_indices.x)] +
                    a_weight.y * u_bones[int(a_indices.y)] +
                    a_weight.z * u_bones[int(a_indices.z)] +
                    a_weight.w * u_bones[int(a_indices.w)];
        localPosition = mul(skin, vec4(a_position, 1.0)).xyz;
        localNormal = mul(skin, vec4(a_normal, 0.0)).xyz;
    }
    vec4 worldPos = mul(u_modelMatrix, vec4(localPosition, 1.0));
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
    v_normal = normalize(mul(u_normalMatrix, localNormal));
    v_texcoord0 = a_texcoord0;
    gl_Position = (windStrength > 0.0)
        ? mul(u_viewProjection, worldPos)
        : mul(u_mvp, vec4(localPosition, 1.0));
}
