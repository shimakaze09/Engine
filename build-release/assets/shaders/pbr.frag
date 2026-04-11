#version 330 core

// ---------- Constants ----------
const float PI = 3.14159265359;
const int MAX_DIR_LIGHTS   = 4;
const int MAX_POINT_LIGHTS = 8;

// ---------- Inputs ----------
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vTexCoord;

// ---------- Material uniforms ----------
uniform vec3  u_albedo;
uniform float u_roughness;
uniform float u_metallic;
uniform float u_time;
uniform vec3  u_cameraPos;
uniform int   u_hasAlbedoTexture;
uniform sampler2D u_albedoMap;

// ---------- Directional lights ----------
struct DirLight {
  vec3  direction;
  vec3  color;
  float intensity;
};
uniform int      u_dirLightCount;
uniform DirLight u_dirLights[MAX_DIR_LIGHTS];

// ---------- Point lights ----------
struct PointLight {
  vec3  position;
  vec3  color;
  float intensity;
};
uniform int        u_pointLightCount;
uniform PointLight u_pointLights[MAX_POINT_LIGHTS];

// ---------- Output ----------
out vec4 outColor;

// ---------- PBR helper functions ----------

// Normal distribution function (GGX/Trowbridge-Reitz).
float distribution_ggx(vec3 N, vec3 H, float roughness) {
  float a  = roughness * roughness;
  float a2 = a * a;
  float NdotH  = max(dot(N, H), 0.0);
  float NdotH2 = NdotH * NdotH;

  float denom = NdotH2 * (a2 - 1.0) + 1.0;
  denom = PI * denom * denom;
  return a2 / max(denom, 0.0001);
}

// Geometry function (Schlick-GGX, single direction).
float geometry_schlick_ggx(float NdotV, float roughness) {
  float r = roughness + 1.0;
  float k = (r * r) / 8.0;
  return NdotV / (NdotV * (1.0 - k) + k);
}

// Geometry function (Smith's method, both view and light directions).
float geometry_smith(vec3 N, vec3 V, vec3 L, float roughness) {
  float NdotV = max(dot(N, V), 0.0);
  float NdotL = max(dot(N, L), 0.0);
  return geometry_schlick_ggx(NdotV, roughness)
       * geometry_schlick_ggx(NdotL, roughness);
}

// Fresnel equation (Schlick approximation).
vec3 fresnel_schlick(float cosTheta, vec3 F0) {
  return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ---------- Cook-Torrance BRDF evaluation for a single light ----------
vec3 cook_torrance(vec3 N, vec3 V, vec3 L, vec3 radiance,
                   vec3 albedo, float metallic, float roughness, vec3 F0) {
  vec3 H = normalize(V + L);

  float D = distribution_ggx(N, H, roughness);
  float G = geometry_smith(N, V, L, roughness);
  vec3  F = fresnel_schlick(max(dot(H, V), 0.0), F0);

  // Specular.
  vec3  numerator   = D * G * F;
  float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0);
  vec3  specular    = numerator / max(denominator, 0.001);

  // Diffuse (energy conservation).
  vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

  float NdotL = max(dot(N, L), 0.0);
  return (kD * albedo / PI + specular) * radiance * NdotL;
}

// ---------- Main ----------
void main() {
  vec3 N = normalize(vNormal);
  vec3 V = normalize(u_cameraPos - vWorldPos);

  // Resolve albedo color.
  vec3 albedo = u_albedo;
  if (u_hasAlbedoTexture != 0) {
    albedo *= texture(u_albedoMap, vTexCoord).rgb;
  }

  float roughness = clamp(u_roughness, 0.04, 1.0);
  float metallic  = clamp(u_metallic,  0.0,  1.0);

  // Fresnel reflectance at normal incidence.
  vec3 F0 = mix(vec3(0.04), albedo, metallic);

  vec3 Lo = vec3(0.0);

  // Directional lights.
  for (int i = 0; i < u_dirLightCount; ++i) {
    vec3 L = normalize(-u_dirLights[i].direction);
    vec3 radiance = u_dirLights[i].color * u_dirLights[i].intensity;
    Lo += cook_torrance(N, V, L, radiance, albedo, metallic, roughness, F0);
  }

  // Point lights.
  for (int i = 0; i < u_pointLightCount; ++i) {
    vec3  lightVec  = u_pointLights[i].position - vWorldPos;
    float dist      = length(lightVec);
    vec3  L         = lightVec / max(dist, 0.0001);
    float atten     = 1.0 / (dist * dist + 1.0);
    vec3  radiance  = u_pointLights[i].color
                    * u_pointLights[i].intensity * atten;
    Lo += cook_torrance(N, V, L, radiance, albedo, metallic, roughness, F0);
  }

  // Ambient approximation.
  vec3 ambient = vec3(0.03) * albedo;

  // Output linear HDR — tone mapping in post-process (Phase 1C).
  vec3 color = ambient + Lo;
  outColor = vec4(color, 1.0);
}
