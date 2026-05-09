#version 330 core

in vec2 vTexCoord;

out vec2 FragColor;

const float PI = 3.14159265359;
const uint SAMPLE_COUNT = 512u;

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

float geometry_schlick_ggx(float nDotV, float roughness) {
  float a = roughness * roughness;
  float k = a * 0.5;
  return nDotV / (nDotV * (1.0 - k) + k);
}

float geometry_smith(float nDotV, float nDotL, float roughness) {
  return geometry_schlick_ggx(nDotV, roughness) *
         geometry_schlick_ggx(nDotL, roughness);
}

vec2 integrate_brdf(float nDotV, float roughness) {
  vec3 viewDir = vec3(sqrt(max(1.0 - nDotV * nDotV, 0.0)), 0.0, nDotV);
  vec3 normal = vec3(0.0, 0.0, 1.0);

  float scale = 0.0;
  float bias = 0.0;
  for (uint i = 0u; i < SAMPLE_COUNT; ++i) {
    vec3 halfVector = importance_sample_ggx(
        hammersley(i, SAMPLE_COUNT), normal, roughness);
    vec3 lightDir = normalize(2.0 * dot(viewDir, halfVector) * halfVector -
                              viewDir);

    float nDotL = max(lightDir.z, 0.0);
    float nDotH = max(halfVector.z, 0.0);
    float vDotH = max(dot(viewDir, halfVector), 0.0);
    if (nDotL > 0.0) {
      float geometry = geometry_smith(nDotV, nDotL, roughness);
      float geometryVisibility =
          (geometry * vDotH) / max(nDotH * nDotV, 0.0001);
      float fresnel = pow(1.0 - vDotH, 5.0);
      scale += (1.0 - fresnel) * geometryVisibility;
      bias += fresnel * geometryVisibility;
    }
  }

  return vec2(scale, bias) / float(SAMPLE_COUNT);
}

void main() {
  float nDotV = clamp(vTexCoord.x, 0.001, 1.0);
  float roughness = clamp(vTexCoord.y, 0.001, 1.0);
  FragColor = integrate_brdf(nDotV, roughness);
}
