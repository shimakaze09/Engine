// Defines the skybox fragment shader used by the Engine renderer.

#version 330 core

in vec3 vTexCoord;

uniform samplerCube u_skybox;

out vec4 FragColor;

/// Runs the shader entry point for this stage.
void main() {
  vec3 skyColor = texture(u_skybox, normalize(vTexCoord)).rgb;
  FragColor = vec4(skyColor, 1.0);
}
