$input v_worldpos, v_normal, v_texcoord0

// PBR forward fragment stage (bgfx port of pbr.frag, #138): Cook-
// Torrance direct lighting over the flat light-array vocabulary, the
// five material texture slots (baked stages 0-4 matching the flush's
// unit assignment), alpha modes, and distance/height fog. The PBR_FULL
// variant adds cascade/spot/point shadow sampling and split-sum IBL on
// the GL unit map (6-17, 19-21) — it needs 20 sampler units, so the
// runtime selects it only when caps.maxTextureSamplers covers the map
// (WebGL2's 16-unit floor keeps this default: shadow=1 and constant
// ambient). Scalar and integer GL uniforms become vec4 read through .x.

#include <bgfx_shader.sh>

// Clip-depth convention: the GLSL-family profiles (glsl/essl) run on GL
// APIs with NDC z in [-1, 1]; every other profile (spirv/metal) runs a
// zero-to-one API. The CPU builds matching projection matrices
// (DeviceCaps::depthZeroToOne), so depth<->NDC mapping keys off the
// shader language.
#if BGFX_SHADER_LANGUAGE_GLSL
#define ENGINE_DEPTH_TO_NDC(d) ((d) * 2.0 - 1.0)
#define ENGINE_CLIP_Z_TO_DEPTH(z) ((z) * 0.5 + 0.5)
#else
#define ENGINE_DEPTH_TO_NDC(d) (d)
#define ENGINE_CLIP_Z_TO_DEPTH(z) (z)
#endif

#define MAX_DIR_LIGHTS 4
#define MAX_POINT_LIGHTS 8
#define MAX_SPOT_LIGHTS 8

SAMPLER2D(u_albedoMap, 0);
SAMPLER2D(u_metallicRoughnessMap, 1);
SAMPLER2D(u_emissiveMap, 2);
SAMPLER2D(u_occlusionMap, 3);
SAMPLER2D(u_opacityMap, 4);

uniform vec4 u_albedo;             // .xyz
uniform vec4 u_emissive;           // .xyz
uniform vec4 u_roughness;          // .x
uniform vec4 u_metallic;           // .x
uniform vec4 u_opacity;            // .x
uniform vec4 u_cameraPos;          // .xyz
uniform vec4 u_cameraForwardOrtho; // xyz forward, w 1 when orthographic
uniform vec4 u_hasAlbedoTexture;   // .x
uniform vec4 u_hasMetallicRoughnessTexture;
uniform vec4 u_hasEmissiveTexture;
uniform vec4 u_hasOcclusionTexture;
uniform vec4 u_hasOpacityTexture;
uniform vec4 u_alphaMode;          // .x: 0 opaque, 1 mask, 2 blend
uniform vec4 u_alphaCutoff;        // .x
uniform vec4 u_uvTiling;           // .xy
uniform vec4 u_uvOffset;           // .xy

uniform vec4 uFogMode;             // .x
uniform vec4 uFogStart;            // .x
uniform vec4 uFogEnd;              // .x
uniform vec4 uFogDensity;          // .x
uniform vec4 uFogColor;            // .xyz
uniform vec4 uHeightFogEnabled;    // .x
uniform vec4 uHeightFogBaseHeight; // .x
uniform vec4 uHeightFogDensity;    // .x
uniform vec4 uHeightFogFalloff;    // .x
uniform vec4 uHeightFogStepCount;  // .x

uniform vec4 u_dirLightCount;      // .x
uniform vec4 u_dirLightDirection[MAX_DIR_LIGHTS];
uniform vec4 u_dirLightColorIntensity[MAX_DIR_LIGHTS];
uniform vec4 u_pointLightCount;    // .x
uniform vec4 u_pointLightPosRadius[MAX_POINT_LIGHTS];
uniform vec4 u_pointLightColorIntensity[MAX_POINT_LIGHTS];
uniform vec4 u_spotLightCount;     // .x
uniform vec4 u_spotLightPosRadius[MAX_SPOT_LIGHTS];
uniform vec4 u_spotLightDirInner[MAX_SPOT_LIGHTS];
uniform vec4 u_spotLightColorIntensity[MAX_SPOT_LIGHTS];
uniform vec4 u_spotLightParams[MAX_SPOT_LIGHTS];

#if PBR_FULL
#define SHADOW_CASCADE_COUNT 4
#define MAX_SPOT_SHADOW_LIGHTS 4
#define MAX_POINT_SHADOW_LIGHTS 4

SAMPLER2D(uShadowMap0, 6);
SAMPLER2D(uShadowMap1, 7);
SAMPLER2D(uShadowMap2, 8);
SAMPLER2D(uShadowMap3, 9);
SAMPLER2D(uSpotShadowMap0, 10);
SAMPLER2D(uSpotShadowMap1, 11);
SAMPLER2D(uSpotShadowMap2, 12);
SAMPLER2D(uSpotShadowMap3, 13);
SAMPLERCUBE(uPointShadowMap0, 14);
SAMPLERCUBE(uPointShadowMap1, 15);
SAMPLERCUBE(uPointShadowMap2, 16);
SAMPLERCUBE(uPointShadowMap3, 17);
SAMPLERCUBE(uIrradianceMap, 19);
SAMPLERCUBE(uPrefilteredMap, 20);
SAMPLER2D(uBrdfLut, 21);

uniform mat4 u_viewMatrix; // cascade selection needs the view depth
uniform vec4 uShadowEnabled;          // .x
uniform mat4 uShadowMatrix[SHADOW_CASCADE_COUNT];
uniform vec4 uCascadeSplits;
uniform vec4 uSpotShadowEnabled;      // .x
uniform mat4 uSpotShadowMatrix[MAX_SPOT_SHADOW_LIGHTS];
uniform vec4 uSpotShadowLightIdxVec;
uniform vec4 uPointShadowEnabled;     // .x
uniform vec4 uPointShadowPosFar[MAX_POINT_SHADOW_LIGHTS];
uniform vec4 uPointShadowLightIdxVec;
uniform vec4 uIblEnabled;             // .x
uniform vec4 uPrefilteredMips;        // .x

// 3x3 PCF texel steps mirror kShadowMapResolution (2048) and
// kSpotShadowMapResolution (1024) — constants because HLSL-path
// shaderc lacks textureSize (same shape as deferred_lighting.fs.sc).
// Shadow samples use explicit LOD 0: the maps are single-mip, so the
// texel fetched is identical, and the dx11 profile's fxc rejects
// gradient sampling inside the dynamic light loops that reach these
// helpers (X3511 unroll explosion otherwise).
#define CASCADE_SHADOW_TEXEL (1.0 / 2048.0)
#define SPOT_SHADOW_TEXEL (1.0 / 1024.0)

float sample_shadow_pcf_at(vec2 uv, float compareDepth, int mapIdx) {
    if (mapIdx == 0) {
        return ((compareDepth - 0.002) >
                texture2DLod(uShadowMap0, uv, 0.0).r) ? 0.0 : 1.0;
    }
    if (mapIdx == 1) {
        return ((compareDepth - 0.002) >
                texture2DLod(uShadowMap1, uv, 0.0).r) ? 0.0 : 1.0;
    }
    if (mapIdx == 2) {
        return ((compareDepth - 0.002) >
                texture2DLod(uShadowMap2, uv, 0.0).r) ? 0.0 : 1.0;
    }
    return ((compareDepth - 0.002) >
            texture2DLod(uShadowMap3, uv, 0.0).r) ? 0.0 : 1.0;
}

float sample_spot_shadow_at(vec2 uv, float compareDepth, int mapIdx) {
    if (mapIdx == 0) {
        return ((compareDepth - 0.002) >
                texture2DLod(uSpotShadowMap0, uv, 0.0).r) ? 0.0 : 1.0;
    }
    if (mapIdx == 1) {
        return ((compareDepth - 0.002) >
                texture2DLod(uSpotShadowMap1, uv, 0.0).r) ? 0.0 : 1.0;
    }
    if (mapIdx == 2) {
        return ((compareDepth - 0.002) >
                texture2DLod(uSpotShadowMap2, uv, 0.0).r) ? 0.0 : 1.0;
    }
    return ((compareDepth - 0.002) >
            texture2DLod(uSpotShadowMap3, uv, 0.0).r) ? 0.0 : 1.0;
}

float shadow_pcf(vec3 projCoords, int mapIdx, bool spot) {
    if (projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0) {
        return 1.0;
    }
    float shadow = 0.0;
    float texel = spot ? SPOT_SHADOW_TEXEL : CASCADE_SHADOW_TEXEL;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec2 uv = projCoords.xy +
                      vec2(float(x), float(y)) * texel;
            shadow += spot
                          ? sample_spot_shadow_at(uv, projCoords.z, mapIdx)
                          : sample_shadow_pcf_at(uv, projCoords.z, mapIdx);
        }
    }
    return shadow / 9.0;
}

float sample_point_shadow_depth(int shadowIdx, vec3 sampleVector) {
    if (shadowIdx == 0) {
        return textureCubeLod(uPointShadowMap0, sampleVector, 0.0).r;
    }
    if (shadowIdx == 1) {
        return textureCubeLod(uPointShadowMap1, sampleVector, 0.0).r;
    }
    if (shadowIdx == 2) {
        return textureCubeLod(uPointShadowMap2, sampleVector, 0.0).r;
    }
    return textureCubeLod(uPointShadowMap3, sampleVector, 0.0).r;
}

// Forward path selects the cascade from the fragment's view-space
// depth (the GL pbr.frag shape; deferred linearizes the depth buffer
// instead).
float compute_directional_shadow(vec3 worldPos) {
    if (uShadowEnabled.x == 0.0) {
        return 1.0;
    }
    float viewDepth = -(mul(u_viewMatrix, vec4(worldPos, 1.0))).z;
    int cascadeIdx = SHADOW_CASCADE_COUNT - 1;
    for (int i = 0; i < SHADOW_CASCADE_COUNT; ++i) {
        if (viewDepth < uCascadeSplits[i]) {
            cascadeIdx = i;
            break;
        }
    }
    vec4 shadowCoord = mul(uShadowMatrix[cascadeIdx], vec4(worldPos, 1.0));
    vec3 projCoords = shadowCoord.xyz / shadowCoord.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;
    projCoords.z = ENGINE_CLIP_Z_TO_DEPTH(projCoords.z);
    float shadow = shadow_pcf(projCoords, cascadeIdx, false);

    float blendRange = uCascadeSplits[cascadeIdx] * 0.1;
    if (cascadeIdx < SHADOW_CASCADE_COUNT - 1 &&
        viewDepth > uCascadeSplits[cascadeIdx] - blendRange) {
        vec4 nextShadowCoord =
            mul(uShadowMatrix[cascadeIdx + 1], vec4(worldPos, 1.0));
        vec3 nextProjCoords = nextShadowCoord.xyz / nextShadowCoord.w;
        nextProjCoords.xy = nextProjCoords.xy * 0.5 + 0.5;
        nextProjCoords.z = ENGINE_CLIP_Z_TO_DEPTH(nextProjCoords.z);
        float nextShadow = shadow_pcf(nextProjCoords, cascadeIdx + 1, false);
        float blendFactor =
            (viewDepth - (uCascadeSplits[cascadeIdx] - blendRange)) /
            blendRange;
        shadow = mix(shadow, nextShadow, clamp(blendFactor, 0.0, 1.0));
    }
    return shadow;
}

float compute_spot_shadow(vec3 worldPos, int lightIdx) {
    if (uSpotShadowEnabled.x == 0.0) {
        return 1.0;
    }
    for (int s = 0; s < MAX_SPOT_SHADOW_LIGHTS; ++s) {
        if (int(round(uSpotShadowLightIdxVec[s])) != lightIdx) {
            continue;
        }
        vec4 shadowCoord = mul(uSpotShadowMatrix[s], vec4(worldPos, 1.0));
        vec3 projCoords = shadowCoord.xyz / shadowCoord.w;
        projCoords.xy = projCoords.xy * 0.5 + 0.5;
    projCoords.z = ENGINE_CLIP_Z_TO_DEPTH(projCoords.z);
        return shadow_pcf(projCoords, s, true);
    }
    return 1.0;
}

float compute_point_shadow(vec3 worldPos, int lightIdx) {
    if (uPointShadowEnabled.x == 0.0) {
        return 1.0;
    }
    for (int s = 0; s < MAX_POINT_SHADOW_LIGHTS; ++s) {
        if (int(round(uPointShadowLightIdxVec[s])) != lightIdx) {
            continue;
        }
        vec3 fragToLight = worldPos - uPointShadowPosFar[s].xyz;
        float currentDist = length(fragToLight);
        float normalizedDist = currentDist / uPointShadowPosFar[s].w;
        float shadow = 0.0;
        float diskRadius = 0.02;
        for (int i = 0; i < 20; ++i) {
            vec3 offsets[20];
            offsets[0] = vec3(1, 1, 1);   offsets[1] = vec3(1, -1, 1);
            offsets[2] = vec3(-1, -1, 1); offsets[3] = vec3(-1, 1, 1);
            offsets[4] = vec3(1, 1, -1);  offsets[5] = vec3(1, -1, -1);
            offsets[6] = vec3(-1, -1, -1); offsets[7] = vec3(-1, 1, -1);
            offsets[8] = vec3(1, 1, 0);   offsets[9] = vec3(1, -1, 0);
            offsets[10] = vec3(-1, -1, 0); offsets[11] = vec3(-1, 1, 0);
            offsets[12] = vec3(1, 0, 1);  offsets[13] = vec3(-1, 0, 1);
            offsets[14] = vec3(1, 0, -1); offsets[15] = vec3(-1, 0, -1);
            offsets[16] = vec3(0, 1, 1);  offsets[17] = vec3(0, -1, 1);
            offsets[18] = vec3(0, -1, -1); offsets[19] = vec3(0, 1, -1);
            float closestDepth = sample_point_shadow_depth(
                s, fragToLight + offsets[i] * diskRadius);
            shadow += (normalizedDist - 0.005 > closestDepth) ? 0.0 : 1.0;
        }
        return shadow / 20.0;
    }
    return 1.0;
}

vec3 fresnel_schlick_roughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3_splat(1.0 - roughness), F0) - F0) *
                pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Split-sum IBL ambient: irradiance-lit diffuse plus prefiltered
// specular weighted by the BRDF integration LUT.
vec3 ibl_ambient(vec3 N, vec3 V, vec3 albedo, float metallic,
                 float roughness) {
    vec3 F0 = mix(vec3_splat(0.04), albedo, metallic);
    float NdotV = max(dot(N, V), 0.0);
    vec3 F = fresnel_schlick_roughness(NdotV, F0, roughness);
    vec3 kD = (vec3_splat(1.0) - F) * (1.0 - metallic);
    vec3 diffuse = textureCube(uIrradianceMap, N).rgb * albedo;
    vec3 R = reflect(-V, N);
    vec3 prefiltered =
        textureCubeLod(uPrefilteredMap, R,
                       roughness * max(uPrefilteredMips.x - 1.0, 0.0)).rgb;
    vec2 brdf = texture2D(uBrdfLut, vec2(NdotV, roughness)).rg;
    return kD * diffuse + prefiltered * (F * brdf.x + brdf.y);
}
#endif // PBR_FULL

float distribution_ggx(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = 3.14159265359 * denom * denom;
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
    return F0 + (vec3_splat(1.0) - F0) *
                    pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
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
    vec3 kD = (vec3_splat(1.0) - F) * (1.0 - metallic);
    float NdotL = max(dot(N, L), 0.0);
    return (kD * albedo / 3.14159265359 + specular) * radiance * NdotL;
}

float compute_distance_fog_factor(float distanceToCamera) {
    int mode = int(uFogMode.x);
    if (mode == 1) {
        float range = max(uFogEnd.x - uFogStart.x, 0.001);
        return clamp((distanceToCamera - uFogStart.x) / range, 0.0, 1.0);
    }
    if (mode == 2) {
        float densityDistance = max(uFogDensity.x, 0.0) * distanceToCamera;
        return clamp(1.0 - exp(-densityDistance), 0.0, 1.0);
    }
    if (mode == 3) {
        float densityDistance = max(uFogDensity.x, 0.0) * distanceToCamera;
        return clamp(1.0 - exp(-(densityDistance * densityDistance)), 0.0,
                     1.0);
    }
    return 0.0;
}

float compute_height_fog_density(vec3 worldPos) {
    float heightAboveBase = max(worldPos.y - uHeightFogBaseHeight.x, 0.0);
    float falloff = max(uHeightFogFalloff.x, 0.001);
    return max(uHeightFogDensity.x, 0.0) * exp(-heightAboveBase * falloff);
}

float compute_height_fog_factor(vec3 cameraPos, vec3 worldPos) {
    if ((uHeightFogEnabled.x == 0.0) || (uHeightFogDensity.x <= 0.0)) {
        return 0.0;
    }
    vec3 ray = worldPos - cameraPos;
    float rayLength = length(ray);
    if (rayLength <= 0.001) {
        return 0.0;
    }
    int stepCount = clamp(int(uHeightFogStepCount.x), 1, 64);
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

void main() {
    vec2 uv = v_texcoord0 * u_uvTiling.xy + u_uvOffset.xy;
    vec3 N = normalize(v_normal);
    vec3 V = mix(normalize(u_cameraPos.xyz - v_worldpos),
                 -u_cameraForwardOrtho.xyz, u_cameraForwardOrtho.w);

    vec3 albedo = u_albedo.xyz;
    if (u_hasAlbedoTexture.x != 0.0) {
        albedo *= texture2D(u_albedoMap, uv).rgb;
    }

    float roughnessFactor = u_roughness.x;
    float metallicFactor = u_metallic.x;
    if (u_hasMetallicRoughnessTexture.x != 0.0) {
        vec3 mr = texture2D(u_metallicRoughnessMap, uv).rgb;
        roughnessFactor *= mr.g;
        metallicFactor *= mr.b;
    }
    float roughness = clamp(roughnessFactor, 0.04, 1.0);
    float metallic = clamp(metallicFactor, 0.0, 1.0);

    float ao = 1.0;
    if (u_hasOcclusionTexture.x != 0.0) {
        ao = texture2D(u_occlusionMap, uv).r;
    }

    vec3 emissive = u_emissive.xyz;
    if (u_hasEmissiveTexture.x != 0.0) {
        emissive *= texture2D(u_emissiveMap, uv).rgb;
    }

    float maskAlpha = 1.0;
    if (u_hasOpacityTexture.x != 0.0) {
        maskAlpha = texture2D(u_opacityMap, uv).r;
    }
    if ((int(u_alphaMode.x) == 1) && (maskAlpha < u_alphaCutoff.x)) {
        discard;
    }
    float opacity = clamp(u_opacity.x, 0.0, 1.0);
    vec3 F0 = mix(vec3_splat(0.04), albedo, metallic);
    vec3 Lo = vec3_splat(0.0);

    int dirCount = int(u_dirLightCount.x);
    for (int i = 0; i < MAX_DIR_LIGHTS; ++i) {
        if (i >= dirCount) {
            break;
        }
        vec3 L = normalize(-u_dirLightDirection[i].xyz);
        vec3 radiance = u_dirLightColorIntensity[i].rgb *
                        u_dirLightColorIntensity[i].w;
        float shadow = 1.0;
#if PBR_FULL
        // Only the primary directional light casts cascades, matching
        // the GL pbr.frag contract.
        shadow = (i == 0) ? compute_directional_shadow(v_worldpos) : 1.0;
#endif
        Lo += cook_torrance(N, V, L, radiance, albedo, metallic, roughness,
                            F0) * shadow;
    }

    int pointCount = int(u_pointLightCount.x);
    for (int i = 0; i < MAX_POINT_LIGHTS; ++i) {
        if (i >= pointCount) {
            break;
        }
        vec3 lightVec = u_pointLightPosRadius[i].xyz - v_worldpos;
        float dist = length(lightVec);
        float radius = max(u_pointLightPosRadius[i].w, 0.001);
        if (dist > radius) {
            continue;
        }
        vec3 L = lightVec / max(dist, 0.0001);
        float atten =
            clamp(1.0 - (dist * dist) / (radius * radius), 0.0, 1.0);
        atten *= atten;
        vec3 radiance = u_pointLightColorIntensity[i].rgb *
                        u_pointLightColorIntensity[i].w * atten;
        float shadow = 1.0;
#if PBR_FULL
        shadow = compute_point_shadow(v_worldpos, i);
#endif
        Lo += cook_torrance(N, V, L, radiance, albedo, metallic, roughness,
                            F0) * shadow;
    }

    int spotCount = int(u_spotLightCount.x);
    for (int i = 0; i < MAX_SPOT_LIGHTS; ++i) {
        if (i >= spotCount) {
            break;
        }
        vec3 lightVec = u_spotLightPosRadius[i].xyz - v_worldpos;
        float dist = length(lightVec);
        float radius = max(u_spotLightPosRadius[i].w, 0.001);
        if (dist > radius) {
            continue;
        }
        vec3 L = lightVec / max(dist, 0.0001);
        float atten =
            clamp(1.0 - (dist * dist) / (radius * radius), 0.0, 1.0);
        atten *= atten;
        vec3 spotDir = normalize(u_spotLightDirInner[i].xyz);
        float theta = dot(L, -spotDir);
        float innerCone = u_spotLightDirInner[i].w;
        float outerCone = u_spotLightParams[i].x;
        float epsilon = innerCone - outerCone;
        float spotFactor =
            clamp((theta - outerCone) / max(epsilon, 0.0001), 0.0, 1.0);
        vec3 radiance = u_spotLightColorIntensity[i].rgb *
                        u_spotLightColorIntensity[i].w * atten * spotFactor;
        float shadow = 1.0;
#if PBR_FULL
        shadow = compute_spot_shadow(v_worldpos, i);
#endif
        Lo += cook_torrance(N, V, L, radiance, albedo, metallic, roughness,
                            F0) * shadow;
    }

    vec3 ambient = vec3_splat(0.03) * albedo * ao;
#if PBR_FULL
    if (uIblEnabled.x != 0.0) {
        ambient = ibl_ambient(N, V, albedo, metallic, roughness) * ao;
    }
#endif
    vec3 color = ambient + Lo + emissive;
    float distanceFog =
        compute_distance_fog_factor(length(u_cameraPos.xyz - v_worldpos));
    float heightFog = compute_height_fog_factor(u_cameraPos.xyz, v_worldpos);
    float fogFactor =
        clamp(1.0 - ((1.0 - distanceFog) * (1.0 - heightFog)), 0.0, 1.0);
    gl_FragColor = vec4(mix(color, uFogColor.xyz, fogFactor), opacity);
}
