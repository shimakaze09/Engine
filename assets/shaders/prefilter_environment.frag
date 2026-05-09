#version 330 core

in vec3 vTexCoord;

uniform samplerCube u_environmentMap;
uniform float u_roughness;

out vec4 FragColor;

const float PI = 3.14159265359;
const uint SAMPLE_COUNT = 64u;

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

vec3 importance_sample_ggx(vec2 xi, vec3 normal, float roughness) {
  float a = roughness * roughness;
  float phi = 2.0 * PI * xi.x;
  float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
  float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));

  vec3 halfVector =
      vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
  vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0)
                                  : vec3(1.0, 0.0, 0.0);
  vec3 tangent = normalize(cross(up, normal));
  vec3 bitangent = cross(normal, tangent);
  return normalize(tangent * halfVector.x + bitangent * halfVector.y +
                   normal * halfVector.z);
}

void main() {
  vec3 normal = normalize(vTexCoord);
  vec3 viewDir = normal;
  vec3 prefilteredColor = vec3(0.0);
  float totalWeight = 0.0;

  for (uint i = 0u; i < SAMPLE_COUNT; ++i) {
    vec2 xi = hammersley(i, SAMPLE_COUNT);
    vec3 halfVector = importance_sample_ggx(xi, normal, u_roughness);
    vec3 lightDir = normalize(2.0 * dot(viewDir, halfVector) * halfVector -
                              viewDir);
    float nDotL = max(dot(normal, lightDir), 0.0);
    if (nDotL > 0.0) {
      prefilteredColor += texture(u_environmentMap, lightDir).rgb * nDotL;
      totalWeight += nDotL;
    }
  }

  prefilteredColor /= max(totalWeight, 0.0001);
  FragColor = vec4(prefilteredColor, 1.0);
}
