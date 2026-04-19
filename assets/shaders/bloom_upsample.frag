#version 330 core

in vec2 vTexCoord;

uniform sampler2D u_input;
uniform vec2 u_texelSize;

out vec4 outColor;

void main() {
  // Dual-Kawase upsample: 9-tap tent filter.
  vec3 sum = vec3(0.0);
  sum += texture(u_input, vTexCoord + vec2(-1.0,  0.0) * u_texelSize).rgb;
  sum += texture(u_input, vTexCoord + vec2( 1.0,  0.0) * u_texelSize).rgb;
  sum += texture(u_input, vTexCoord + vec2( 0.0,  1.0) * u_texelSize).rgb;
  sum += texture(u_input, vTexCoord + vec2( 0.0, -1.0) * u_texelSize).rgb;
  sum += texture(u_input, vTexCoord + vec2(-0.5, -0.5) * u_texelSize * 2.0).rgb * 2.0;
  sum += texture(u_input, vTexCoord + vec2( 0.5, -0.5) * u_texelSize * 2.0).rgb * 2.0;
  sum += texture(u_input, vTexCoord + vec2(-0.5,  0.5) * u_texelSize * 2.0).rgb * 2.0;
  sum += texture(u_input, vTexCoord + vec2( 0.5,  0.5) * u_texelSize * 2.0).rgb * 2.0;
  outColor = vec4(sum / 12.0, 1.0);
}
