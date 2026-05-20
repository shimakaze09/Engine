#version 330 core

in vec3 vTexCoord;

uniform samplerCube u_environmentMap;

out vec4 FragColor;

const float PI = 3.14159265359;
const uint SAMPLE_COUNT = 128u;

float radical_inverse_vdc(uint bits) {
  bits = (bits << 16u) | (bits >> 16u);
  bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
  bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
  bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
  bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
  return float(bits) * 2.3283064365386963e-10;
}

vec2 hammersley(uint index, uint count) {
  return vec2(float(index) / float(count), radical_inverse_vdc(index));
}

vec3 cosine_sample_hemisphere(vec2 xi, vec3 normal) {
  float phi = 2.0 * PI * xi.x;
  float cosTheta = sqrt(1.0 - xi.y);
  float sinTheta = sqrt(xi.y);

  vec3 localDir =
      vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
  vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0)
                                  : vec3(1.0, 0.0, 0.0);
  vec3 tangent = normalize(cross(up, normal));
  vec3 bitangent = cross(normal, tangent);
  return normalize(tangent * localDir.x + bitangent * localDir.y +
                   normal * localDir.z);
}

void main() {
  vec3 normal = normalize(vTexCoord);
  vec3 irradiance = vec3(0.0);

  for (uint i = 0u; i < SAMPLE_COUNT; ++i) {
    vec3 sampleDir = cosine_sample_hemisphere(
        hammersley(i, SAMPLE_COUNT), normal);
    irradiance += texture(u_environmentMap, sampleDir).rgb;
  }

  irradiance = (PI / float(SAMPLE_COUNT)) * irradiance;
  FragColor = vec4(irradiance, 1.0);
}
