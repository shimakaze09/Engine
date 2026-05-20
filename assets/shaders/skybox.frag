#version 330 core

in vec3 vTexCoord;

uniform samplerCube u_skybox;

out vec4 FragColor;

void main() {
  vec3 skyColor = texture(u_skybox, normalize(vTexCoord)).rgb;
  FragColor = vec4(skyColor, 1.0);
}
