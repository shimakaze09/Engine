// Defines the shadow depth vertex shader used by the Engine renderer.

#version 330 core

layout(location = 0) in vec3 aPosition;

// u_lightMVP already contains the model matrix (light VP x model on the CPU).
uniform mat4 u_lightMVP;

/// Runs the shader entry point for this stage.
void main() {
  gl_Position = u_lightMVP * vec4(aPosition, 1.0);
}
