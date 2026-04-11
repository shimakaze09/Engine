#version 450 core
in vec3 vNormal;
uniform float u_time;
uniform vec3 u_albedo;
out vec4 outColor;

void main() {
  const vec3 lightDirection = normalize(vec3(0.4, 1.0, 0.6));
  const float diffuse = max(dot(normalize(vNormal), lightDirection), 0.0);
  const float pulse = 0.95 + (0.05 * sin(u_time * 0.5));
  const vec3 albedo = u_albedo;
  outColor = vec4(albedo * (diffuse * 0.9 + 0.1) * pulse, 1.0);
}
