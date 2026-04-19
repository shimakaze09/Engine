#version 330 core

in vec2 vTexCoord;

uniform sampler2D u_input;
uniform vec2 u_texelSize;

out vec4 outColor;

void main() {
  // Dual-Kawase downsample: 5-tap pattern.
  vec3 sum = texture(u_input, vTexCoord).rgb * 4.0;
  sum += texture(u_input, vTexCoord + vec2(-1.0, -1.0) * u_texelSize).rgb;
  sum += texture(u_input, vTexCoord + vec2( 1.0, -1.0) * u_texelSize).rgb;
  sum += texture(u_input, vTexCoord + vec2(-1.0,  1.0) * u_texelSize).rgb;
  sum += texture(u_input, vTexCoord + vec2( 1.0,  1.0) * u_texelSize).rgb;
  outColor = vec4(sum / 8.0, 1.0);
}
