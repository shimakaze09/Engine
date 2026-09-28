$input a_position, a_texcoord0, a_indices, a_weight
$output v_texcoord0

// Shadow depth vertex stage for skinned alpha-mask casters (MASKED,
// SKINNED): linear-blend skinning from the uBones array, as in
// shadow_depth_skinned.vs.sc, plus the texture coordinate the fragment
// stage tests the material's opacity mask at.

#include <bgfx_shader.sh>

uniform mat4 u_lightMVP;
uniform mat4 uBones[128];

void main() {
    vec3 localPosition = a_position;
    float weightSum = dot(a_weight, vec4_splat(1.0));
    if (weightSum > 0.0) {
        mat4 skin = a_weight.x * uBones[int(a_indices.x)] +
                    a_weight.y * uBones[int(a_indices.y)] +
                    a_weight.z * uBones[int(a_indices.z)] +
                    a_weight.w * uBones[int(a_indices.w)];
        localPosition = mul(skin, vec4(a_position, 1.0)).xyz;
    }
    v_texcoord0 = a_texcoord0;
    gl_Position = mul(u_lightMVP, vec4(localPosition, 1.0));
}
