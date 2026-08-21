$input a_position, a_normal, a_texcoord0, a_indices, a_weight
$output v_worldpos, v_normal, v_texcoord0

// SKINNED G-buffer vertex stage (bgfx port of gbuffer.vert's SKINNED
// variant, #138): four-joint linear-blend skinning from the uBones
// mat4 array (uploaded via set_param_mat4_array), then the shared
// non-instanced path incl. foliage wind. Cooked as gbuffer.vert's
// SKINNED variant; shaderc cannot guard $input lines, hence the
// separate source.

#include <bgfx_shader.sh>

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat3 uNormalMatrix;
uniform vec4 uTime;                 // .x: seconds
uniform vec4 uFoliageWindStrength;  // .x
uniform vec4 uFoliageWindFrequency; // .x
uniform vec4 uFoliagePhase;         // .x
uniform mat4 uBones[128];

void main() {
    vec3 localPosition = a_position;
    vec3 localNormal = a_normal;
    float weightSum = dot(a_weight, vec4_splat(1.0));
    if (weightSum > 0.0) {
        mat4 skin = a_weight.x * uBones[int(a_indices.x)] +
                    a_weight.y * uBones[int(a_indices.y)] +
                    a_weight.z * uBones[int(a_indices.z)] +
                    a_weight.w * uBones[int(a_indices.w)];
        localPosition = mul(skin, vec4(a_position, 1.0)).xyz;
        localNormal = mul(skin, vec4(a_normal, 0.0)).xyz;
    }
    vec4 worldPos = mul(uModel, vec4(localPosition, 1.0));
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
    v_normal = normalize(mul(uNormalMatrix, localNormal));
    v_texcoord0 = a_texcoord0;
    gl_Position = mul(uProjection, mul(uView, worldPos));
}
