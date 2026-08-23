$input v_texcoord0

// Deferred lighting fragment stage (bgfx port of deferred_lighting.frag,
// #138): Cook-Torrance shading of the G-buffer with tile-culled point/
// spot lights fetched from the R32F light-data and tile textures
// (texelFetch; layouts match light_culling.h). Sampler stages are baked
// to the flush's unit assignment (G-buffer 0-3, tile 4, SSAO 5, light
// data 6, cascade array 7, spot array 8, point cubes 9-12 — max
// register 12, inside DXBC's 16-sampler cap; #301) with full
// CSM/spot/point shadow sampling. IBL sampling still awaits the environment textures'
// arrival under this backend: ambient takes the constant-term branch.
// Scalar and integer GL uniforms become vec4 read through .x.

#include <bgfx_shader.sh>

// Clip-depth convention: the GLSL-family profiles (glsl/essl) run on GL
// APIs with NDC z in [-1, 1]; every other profile (spirv/metal) runs a
// zero-to-one API. The CPU builds matching projection matrices
// (DeviceCaps::depthZeroToOne), so depth<->NDC mapping keys off the
// shader language.
#if BGFX_SHADER_LANGUAGE_GLSL
#define ENGINE_DEPTH_TO_NDC(d) ((d) * 2.0 - 1.0)
#define ENGINE_CLIP_Z_TO_DEPTH(z) ((z) * 0.5 + 0.5)
#define ENGINE_NDC_XY_TO_UV(xy) ((xy) * 0.5 + 0.5)
#define ENGINE_UV_TO_NDC_XY(uv) ((uv) * 2.0 - 1.0)
#else
#define ENGINE_DEPTH_TO_NDC(d) (d)
#define ENGINE_CLIP_Z_TO_DEPTH(z) (z)
// y-down APIs store a direct render's ndc.y = +1 at texture row v = 0,
// so ndc<->uv conversions flip v (fullscreen.vs.sc explains the parity
// rule). NDC_XY_TO_UV addresses the shadow maps; UV_TO_NDC_XY undoes
// the fullscreen stage's already-flipped v_texcoord0 before position
// reconstruction — without it worldPos mirrors about the view center
// and the shadow term zeroes whole bands (ambient-only scene).
#define ENGINE_NDC_XY_TO_UV(xy) (vec2(0.5, -0.5) * (xy) + 0.5)
#define ENGINE_UV_TO_NDC_XY(uv) (vec2(2.0, -2.0) * (uv) + vec2(-1.0, 1.0))
#endif


#define TILE_MAX_POINT_LIGHTS 32
#define TILE_MAX_SPOT_LIGHTS 16
// Texels per tile in the 2-D tile table; mirrors kTileDataWidth
// (1 + point cap + 1 + spot cap) in light_culling.h.
#define TILE_DATA_WIDTH (1 + TILE_MAX_POINT_LIGHTS + 1 + TILE_MAX_SPOT_LIGHTS)
#define LIGHT_DATA_SPOT_ROW 128

SAMPLER2D(uGBufferAlbedo, 0);
SAMPLER2D(uGBufferNormal, 1);
SAMPLER2D(uGBufferEmissive, 2);
SAMPLER2D(uGBufferDepth, 3);
SAMPLER2D(uTileLightTex, 4);
SAMPLER2D(uSsaoTexture, 5);
// #301 unit map: the cascade and spot sets are Tex2DArrays (one
// register each), so the deferred map tops out at register 12 and fits
// DXBC's 16-sampler cap and WebGL2's 16-unit floor.
SAMPLER2D(uLightDataTex, 6);
SAMPLER2DARRAY(uShadowMapArray, 7);
SAMPLER2DARRAY(uSpotShadowMapArray, 8);
SAMPLERCUBE(uPointShadowMap0, 9);
SAMPLERCUBE(uPointShadowMap1, 10);
SAMPLERCUBE(uPointShadowMap2, 11);
SAMPLERCUBE(uPointShadowMap3, 12);

uniform vec4 uSsaoEnabled;        // .x
uniform mat4 uInvProjection;
uniform mat4 uInvView;
uniform vec4 uDirLightDirection;  // .xyz
uniform vec4 uDirLightColor;      // .xyz
uniform vec4 uCameraPos;          // .xyz
uniform vec4 uCameraForwardOrtho; // xyz forward, w 1 when orthographic
uniform vec4 uTileCountX;         // .x
uniform vec4 uScreenSize;         // .xy (tile row flip on y-down APIs)
uniform vec4 uPointLightCount;    // .x
uniform vec4 uSpotLightCount;     // .x

uniform vec4 uFogMode;            // .x
uniform vec4 uFogStart;           // .x
uniform vec4 uFogEnd;             // .x
uniform vec4 uFogDensity;         // .x
uniform vec4 uFogColor;           // .xyz
uniform vec4 uHeightFogEnabled;   // .x
uniform vec4 uHeightFogBaseHeight;
uniform vec4 uHeightFogDensity;
uniform vec4 uHeightFogFalloff;
uniform vec4 uHeightFogStepCount;

#define MAX_SPOT_SHADOW_LIGHTS 4
#define MAX_POINT_SHADOW_LIGHTS 4

// One tap from a shadow Tex2DArray at explicit LOD 0 (single-mip maps;
// fxc also rejects gradient samples in the dynamic light loops). HLSL
// samplers are typed structs, so the array form has its own entry
// point shared by the hlsl/spirv/metal typed-sampler branch; the
// plain-GLSL profiles (glsl/essl) route vec3 through textureLod.
#if BGFX_SHADER_LANGUAGE_GLSL
#define ENGINE_SHADOW_ARRAY_TAP(_s, _uv, _layer) \
    texture2DLod(_s, vec3(_uv, _layer), 0.0).r
#else
#define ENGINE_SHADOW_ARRAY_TAP(_s, _uv, _layer) \
    texture2DArrayLod(_s, vec3(_uv, _layer), 0.0).r
#endif
uniform vec4 uShadowEnabled;          // .x
uniform mat4 uShadowMatrix[4];
uniform vec4 uCascadeSplits;          // split distance per cascade
uniform vec4 uSpotShadowEnabled;      // .x
uniform mat4 uSpotShadowMatrix[MAX_SPOT_SHADOW_LIGHTS];
uniform vec4 uSpotShadowLightIdxVec;  // light index per slot
uniform vec4 uPointShadowEnabled;     // .x
uniform vec4 uPointShadowPosFar[MAX_POINT_SHADOW_LIGHTS]; // xyz pos, w far
uniform vec4 uPointShadowLightIdxVec; // light index per slot

float light_data(int x, int row) {
    return texelFetch(uLightDataTex, ivec2(x, row), 0).r;
}

vec3 light_data3(int x, int row) {
    return vec3(light_data(x, row), light_data(x + 1, row),
                light_data(x + 2, row));
}

float distribution_ggx(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = max(denom, 0.0001);
    return a2 / (3.14159265359 * denom * denom);
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

vec3 cook_torrance(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic,
                   float roughness, vec3 lightColor, float lightIntensity) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) {
        return vec3_splat(0.0);
    }
    vec3 F0 = mix(vec3_splat(0.04), albedo, metallic);
    float D = distribution_ggx(N, H, roughness);
    float G = geometry_smith(N, V, L, roughness);
    vec3 F = fresnel_schlick(max(dot(H, V), 0.0), F0);
    vec3 numerator = D * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL + 0.0001;
    vec3 specular = numerator / denominator;
    vec3 kD = (vec3_splat(1.0) - F) * (1.0 - metallic);
    return (kD * albedo / 3.14159265359 + specular) * lightColor *
           lightIntensity * NdotL;
}

vec3 reconstruct_world_pos(vec2 texCoord, float depth) {
    float z = ENGINE_DEPTH_TO_NDC(depth);
    vec4 clipPos = vec4(ENGINE_UV_TO_NDC_XY(texCoord), z, 1.0);
    vec4 viewPos = mul(uInvProjection, clipPos);
    viewPos /= viewPos.w;
    vec4 worldPos = mul(uInvView, viewPos);
    return worldPos.xyz;
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

float linearize_depth(float depth) {
    float z = ENGINE_DEPTH_TO_NDC(depth);
    vec4 clipPos = vec4(0.0, 0.0, z, 1.0);
    vec4 viewPos = mul(uInvProjection, clipPos);
    return -viewPos.z / viewPos.w;
}

// Shadow samples use explicit LOD 0: the maps are single-mip, so the
// texel fetched is identical, and the dx11 profile's fxc rejects
// gradient sampling inside the dynamic light loops that reach these
// helpers (X3511 unroll explosion otherwise).
float sample_shadow_pcf_at(vec2 uv, float compareDepth, int mapIdx) {
    float stored =
        ENGINE_SHADOW_ARRAY_TAP(uShadowMapArray, uv, float(mapIdx));
    return ((compareDepth - 0.002) > stored) ? 0.0 : 1.0;
}

float sample_spot_shadow_at(vec2 uv, float compareDepth, int mapIdx) {
    float stored =
        ENGINE_SHADOW_ARRAY_TAP(uSpotShadowMapArray, uv, float(mapIdx));
    return ((compareDepth - 0.002) > stored) ? 0.0 : 1.0;
}

// 3x3 PCF over one cascade/spot map; the texel steps mirror
// kShadowMapResolution (2048) and kSpotShadowMapResolution (1024) —
// constants here because HLSL-path shaderc lacks textureSize.
#define CASCADE_SHADOW_TEXEL (1.0 / 2048.0)
#define SPOT_SHADOW_TEXEL (1.0 / 1024.0)

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

float compute_shadow(vec3 worldPos, float depth) {
    if (uShadowEnabled.x == 0.0) {
        return 1.0;
    }
    float viewDepth = linearize_depth(depth);
    int cascadeIdx = 3;
    for (int i = 0; i < 4; ++i) {
        if (viewDepth < uCascadeSplits[i]) {
            cascadeIdx = i;
            break;
        }
    }
    vec4 shadowCoord = mul(uShadowMatrix[cascadeIdx], vec4(worldPos, 1.0));
    vec3 projCoords = shadowCoord.xyz / shadowCoord.w;
    projCoords.xy = ENGINE_NDC_XY_TO_UV(projCoords.xy);
    projCoords.z = ENGINE_CLIP_Z_TO_DEPTH(projCoords.z);
    float shadow = shadow_pcf(projCoords, cascadeIdx, false);

    float blendRange = uCascadeSplits[cascadeIdx] * 0.1;
    if (cascadeIdx < 3 &&
        viewDepth > uCascadeSplits[cascadeIdx] - blendRange) {
        vec4 nextShadowCoord =
            mul(uShadowMatrix[cascadeIdx + 1], vec4(worldPos, 1.0));
        vec3 nextProjCoords = nextShadowCoord.xyz / nextShadowCoord.w;
        nextProjCoords.xy = ENGINE_NDC_XY_TO_UV(nextProjCoords.xy);
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
        projCoords.xy = ENGINE_NDC_XY_TO_UV(projCoords.xy);
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

void main() {
    vec4 albedoMetallic = texture2D(uGBufferAlbedo, v_texcoord0);
    vec4 normalRoughness = texture2D(uGBufferNormal, v_texcoord0);
    vec4 emissiveAO = texture2D(uGBufferEmissive, v_texcoord0);
    float depth = texture2D(uGBufferDepth, v_texcoord0).r;

    if (depth >= 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 albedo = albedoMetallic.rgb;
    float metallic = albedoMetallic.a;
    vec3 N = normalize(normalRoughness.rgb * 2.0 - 1.0);
    float roughness = normalRoughness.a;
    vec3 emissive = emissiveAO.rgb;
    float ao = emissiveAO.a;

    vec3 worldPos = reconstruct_world_pos(v_texcoord0, depth);
    vec3 V = mix(normalize(uCameraPos.xyz - worldPos),
                 -uCameraForwardOrtho.xyz, uCameraForwardOrtho.w);

    vec3 Lo = vec3_splat(0.0);

    vec3 L_dir = normalize(-uDirLightDirection.xyz);
    float shadowFactor = compute_shadow(worldPos, depth);
    Lo += cook_torrance(N, V, L_dir, albedo, metallic, roughness,
                        uDirLightColor.xyz, 1.0) * shadowFactor;

    // Clamped to the tile grid: edge fragments past the last whole tile
    // must not read a neighboring row of the 2-D tile table (the clamp
    // also keeps uTileCountX genuinely read — the deferred resolver
    // requires it, and compilers strip unreferenced uniforms).
    int tileX = min(int(gl_FragCoord.x) / 16, int(uTileCountX.x) - 1);
    // The CPU packs tile rows in GL window order (row 0 at the bottom);
    // y-down APIs flip the fragment row before the lookup.
#if BGFX_SHADER_LANGUAGE_GLSL
    int tileY = int(gl_FragCoord.y) / 16;
#else
    int tileY = int(uScreenSize.y - gl_FragCoord.y) / 16;
#endif
    // 2-D tile table (#301 hardware runs): one texel row per tile ROW
    // with kTileDataWidth texels per tile along x — one texture row per
    // tile overflowed D3D's 16384 dimension cap at 4K.
    int tileBase = tileX * TILE_DATA_WIDTH;

    int pointLightCount = int(uPointLightCount.x);
    int tilePointCount =
        int(texelFetch(uTileLightTex, ivec2(tileBase, tileY), 0).r);
    for (int i = 0; i < TILE_MAX_POINT_LIGHTS; ++i) {
        if (i >= tilePointCount) {
            break;
        }
        int lightIdx =
            int(texelFetch(uTileLightTex, ivec2(tileBase + 1 + i, tileY), 0).r);
        if ((lightIdx < 0) || (lightIdx >= pointLightCount)) {
            continue;
        }
        vec3 Lpos = light_data3(0, lightIdx);
        vec3 toLight = Lpos - worldPos;
        float dist = length(toLight);
        float radius = light_data(7, lightIdx);
        if (dist > radius) {
            continue;
        }
        vec3 L = toLight / dist;
        float attenuation =
            clamp(1.0 - (dist * dist) / (radius * radius), 0.0, 1.0);
        attenuation *= attenuation;
        Lo += cook_torrance(N, V, L, albedo, metallic, roughness,
                            light_data3(3, lightIdx),
                            light_data(6, lightIdx) * attenuation) *
              compute_point_shadow(worldPos, lightIdx);
    }

    int spotLightCount = int(uSpotLightCount.x);
    int spotOffset = TILE_MAX_POINT_LIGHTS + 1;
    int tileSpotCount =
        int(texelFetch(uTileLightTex, ivec2(tileBase + spotOffset, tileY), 0).r);
    for (int i = 0; i < 16; ++i) {
        if (i >= tileSpotCount) {
            break;
        }
        int lightIdx = int(
            texelFetch(uTileLightTex, ivec2(tileBase + spotOffset + 1 + i, tileY), 0)
                .r);
        if ((lightIdx < 0) || (lightIdx >= spotLightCount)) {
            continue;
        }
        int row = LIGHT_DATA_SPOT_ROW + lightIdx;
        vec3 Lpos = light_data3(0, row);
        vec3 toLight = Lpos - worldPos;
        float dist = length(toLight);
        float radius = light_data(10, row);
        if (dist > radius) {
            continue;
        }
        vec3 L = toLight / dist;
        float attenuation =
            clamp(1.0 - (dist * dist) / (radius * radius), 0.0, 1.0);
        attenuation *= attenuation;
        vec3 spotDir = normalize(light_data3(3, row));
        float theta = dot(L, -spotDir);
        float innerCone = light_data(11, row);
        float outerCone = light_data(12, row);
        float epsilon = innerCone - outerCone;
        float spotFactor =
            clamp((theta - outerCone) / max(epsilon, 0.0001), 0.0, 1.0);
        Lo += cook_torrance(N, V, L, albedo, metallic, roughness,
                            light_data3(6, row),
                            light_data(9, row) * attenuation * spotFactor) *
              compute_spot_shadow(worldPos, lightIdx);
    }

    float ssaoFactor =
        (uSsaoEnabled.x != 0.0) ? texture2D(uSsaoTexture, v_texcoord0).r
                                : 1.0;
    vec3 ambient = vec3_splat(0.03) * albedo * ao * ssaoFactor;
    vec3 color = ambient + Lo + emissive;
    float distanceFog =
        compute_distance_fog_factor(length(uCameraPos.xyz - worldPos));
    float heightFog = compute_height_fog_factor(uCameraPos.xyz, worldPos);
    float fogFactor =
        clamp(1.0 - ((1.0 - distanceFog) * (1.0 - heightFog)), 0.0, 1.0);
    gl_FragColor = vec4(mix(color, uFogColor.xyz, fogFactor), 1.0);
}
