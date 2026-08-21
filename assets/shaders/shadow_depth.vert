// Defines the shadow depth vertex shader used by the Engine renderer.

#version 330 core

layout(location = 0) in vec3 aPosition;
#ifdef SKINNED
layout(location = 8) in vec4 aJoints;
layout(location = 9) in vec4 aWeights;

uniform mat4 uBones[128];
#endif

// u_lightMVP already contains the model matrix (light VP x model on the CPU).
uniform mat4 u_lightMVP;

/// Runs the shader entry point for this stage.
void main() {
  vec3 localPosition = aPosition;
#ifdef SKINNED
  float weightSum = dot(aWeights, vec4(1.0));
  if (weightSum > 0.0) {
    mat4 skin = aWeights.x * uBones[int(aJoints.x)]
        + aWeights.y * uBones[int(aJoints.y)]
        + aWeights.z * uBones[int(aJoints.z)]
        + aWeights.w * uBones[int(aJoints.w)];
    localPosition = vec3(skin * vec4(aPosition, 1.0));
  }
#endif
  gl_Position = u_lightMVP * vec4(localPosition, 1.0);
}
