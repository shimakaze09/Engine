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

// Tile light data (R32F texture: x = MAX_LIGHTS_PER_TILE+1, y = numTiles).
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

// ---- Point lights (uniform arrays, max 128) ----
#define MAX_POINT_LIGHTS 128
uniform int uPointLightCount;
uniform vec3 uPointLightPositions[MAX_POINT_LIGHTS];
uniform vec3 uPointLightColors[MAX_POINT_LIGHTS];
uniform float uPointLightIntensities[MAX_POINT_LIGHTS];
uniform float uPointLightRadii[MAX_POINT_LIGHTS];

// ---- Spot lights (uniform arrays, max 64) ----
#define MAX_SPOT_LIGHTS 64
uniform int uSpotLightCount;
uniform vec3 uSpotLightPositions[MAX_SPOT_LIGHTS];
uniform vec3 uSpotLightDirections[MAX_SPOT_LIGHTS];
uniform vec3 uSpotLightColors[MAX_SPOT_LIGHTS];
uniform float uSpotLightIntensities[MAX_SPOT_LIGHTS];
uniform float uSpotLightRadii[MAX_SPOT_LIGHTS];
uniform float uSpotLightInnerCones[MAX_SPOT_LIGHTS];
uniform float uSpotLightOuterCones[MAX_SPOT_LIGHTS];

// ---- PBR BRDF ----

const float PI = 3.14159265359;

float distribution_ggx(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
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

    // Directional light.
    vec3 L_dir = normalize(-uDirLightDirection);
    Lo += cook_torrance(N, V, L_dir, albedo, metallic, roughness,
                        uDirLightColor, 1.0);

    // Determine tile index for culled light lookup.
    int tileX = int(gl_FragCoord.x) / 16;
    int tileY = int(gl_FragCoord.y) / 16;
    int tileIdx = tileY * uTileCountX + tileX;

    // Read tile point light count from tile texture.
    int tilePointCount = int(texelFetch(uTileLightTex, ivec2(0, tileIdx), 0).r);
    for (int i = 0; i < tilePointCount; ++i) {
        int lightIdx = int(texelFetch(uTileLightTex, ivec2(1 + i, tileIdx), 0).r);
        if (lightIdx < 0 || lightIdx >= uPointLightCount) continue;

        vec3 Lpos = uPointLightPositions[lightIdx];
        vec3 toLight = Lpos - worldPos;
        float dist = length(toLight);
        float radius = uPointLightRadii[lightIdx];
        if (dist > radius) continue;

        vec3 L = toLight / dist;
        float attenuation = clamp(1.0 - (dist * dist) / (radius * radius), 0.0, 1.0);
        attenuation *= attenuation;

        Lo += cook_torrance(N, V, L, albedo, metallic, roughness,
                            uPointLightColors[lightIdx],
                            uPointLightIntensities[lightIdx] * attenuation);
    }

    // Read tile spot light count.
    int spotOffset = MAX_POINT_LIGHTS + 1;
    int tileSpotCount = int(texelFetch(uTileLightTex, ivec2(spotOffset, tileIdx), 0).r);
    for (int i = 0; i < tileSpotCount; ++i) {
        int lightIdx = int(texelFetch(uTileLightTex, ivec2(spotOffset + 1 + i, tileIdx), 0).r);
        if (lightIdx < 0 || lightIdx >= uSpotLightCount) continue;

        vec3 Lpos = uSpotLightPositions[lightIdx];
        vec3 toLight = Lpos - worldPos;
        float dist = length(toLight);
        float radius = uSpotLightRadii[lightIdx];
        if (dist > radius) continue;

        vec3 L = toLight / dist;
        float attenuation = clamp(1.0 - (dist * dist) / (radius * radius), 0.0, 1.0);
        attenuation *= attenuation;

        // Spot cone falloff.
        vec3 spotDir = normalize(uSpotLightDirections[lightIdx]);
        float theta = dot(L, -spotDir);
        float innerCone = uSpotLightInnerCones[lightIdx];
        float outerCone = uSpotLightOuterCones[lightIdx];
        float epsilon = innerCone - outerCone;
        float spotFactor = clamp((theta - outerCone) / max(epsilon, 0.0001), 0.0, 1.0);

        Lo += cook_torrance(N, V, L, albedo, metallic, roughness,
                            uSpotLightColors[lightIdx],
                            uSpotLightIntensities[lightIdx] * attenuation * spotFactor);
    }

    // Ambient + emissive.
    float ssaoFactor = (uSsaoEnabled != 0) ? texture(uSsaoTexture, vTexCoord).r : 1.0;
    vec3 ambient = vec3(0.03) * albedo * ao * ssaoFactor;
    vec3 color = ambient + Lo + emissive;

    FragColor = vec4(color, 1.0);
}
