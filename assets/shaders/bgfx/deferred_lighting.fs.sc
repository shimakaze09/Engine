$input v_texcoord0

// Deferred lighting fragment stage (bgfx port of deferred_lighting.frag,
// #138): Cook-Torrance shading of the G-buffer with tile-culled point/
// spot lights fetched from the R32F light-data and tile textures
// (texelFetch; layouts match light_culling.h). Sampler stages are baked
// to the flush's unit assignment (G-buffer 0-3, tile 4, SSAO 5, light
// data 18 — enabled by the build's 32-sampler bgfx config). Shadow and
// IBL sampling are omitted until their units cook the producing passes
// (both soft-disable under this backend today): shadows read as fully
// lit and ambient takes the constant-term branch. Scalar and integer GL
// uniforms become vec4 read through .x.

#include <bgfx_shader.sh>

#define TILE_MAX_POINT_LIGHTS 32
#define LIGHT_DATA_SPOT_ROW 128

SAMPLER2D(uGBufferAlbedo, 0);
SAMPLER2D(uGBufferNormal, 1);
SAMPLER2D(uGBufferEmissive, 2);
SAMPLER2D(uGBufferDepth, 3);
SAMPLER2D(uTileLightTex, 4);
SAMPLER2D(uSsaoTexture, 5);
SAMPLER2D(uLightDataTex, 18);

uniform vec4 uSsaoEnabled;        // .x
uniform mat4 uInvProjection;
uniform mat4 uInvView;
uniform vec4 uDirLightDirection;  // .xyz
uniform vec4 uDirLightColor;      // .xyz
uniform vec4 uCameraPos;          // .xyz
uniform vec4 uCameraForwardOrtho; // xyz forward, w 1 when orthographic
uniform vec4 uTileCountX;         // .x
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
    float z = depth * 2.0 - 1.0;
    vec4 clipPos = vec4(texCoord * 2.0 - 1.0, z, 1.0);
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
    Lo += cook_torrance(N, V, L_dir, albedo, metallic, roughness,
                        uDirLightColor.xyz, 1.0);

    int tileX = int(gl_FragCoord.x) / 16;
    int tileY = int(gl_FragCoord.y) / 16;
    int tileIdx = tileY * int(uTileCountX.x) + tileX;

    int pointLightCount = int(uPointLightCount.x);
    int tilePointCount =
        int(texelFetch(uTileLightTex, ivec2(0, tileIdx), 0).r);
    for (int i = 0; i < TILE_MAX_POINT_LIGHTS; ++i) {
        if (i >= tilePointCount) {
            break;
        }
        int lightIdx =
            int(texelFetch(uTileLightTex, ivec2(1 + i, tileIdx), 0).r);
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
                            light_data(6, lightIdx) * attenuation);
    }

    int spotLightCount = int(uSpotLightCount.x);
    int spotOffset = TILE_MAX_POINT_LIGHTS + 1;
    int tileSpotCount =
        int(texelFetch(uTileLightTex, ivec2(spotOffset, tileIdx), 0).r);
    for (int i = 0; i < 16; ++i) {
        if (i >= tileSpotCount) {
            break;
        }
        int lightIdx = int(
            texelFetch(uTileLightTex, ivec2(spotOffset + 1 + i, tileIdx), 0)
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
                            light_data(9, row) * attenuation * spotFactor);
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
