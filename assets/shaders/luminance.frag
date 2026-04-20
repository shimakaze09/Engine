#version 330 core

in vec2 vTexCoord;

uniform sampler2D u_sceneColor;

out vec4 outColor;

// Compute luminance from linear RGB using standard coefficients.
float luminance(vec3 color) {
  return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
  vec3 hdr = texture(u_sceneColor, vTexCoord).rgb;
  float lum = luminance(hdr);
  // Output log-luminance to allow simple averaging.
  // Add small epsilon to avoid log(0).
  float logLum = log(max(lum, 0.0001));
  outColor = vec4(logLum, lum, 0.0, 1.0);
}
