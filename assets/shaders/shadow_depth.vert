// Defines the shadow depth vertex shader used by the Engine renderer.

#version 330 core

layout(location = 0) in vec3 aPosition;

uniform mat4 u_lightMVP;
uniform mat4 u_model;

/// Runs the shader entry point for this stage.
void main() {
  gl_Position = u_lightMVP * u_model * vec4(aPosition, 1.0);
}
