$input a_position, a_indices, a_weight

// SKINNED shadow depth vertex stage (bgfx port of shadow_depth.vert's
// SKINNED variant, #138): linear-blend skinning from the uBones array,
// then the CPU-combined light MVP. Separate source because shaderc
// cannot guard $input lines.

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
    gl_Position = mul(u_lightMVP, vec4(localPosition, 1.0));
}
