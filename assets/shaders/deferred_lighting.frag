// Defines the deferred lighting fragment shader used by the Engine renderer.

#version 330 core

in vec2 vTexCoord;

out vec4 FragColor;

// G-Buffer samplers.
uniform sampler2D uGBufferAlbedo;
uniform sampler2D uGBufferNormal;
uniform sampler2D uGBufferEmissive;
uniform sampler2D uGBufferDepth;

// SSAO.
uniform sampler2D uSsaoTexture;
uniform int uSsaoEnabled;

// Shadow maps (4 cascades).
uniform sampler2D uShadowMap[4];
uniform mat4 uShadowMatrix[4];
uniform float uCascadeSplit[4];
uniform int uShadowEnabled;

// Spot light shadow maps (up to 4 shadow-casting spots).
#define MAX_SPOT_SHADOW_LIGHTS 4
uniform int uSpotShadowEnabled;
uniform sampler2D uSpotShadowMap[MAX_SPOT_SHADOW_LIGHTS];
uniform mat4 uSpotShadowMatrix[MAX_SPOT_SHADOW_LIGHTS];
uniform int uSpotShadowLightIdx[MAX_SPOT_SHADOW_LIGHTS];

// Point light cubemap shadow maps (up to 4 shadow-casting points).
#define MAX_POINT_SHADOW_LIGHTS 4
uniform int uPointShadowEnabled;
uniform samplerCube uPointShadowMap[MAX_POINT_SHADOW_LIGHTS];
uniform vec3 uPointShadowLightPos[MAX_POINT_SHADOW_LIGHTS];
uniform float uPointShadowFarPlane[MAX_POINT_SHADOW_LIGHTS];
uniform int uPointShadowLightIdx[MAX_POINT_SHADOW_LIGHTS];

// Tile light data (R32F texture: one row per tile laid out as
// [pointCount, pointIdx0..31, spotCount, spotIdx0..15]). The section sizes
// must match kMaxPointLightsPerTile/kMaxSpotLightsPerTile in light_culling.h.
#define TILE_MAX_POINT_LIGHTS 32
uniform sampler2D uTileLightTex;
uniform int uTileCountX;
uniform int uTileCountY;

// Inverse projection for depth reconstruction.
uniform mat4 uInvProjection;
uniform mat4 uInvView;

// Directional light.
uniform vec3 uDirLightDirection;
uniform vec3 uDirLightColor;

// Camera.
uniform vec3 uCameraPos;

// Screen dimensions for tile computation.
uniform vec2 uScreenSize;

// Distance fog.
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

// ---- Per-light data texture ----
// R32F texture, one row per light; fetched by tile light index so the shader
// needs no per-light uniform arrays (which exceed the NVIDIA fragment
// uniform register limit). Rows [0, 128): point light [posXYZ, colorRGB,
// intensity, radius]. Rows [128, 192): spot light [posXYZ, dirXYZ, colorRGB,
// intensity, radius, innerCone, outerCone]. Layout must match
// pack_light_data in light_culling.cpp.
#define LIGHT_DATA_SPOT_ROW 128
uniform sampler2D uLightDataTex;
uniform int uPointLightCount;
uniform int uSpotLightCount;

// Fetch one float from a light data row.
float light_data(int x, int row) {
    return texelFetch(uLightDataTex, ivec2(x, row), 0).r;
}

// Fetch three consecutive floats from a light data row.
vec3 light_data3(int x, int row) {
    return vec3(light_data(x, row), light_data(x + 1, row),
                light_data(x + 2, row));
}

// ---- PBR BRDF ----

const float PI = 3.14159265359;

/// Handles distribution ggx.
float distribution_ggx(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

/// Handles geometry schlick ggx.
float geometry_schlick_ggx(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

/// Handles geometry smith.
float geometry_smith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return geometry_schlick_ggx(NdotV, roughness) *
           geometry_schlick_ggx(NdotL, roughness);
}

/// Handles fresnel schlick.
vec3 fresnel_schlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Cook-Torrance specular BRDF evaluation for a single light.
vec3 cook_torrance(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic,
                   float roughness, vec3 lightColor, float lightIntensity) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return vec3(0.0);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    float D = distribution_ggx(N, H, roughness);
    float G = geometry_smith(N, V, L, roughness);
    vec3 F = fresnel_schlick(max(dot(H, V), 0.0), F0);

    vec3 numerator = D * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL + 0.0001;
    vec3 specular = numerator / denominator;

    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    return (kD * albedo / PI + specular) * lightColor * lightIntensity * NdotL;
}

// Reconstruct view-space position from depth buffer value.
vec3 reconstruct_world_pos(vec2 texCoord, float depth) {
    // depth is [0,1] from the depth texture.
    float z = depth * 2.0 - 1.0; // to NDC [-1,1]
    vec4 clipPos = vec4(texCoord * 2.0 - 1.0, z, 1.0);
    vec4 viewPos = uInvProjection * clipPos;
    viewPos /= viewPos.w;
    vec4 worldPos = uInvView * viewPos;
    return worldPos.xyz;
}

// Compute view-space depth for cascade selection.
float linearize_depth(float depth) {
    float z = depth * 2.0 - 1.0;
    vec4 clipPos = vec4(0.0, 0.0, z, 1.0);
    vec4 viewPos = uInvProjection * clipPos;
    return -viewPos.z / viewPos.w;
}

/// Handles compute distance fog factor.
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

/// Handles compute height fog density.
float compute_height_fog_density(vec3 worldPos) {
    float heightAboveBase = max(worldPos.y - uHeightFogBaseHeight, 0.0);
    float falloff = max(uHeightFogFalloff, 0.001);
    return max(uHeightFogDensity, 0.0) * exp(-heightAboveBase * falloff);
}

/// Handles compute height fog factor.
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

/// Handles combine fog factors.
float combine_fog_factors(float distanceFog, float heightFog) {
    return clamp(1.0 - ((1.0 - distanceFog) * (1.0 - heightFog)), 0.0, 1.0);
}

// PCF shadow sampling with 3x3 kernel.
float sample_shadow_pcf(sampler2D shadowMap, vec3 projCoords) {
    if (projCoords.z > 1.0) return 1.0;

    float shadow = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float pcfDepth = texture(shadowMap,
                projCoords.xy + vec2(float(x), float(y)) * texelSize).r;
            shadow += (projCoords.z - 0.002) > pcfDepth ? 0.0 : 1.0;
        }
    }
    return shadow / 9.0;
}

/// Handles sample directional shadow pcf.
float sample_directional_shadow_pcf(int cascadeIdx, vec3 projCoords) {
    if (cascadeIdx == 0) return sample_shadow_pcf(uShadowMap[0], projCoords);
    if (cascadeIdx == 1) return sample_shadow_pcf(uShadowMap[1], projCoords);
    if (cascadeIdx == 2) return sample_shadow_pcf(uShadowMap[2], projCoords);
    return sample_shadow_pcf(uShadowMap[3], projCoords);
}

/// Handles sample spot shadow pcf.
float sample_spot_shadow_pcf(int shadowIdx, vec3 projCoords) {
    if (shadowIdx == 0) return sample_shadow_pcf(uSpotShadowMap[0], projCoords);
    if (shadowIdx == 1) return sample_shadow_pcf(uSpotShadowMap[1], projCoords);
    if (shadowIdx == 2) return sample_shadow_pcf(uSpotShadowMap[2], projCoords);
    return sample_shadow_pcf(uSpotShadowMap[3], projCoords);
}

/// Handles sample point shadow depth.
float sample_point_shadow_depth(int shadowIdx, vec3 sampleVector) {
    if (shadowIdx == 0) return texture(uPointShadowMap[0], sampleVector).r;
    if (shadowIdx == 1) return texture(uPointShadowMap[1], sampleVector).r;
    if (shadowIdx == 2) return texture(uPointShadowMap[2], sampleVector).r;
    return texture(uPointShadowMap[3], sampleVector).r;
}

// Compute shadow factor for a world position using CSM.
float compute_shadow(vec3 worldPos, float depth) {
    if (uShadowEnabled == 0) return 1.0;

    float viewDepth = linearize_depth(depth);

    // Select cascade based on view-space depth.
    int cascadeIdx = 3;
    for (int i = 0; i < 4; ++i) {
        if (viewDepth < uCascadeSplit[i]) {
            cascadeIdx = i;
            break;
        }
    }

    // Project world position into shadow map space.
    vec4 shadowCoord = uShadowMatrix[cascadeIdx] * vec4(worldPos, 1.0);
    vec3 projCoords = shadowCoord.xyz / shadowCoord.w;
    projCoords = projCoords * 0.5 + 0.5; // [-1,1] → [0,1]

    // Blend between cascades at the boundary to reduce seams.
    float shadow = sample_directional_shadow_pcf(cascadeIdx, projCoords);

    // Cascade blending: blend with next cascade near the split boundary.
    float blendFactor = 0.0;
    float blendRange = uCascadeSplit[cascadeIdx] * 0.1;
    if (cascadeIdx < 3 && viewDepth > uCascadeSplit[cascadeIdx] - blendRange) {
        vec4 nextShadowCoord = uShadowMatrix[cascadeIdx + 1] * vec4(worldPos, 1.0);
        vec3 nextProjCoords = nextShadowCoord.xyz / nextShadowCoord.w;
        nextProjCoords = nextProjCoords * 0.5 + 0.5;
        float nextShadow = sample_directional_shadow_pcf(cascadeIdx + 1, nextProjCoords);
        blendFactor = (viewDepth - (uCascadeSplit[cascadeIdx] - blendRange)) / blendRange;
        shadow = mix(shadow, nextShadow, clamp(blendFactor, 0.0, 1.0));
    }

    return shadow;
}

// Compute spot light shadow factor for a world position.
float compute_spot_shadow(vec3 worldPos, int lightIdx) {
    if (uSpotShadowEnabled == 0) return 1.0;

    for (int s = 0; s < MAX_SPOT_SHADOW_LIGHTS; ++s) {
        if (uSpotShadowLightIdx[s] != lightIdx) continue;

        vec4 shadowCoord = uSpotShadowMatrix[s] * vec4(worldPos, 1.0);
        vec3 projCoords = shadowCoord.xyz / shadowCoord.w;
        projCoords = projCoords * 0.5 + 0.5;

        if (projCoords.z > 1.0) return 1.0;
        return sample_spot_shadow_pcf(s, projCoords);
    }

    return 1.0; // No shadow slot for this light.
}

// Compute point light shadow factor using cubemap sampling.
float compute_point_shadow(vec3 worldPos, int lightIdx) {
    if (uPointShadowEnabled == 0) return 1.0;

    for (int s = 0; s < MAX_POINT_SHADOW_LIGHTS; ++s) {
        if (uPointShadowLightIdx[s] != lightIdx) continue;

        vec3 fragToLight = worldPos - uPointShadowLightPos[s];
        float currentDist = length(fragToLight);
        float normalizedDist = currentDist / uPointShadowFarPlane[s];

        // PCF with 20 offset sample directions for soft cubemap shadows.
        vec3 sampleOffsets[20] = vec3[](
            vec3( 1,  1,  1), vec3( 1, -1,  1), vec3(-1, -1,  1), vec3(-1,  1,  1),
            vec3( 1,  1, -1), vec3( 1, -1, -1), vec3(-1, -1, -1), vec3(-1,  1, -1),
            vec3( 1,  1,  0), vec3( 1, -1,  0), vec3(-1, -1,  0), vec3(-1,  1,  0),
            vec3( 1,  0,  1), vec3(-1,  0,  1), vec3( 1,  0, -1), vec3(-1,  0, -1),
            vec3( 0,  1,  1), vec3( 0, -1,  1), vec3( 0, -1, -1), vec3( 0,  1, -1)
        );

        float shadow = 0.0;
        float diskRadius = 0.02;
        float bias = 0.005;
        for (int i = 0; i < 20; ++i) {
            float closestDepth = sample_point_shadow_depth(
                s, fragToLight + sampleOffsets[i] * diskRadius);
            shadow += (normalizedDist - bias > closestDepth) ? 0.0 : 1.0;
        }
        return shadow / 20.0;
    }

    return 1.0; // No shadow slot for this light.
}

/// Runs the shader entry point for this stage.
void main() {
    // Sample G-Buffer.
    vec4 albedoMetallic = texture(uGBufferAlbedo, vTexCoord);
    vec4 normalRoughness = texture(uGBufferNormal, vTexCoord);
    vec4 emissiveAO = texture(uGBufferEmissive, vTexCoord);
    float depth = texture(uGBufferDepth, vTexCoord).r;

    // Discard background fragments.
    if (depth >= 1.0) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 albedo = albedoMetallic.rgb;
    float metallic = albedoMetallic.a;
    vec3 N = normalize(normalRoughness.rgb * 2.0 - 1.0);
    float roughness = normalRoughness.a;
    vec3 emissive = emissiveAO.rgb;
    float ao = emissiveAO.a;

    vec3 worldPos = reconstruct_world_pos(vTexCoord, depth);
    vec3 V = normalize(uCameraPos - worldPos);

    // Accumulate lighting.
    vec3 Lo = vec3(0.0);

    // Directional light with shadow.
    vec3 L_dir = normalize(-uDirLightDirection);
    float shadowFactor = compute_shadow(worldPos, depth);
    Lo += cook_torrance(N, V, L_dir, albedo, metallic, roughness,
                        uDirLightColor, 1.0) * shadowFactor;

    // Determine tile index for culled light lookup.
    int tileX = int(gl_FragCoord.x) / 16;
    int tileY = int(gl_FragCoord.y) / 16;
    int tileIdx = tileY * uTileCountX + tileX;

    // Read tile point light count from tile texture.
    int tilePointCount = int(texelFetch(uTileLightTex, ivec2(0, tileIdx), 0).r);
    for (int i = 0; i < tilePointCount; ++i) {
        int lightIdx = int(texelFetch(uTileLightTex, ivec2(1 + i, tileIdx), 0).r);
        if (lightIdx < 0 || lightIdx >= uPointLightCount) continue;

        vec3 Lpos = light_data3(0, lightIdx);
        vec3 toLight = Lpos - worldPos;
        float dist = length(toLight);
        float radius = light_data(7, lightIdx);
        if (dist > radius) continue;

        vec3 L = toLight / dist;
        float attenuation = clamp(1.0 - (dist * dist) / (radius * radius), 0.0, 1.0);
        attenuation *= attenuation;

        Lo += cook_torrance(N, V, L, albedo, metallic, roughness,
                            light_data3(3, lightIdx),
                            light_data(6, lightIdx) * attenuation)
              * compute_point_shadow(worldPos, lightIdx);
    }

    // Read tile spot light count (stored after the point section).
    int spotOffset = TILE_MAX_POINT_LIGHTS + 1;
    int tileSpotCount = int(texelFetch(uTileLightTex, ivec2(spotOffset, tileIdx), 0).r);
    for (int i = 0; i < tileSpotCount; ++i) {
        int lightIdx = int(texelFetch(uTileLightTex, ivec2(spotOffset + 1 + i, tileIdx), 0).r);
        if (lightIdx < 0 || lightIdx >= uSpotLightCount) continue;

        int row = LIGHT_DATA_SPOT_ROW + lightIdx;
        vec3 Lpos = light_data3(0, row);
        vec3 toLight = Lpos - worldPos;
        float dist = length(toLight);
        float radius = light_data(10, row);
        if (dist > radius) continue;

        vec3 L = toLight / dist;
        float attenuation = clamp(1.0 - (dist * dist) / (radius * radius), 0.0, 1.0);
        attenuation *= attenuation;

        // Spot cone falloff.
        vec3 spotDir = normalize(light_data3(3, row));
        float theta = dot(L, -spotDir);
        float innerCone = light_data(11, row);
        float outerCone = light_data(12, row);
        float epsilon = innerCone - outerCone;
        float spotFactor = clamp((theta - outerCone) / max(epsilon, 0.0001), 0.0, 1.0);

        Lo += cook_torrance(N, V, L, albedo, metallic, roughness,
                            light_data3(6, row),
                            light_data(9, row) * attenuation * spotFactor)
              * compute_spot_shadow(worldPos, lightIdx);
    }

    // Ambient + emissive.
    float ssaoFactor = (uSsaoEnabled != 0) ? texture(uSsaoTexture, vTexCoord).r : 1.0;
    vec3 ambient = vec3(0.03) * albedo * ao * ssaoFactor;
    vec3 color = ambient + Lo + emissive;
    float distanceFog = compute_distance_fog_factor(length(uCameraPos - worldPos));
    float heightFog = compute_height_fog_factor(uCameraPos, worldPos);
    float fogFactor = combine_fog_factors(distanceFog, heightFog);
    color = mix(color, uFogColor, fogFactor);

    FragColor = vec4(color, 1.0);
}
