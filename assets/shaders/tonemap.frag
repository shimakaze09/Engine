#version 450 core

in vec2 vTexCoord;

uniform sampler2D u_sceneColor;
uniform float u_exposure;

out vec4 outColor;

void main() {
  vec3 hdr = texture(u_sceneColor, vTexCoord).rgb;

  // Apply exposure.
  hdr *= u_exposure;

  // Reinhard tone mapping.
  vec3 mapped = hdr / (hdr + vec3(1.0));

  // sRGB gamma correction (approximate).
  outColor = vec4(pow(mapped, vec3(1.0 / 2.2)), 1.0);
}
