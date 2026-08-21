$input v_worldpos, v_normal, v_texcoord0

// PBR forward fragment stage (bgfx port of pbr.frag, #138): Cook-
// Torrance direct lighting over the flat light-array vocabulary, the
// five material texture slots (baked stages 0-4 matching the flush's
// unit assignment), alpha modes, and distance/height fog. Shadow and
// IBL sampling are omitted in this port: their GL texture units (6-17,
// 19-21) exceed bgfx's 16-sampler budget, and both features already
// soft-disable under this backend until their units land with a
// remapped budget — the shader takes the shadow=1 / constant-ambient
// paths. Scalar and integer GL uniforms become vec4 read through .x.

#include <bgfx_shader.sh>

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
        Lo += cook_torrance(N, V, L, radiance, albedo, metallic, roughness,
                            F0);
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
        Lo += cook_torrance(N, V, L, radiance, albedo, metallic, roughness,
                            F0);
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
        Lo += cook_torrance(N, V, L, radiance, albedo, metallic, roughness,
                            F0);
    }

    vec3 ambient = vec3_splat(0.03) * albedo * ao;
    vec3 color = ambient + Lo + emissive;
    float distanceFog =
        compute_distance_fog_factor(length(u_cameraPos.xyz - v_worldpos));
    float heightFog = compute_height_fog_factor(u_cameraPos.xyz, v_worldpos);
    float fogFactor =
        clamp(1.0 - ((1.0 - distanceFog) * (1.0 - heightFog)), 0.0, 1.0);
    gl_FragColor = vec4(mix(color, uFogColor.xyz, fogFactor), opacity);
}
