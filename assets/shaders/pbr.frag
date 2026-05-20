#version 330 core

const float PI = 3.14159265359;
const int MAX_DIR_LIGHTS = 4;
const int MAX_POINT_LIGHTS = 8;
const int MAX_SPOT_LIGHTS = 8;
const int SHADOW_CASCADE_COUNT = 4;
const int MAX_SPOT_SHADOW_LIGHTS = 4;
const int MAX_POINT_SHADOW_LIGHTS = 4;

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vTexCoord;

uniform vec3 u_albedo;
uniform float u_roughness;
uniform float u_metallic;
uniform float u_opacity;
uniform float u_time;
uniform vec3 u_cameraPos;
uniform int u_hasAlbedoTexture;
uniform sampler2D u_albedoMap;
uniform mat4 u_viewMatrix;
uniform int uFogMode;
uniform float uFogStart;
uniform float uFogEnd;
uniform float uFogDensity;
uniform vec3 uFogColor;
uniform int uHeightFogEnabled;
uniform float uHeightFogBaseHeight;
uniform float uHeightFogDensity;
uniform float uHeightFogFalloff;
uniform int uHeightFogStepCount;

struct DirLight {
  vec3 direction;
  vec3 color;
  float intensity;
};
uniform int u_dirLightCount;
uniform DirLight u_dirLights[MAX_DIR_LIGHTS];

struct PointLight {
  vec3 position;
  vec3 color;
  float intensity;
  float radius;
};
uniform int u_pointLightCount;
uniform PointLight u_pointLights[MAX_POINT_LIGHTS];

struct SpotLight {
  vec3 position;
  vec3 direction;
  vec3 color;
  float intensity;
  float radius;
  float innerCone;
  float outerCone;
};
uniform int u_spotLightCount;
uniform SpotLight u_spotLights[MAX_SPOT_LIGHTS];

uniform int uShadowEnabled;
uniform sampler2D uShadowMap[SHADOW_CASCADE_COUNT];
uniform mat4 uShadowMatrix[SHADOW_CASCADE_COUNT];
uniform float uCascadeSplit[SHADOW_CASCADE_COUNT];

uniform int uSpotShadowEnabled;
uniform sampler2D uSpotShadowMap[MAX_SPOT_SHADOW_LIGHTS];
uniform mat4 uSpotShadowMatrix[MAX_SPOT_SHADOW_LIGHTS];
uniform int uSpotShadowLightIdx[MAX_SPOT_SHADOW_LIGHTS];

uniform int uPointShadowEnabled;
uniform samplerCube uPointShadowMap[MAX_POINT_SHADOW_LIGHTS];
uniform vec3 uPointShadowLightPos[MAX_POINT_SHADOW_LIGHTS];
uniform float uPointShadowFarPlane[MAX_POINT_SHADOW_LIGHTS];
uniform int uPointShadowLightIdx[MAX_POINT_SHADOW_LIGHTS];

out vec4 outColor;

float distribution_ggx(vec3 N, vec3 H, float roughness) {
  float a = roughness * roughness;
  float a2 = a * a;
  float NdotH = max(dot(N, H), 0.0);
  float NdotH2 = NdotH * NdotH;
  float denom = NdotH2 * (a2 - 1.0) + 1.0;
  denom = PI * denom * denom;
  return a2 / max(denom, 0.0001);
}

float geometry_schlick_ggx(float NdotV, float roughness) {
  float r = roughness + 1.0;
  float k = (r * r) / 8.0;
  return NdotV / (NdotV * (1.0 - k) + k);
}

float geometry_smith(vec3 N, vec3 V, vec3 L, float roughness) {
  float NdotV = max(dot(N, V), 0.0);
  float NdotL = max(dot(N, L), 0.0);
  return geometry_schlick_ggx(NdotV, roughness) *
         geometry_schlick_ggx(NdotL, roughness);
}

vec3 fresnel_schlick(float cosTheta, vec3 F0) {
  return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 cook_torrance(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo,
                   float metallic, float roughness, vec3 F0) {
  vec3 H = normalize(V + L);
  float D = distribution_ggx(N, H, roughness);
  float G = geometry_smith(N, V, L, roughness);
  vec3 F = fresnel_schlick(max(dot(H, V), 0.0), F0);

  vec3 numerator = D * G * F;
  float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0);
  vec3 specular = numerator / max(denominator, 0.001);
  vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
  float NdotL = max(dot(N, L), 0.0);
  return (kD * albedo / PI + specular) * radiance * NdotL;
}

float sample_shadow_pcf(sampler2D shadowMap, vec3 projCoords) {
  if (projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 ||
      projCoords.y < 0.0 || projCoords.y > 1.0) {
    return 1.0;
  }

  float shadow = 0.0;
  vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
  for (int x = -1; x <= 1; ++x) {
    for (int y = -1; y <= 1; ++y) {
      vec2 offset = vec2(float(x), float(y)) * texelSize;
      float pcfDepth = texture(shadowMap, projCoords.xy + offset).r;
      shadow += (projCoords.z - 0.002) > pcfDepth ? 0.0 : 1.0;
    }
  }
  return shadow / 9.0;
}

float sample_directional_shadow_pcf(int cascadeIdx, vec3 projCoords) {
  if (cascadeIdx == 0) {
    return sample_shadow_pcf(uShadowMap[0], projCoords);
  }
  if (cascadeIdx == 1) {
    return sample_shadow_pcf(uShadowMap[1], projCoords);
  }
  if (cascadeIdx == 2) {
    return sample_shadow_pcf(uShadowMap[2], projCoords);
  }
  return sample_shadow_pcf(uShadowMap[3], projCoords);
}

float sample_spot_shadow_pcf(int shadowIdx, vec3 projCoords) {
  if (shadowIdx == 0) {
    return sample_shadow_pcf(uSpotShadowMap[0], projCoords);
  }
  if (shadowIdx == 1) {
    return sample_shadow_pcf(uSpotShadowMap[1], projCoords);
  }
  if (shadowIdx == 2) {
    return sample_shadow_pcf(uSpotShadowMap[2], projCoords);
  }
  return sample_shadow_pcf(uSpotShadowMap[3], projCoords);
}

float sample_point_shadow_depth(int shadowIdx, vec3 sampleVector) {
  if (shadowIdx == 0) {
    return texture(uPointShadowMap[0], sampleVector).r;
  }
  if (shadowIdx == 1) {
    return texture(uPointShadowMap[1], sampleVector).r;
  }
  if (shadowIdx == 2) {
    return texture(uPointShadowMap[2], sampleVector).r;
  }
  return texture(uPointShadowMap[3], sampleVector).r;
}

float compute_directional_shadow(vec3 worldPos) {
  if (uShadowEnabled == 0) {
    return 1.0;
  }

  float viewDepth = -(u_viewMatrix * vec4(worldPos, 1.0)).z;
  int cascadeIdx = SHADOW_CASCADE_COUNT - 1;
  for (int i = 0; i < SHADOW_CASCADE_COUNT; ++i) {
    if (viewDepth < uCascadeSplit[i]) {
      cascadeIdx = i;
      break;
    }
  }

  vec4 shadowCoord = uShadowMatrix[cascadeIdx] * vec4(worldPos, 1.0);
  vec3 projCoords = shadowCoord.xyz / shadowCoord.w;
  projCoords = projCoords * 0.5 + 0.5;
  float shadow = sample_directional_shadow_pcf(cascadeIdx, projCoords);

  float blendRange = uCascadeSplit[cascadeIdx] * 0.1;
  if (cascadeIdx < SHADOW_CASCADE_COUNT - 1 &&
      viewDepth > uCascadeSplit[cascadeIdx] - blendRange) {
    vec4 nextShadowCoord =
        uShadowMatrix[cascadeIdx + 1] * vec4(worldPos, 1.0);
    vec3 nextProjCoords = nextShadowCoord.xyz / nextShadowCoord.w;
    nextProjCoords = nextProjCoords * 0.5 + 0.5;
    float nextShadow =
        sample_directional_shadow_pcf(cascadeIdx + 1, nextProjCoords);
    float blendFactor =
        (viewDepth - (uCascadeSplit[cascadeIdx] - blendRange)) / blendRange;
    shadow = mix(shadow, nextShadow, clamp(blendFactor, 0.0, 1.0));
  }

  return shadow;
}

float compute_spot_shadow(vec3 worldPos, int lightIdx) {
  if (uSpotShadowEnabled == 0) {
    return 1.0;
  }

  for (int s = 0; s < MAX_SPOT_SHADOW_LIGHTS; ++s) {
    if (uSpotShadowLightIdx[s] != lightIdx) {
      continue;
    }
    vec4 shadowCoord = uSpotShadowMatrix[s] * vec4(worldPos, 1.0);
    vec3 projCoords = shadowCoord.xyz / shadowCoord.w;
    projCoords = projCoords * 0.5 + 0.5;
    return sample_spot_shadow_pcf(s, projCoords);
  }

  return 1.0;
}

float compute_point_shadow(vec3 worldPos, int lightIdx) {
  if (uPointShadowEnabled == 0) {
    return 1.0;
  }

  for (int s = 0; s < MAX_POINT_SHADOW_LIGHTS; ++s) {
    if (uPointShadowLightIdx[s] != lightIdx) {
      continue;
    }

    vec3 fragToLight = worldPos - uPointShadowLightPos[s];
    float currentDist = length(fragToLight);
    float normalizedDist = currentDist / uPointShadowFarPlane[s];
    vec3 sampleOffsets[20] = vec3[](
        vec3(1, 1, 1), vec3(1, -1, 1), vec3(-1, -1, 1),
        vec3(-1, 1, 1), vec3(1, 1, -1), vec3(1, -1, -1),
        vec3(-1, -1, -1), vec3(-1, 1, -1), vec3(1, 1, 0),
        vec3(1, -1, 0), vec3(-1, -1, 0), vec3(-1, 1, 0),
        vec3(1, 0, 1), vec3(-1, 0, 1), vec3(1, 0, -1),
        vec3(-1, 0, -1), vec3(0, 1, 1), vec3(0, -1, 1),
        vec3(0, -1, -1), vec3(0, 1, -1));

    float shadow = 0.0;
    float diskRadius = 0.02;
    float bias = 0.005;
    for (int i = 0; i < 20; ++i) {
      vec3 offset = sampleOffsets[i] * diskRadius;
      float closestDepth =
          sample_point_shadow_depth(s, fragToLight + offset);
      shadow += (normalizedDist - bias > closestDepth) ? 0.0 : 1.0;
    }
    return shadow / 20.0;
  }

  return 1.0;
}

float compute_distance_fog_factor(float distanceToCamera) {
  if (uFogMode == 1) {
    float range = max(uFogEnd - uFogStart, 0.001);
    return clamp((distanceToCamera - uFogStart) / range, 0.0, 1.0);
  }
  if (uFogMode == 2) {
    float densityDistance = max(uFogDensity, 0.0) * distanceToCamera;
    return clamp(1.0 - exp(-densityDistance), 0.0, 1.0);
  }
  if (uFogMode == 3) {
    float densityDistance = max(uFogDensity, 0.0) * distanceToCamera;
    return clamp(1.0 - exp(-(densityDistance * densityDistance)), 0.0, 1.0);
  }
  return 0.0;
}

float compute_height_fog_density(vec3 worldPos) {
  float heightAboveBase = max(worldPos.y - uHeightFogBaseHeight, 0.0);
  float falloff = max(uHeightFogFalloff, 0.001);
  return max(uHeightFogDensity, 0.0) * exp(-heightAboveBase * falloff);
}

float compute_height_fog_factor(vec3 cameraPos, vec3 worldPos) {
  if (uHeightFogEnabled == 0 || uHeightFogDensity <= 0.0) {
    return 0.0;
  }

  vec3 ray = worldPos - cameraPos;
  float rayLength = length(ray);
  if (rayLength <= 0.001) {
    return 0.0;
  }

  int stepCount = clamp(uHeightFogStepCount, 1, 64);
  vec3 stepVector = ray / float(stepCount);
  float stepLength = rayLength / float(stepCount);
  float opticalDepth = 0.0;

  for (int i = 0; i < 64; ++i) {
    if (i >= stepCount) {
      break;
    }
    vec3 samplePos = cameraPos + stepVector * (float(i) + 0.5);
    opticalDepth += compute_height_fog_density(samplePos) * stepLength;
  }

  return clamp(1.0 - exp(-opticalDepth), 0.0, 1.0);
}

float combine_fog_factors(float distanceFog, float heightFog) {
  return clamp(1.0 - ((1.0 - distanceFog) * (1.0 - heightFog)), 0.0, 1.0);
}

void main() {
  vec3 N = normalize(vNormal);
  vec3 V = normalize(u_cameraPos - vWorldPos);

  vec3 albedo = u_albedo;
  if (u_hasAlbedoTexture != 0) {
    albedo *= texture(u_albedoMap, vTexCoord).rgb;
  }

  float roughness = clamp(u_roughness, 0.04, 1.0);
  float metallic = clamp(u_metallic, 0.0, 1.0);
  float opacity = clamp(u_opacity, 0.0, 1.0);
  vec3 F0 = mix(vec3(0.04), albedo, metallic);
  vec3 Lo = vec3(0.0);

  for (int i = 0; i < u_dirLightCount; ++i) {
    vec3 L = normalize(-u_dirLights[i].direction);
    vec3 radiance = u_dirLights[i].color * u_dirLights[i].intensity;
    float shadow = (i == 0) ? compute_directional_shadow(vWorldPos) : 1.0;
    Lo += cook_torrance(N, V, L, radiance, albedo, metallic, roughness, F0) *
          shadow;
  }

  for (int i = 0; i < u_pointLightCount; ++i) {
    vec3 lightVec = u_pointLights[i].position - vWorldPos;
    float dist = length(lightVec);
    float radius = max(u_pointLights[i].radius, 0.001);
    if (dist > radius) {
      continue;
    }
    vec3 L = lightVec / max(dist, 0.0001);
    float atten = clamp(1.0 - (dist * dist) / (radius * radius), 0.0, 1.0);
    atten *= atten;
    vec3 radiance = u_pointLights[i].color * u_pointLights[i].intensity *
                    atten;
    Lo += cook_torrance(N, V, L, radiance, albedo, metallic, roughness, F0) *
          compute_point_shadow(vWorldPos, i);
  }

  for (int i = 0; i < u_spotLightCount; ++i) {
    vec3 lightVec = u_spotLights[i].position - vWorldPos;
    float dist = length(lightVec);
    float radius = max(u_spotLights[i].radius, 0.001);
    if (dist > radius) {
      continue;
    }
    vec3 L = lightVec / max(dist, 0.0001);
    float atten = clamp(1.0 - (dist * dist) / (radius * radius), 0.0, 1.0);
    atten *= atten;

    vec3 spotDir = normalize(u_spotLights[i].direction);
    float theta = dot(L, -spotDir);
    float epsilon = u_spotLights[i].innerCone - u_spotLights[i].outerCone;
    float spotFactor =
        clamp((theta - u_spotLights[i].outerCone) / max(epsilon, 0.0001),
              0.0, 1.0);
    vec3 radiance = u_spotLights[i].color * u_spotLights[i].intensity * atten *
                    spotFactor;
    Lo += cook_torrance(N, V, L, radiance, albedo, metallic, roughness, F0) *
          compute_spot_shadow(vWorldPos, i);
  }

  vec3 ambient = vec3(0.03) * albedo;
  vec3 color = ambient + Lo;
  float distanceFog = compute_distance_fog_factor(length(u_cameraPos - vWorldPos));
  float heightFog = compute_height_fog_factor(u_cameraPos, vWorldPos);
  float fogFactor = combine_fog_factors(distanceFog, heightFog);
  outColor = vec4(mix(color, uFogColor, fogFactor), opacity);
}
