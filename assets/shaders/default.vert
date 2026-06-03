// Defines the default vertex shader used by the Engine renderer.

#version 330 core
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
uniform mat4 u_mvp;
uniform mat3 u_normalMatrix;
out vec3 vNormal;

/// Runs the shader entry point for this stage.
void main() {
  gl_Position = u_mvp * vec4(inPosition, 1.0);
  vNormal = u_normalMatrix * inNormal;
}
