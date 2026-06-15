// Defines the skybox vertex shader used by the Engine renderer.

#version 330 core

layout(location = 0) in vec3 inPosition;

uniform mat4 u_view;
uniform mat4 u_projection;

out vec3 vTexCoord;

/// Runs the shader entry point for this stage.
void main() {
  vTexCoord = inPosition;
  mat4 viewRotation = mat4(mat3(u_view));
  vec4 clipPos = u_projection * viewRotation * vec4(inPosition, 1.0);
  gl_Position = clipPos.xyww;
}
