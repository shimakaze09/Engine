// Defines the gbuffer vertex shader used by the Engine renderer. Foliage
// meshes root at local y = 0: the quadratic wind factor pins roots to the
// ground and bends only the tips.

#version 330 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in mat4 aInstanceModel;
layout(location = 7) in vec4 aInstanceFoliage;
#ifdef SKINNED
layout(location = 8) in vec4 aJoints;
layout(location = 9) in vec4 aWeights;

layout(std140) uniform BonePalette {
    mat4 uBones[128];
};
#endif

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat3 uNormalMatrix;
uniform int uUseInstancing;
uniform float uTime;
uniform float uFoliageWindStrength;
uniform float uFoliageWindFrequency;
uniform float uFoliagePhase;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vTexCoord;

/// Runs the shader entry point for this stage.
void main() {
    mat4 model = (uUseInstancing != 0) ? aInstanceModel : uModel;
    mat3 normalMatrix = (uUseInstancing != 0)
        /// Handles transpose.
        ? transpose(inverse(mat3(model)))
        : uNormalMatrix;
    vec3 localPosition = aPosition;
    vec3 localNormal = aNormal;
#ifdef SKINNED
    float weightSum = dot(aWeights, vec4(1.0));
    if (weightSum > 0.0) {
        mat4 skin = aWeights.x * uBones[int(aJoints.x)]
            + aWeights.y * uBones[int(aJoints.y)]
            + aWeights.z * uBones[int(aJoints.z)]
            + aWeights.w * uBones[int(aJoints.w)];
        localPosition = vec3(skin * vec4(aPosition, 1.0));
        localNormal = mat3(skin) * aNormal;
    }
#endif
    vec4 worldPos = model * vec4(localPosition, 1.0);
    float phase = (uUseInstancing != 0)
        ? aInstanceFoliage.x
        : uFoliagePhase;
    if (uFoliageWindStrength > 0.0) {
        float heightFactor = clamp(aPosition.y * 2.0, 0.0, 1.0);
        float bend = heightFactor * heightFactor;
        float waveArg =
            ((worldPos.x + worldPos.z) * uFoliageWindFrequency) + uTime + phase;
        float sway = sin(waveArg) * uFoliageWindStrength * bend;
        worldPos.x += sway;
        worldPos.z += cos(waveArg * 0.73) * uFoliageWindStrength * 0.35
            * bend;
    }
    vWorldPos = worldPos.xyz;
    vNormal = normalize(normalMatrix * localNormal);
    vTexCoord = aTexCoord;
    gl_Position = uProjection * uView * worldPos;
}
