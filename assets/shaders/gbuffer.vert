// Defines the gbuffer vertex shader used by the Engine renderer.

#version 330 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in mat4 aInstanceModel;
layout(location = 7) in vec4 aInstanceFoliage;

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
    vec4 worldPos = model * vec4(aPosition, 1.0);
    float phase = (uUseInstancing != 0)
        ? aInstanceFoliage.x
        : uFoliagePhase;
    if (uFoliageWindStrength > 0.0) {
        float heightFactor = clamp(aPosition.y + 0.5, 0.0, 1.0);
        float waveArg =
            ((worldPos.x + worldPos.z) * uFoliageWindFrequency) + uTime + phase;
        float sway = sin(waveArg) * uFoliageWindStrength * heightFactor;
        worldPos.x += sway;
        worldPos.z += cos(waveArg * 0.73) * uFoliageWindStrength * 0.35
            * heightFactor;
    }
    vWorldPos = worldPos.xyz;
    vNormal = normalize(normalMatrix * aNormal);
    vTexCoord = aTexCoord;
    gl_Position = uProjection * uView * worldPos;
}
