#version 330 core

in vec2 vTexCoord;

uniform sampler2D u_sceneColor;
uniform float u_threshold;

out vec4 outColor;

void main() {
  vec3 c = texture(u_sceneColor, vTexCoord).rgb;
  float brightness = dot(c, vec3(0.2126, 0.7152, 0.0722));
  float contribution = max(brightness - u_threshold, 0.0);
  outColor = vec4(c * contribution / max(brightness, 0.001), 1.0);
}
