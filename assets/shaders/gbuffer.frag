// Defines the gbuffer fragment shader used by the Engine renderer.

#version 330 core

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vTexCoord;

// MRT outputs.
layout(location = 0) out vec4 gAlbedoMetallic;   // RT0: albedo.rgb + metallic
layout(location = 1) out vec4 gNormalRoughness;   // RT1: normal.xyz + roughness
layout(location = 2) out vec4 gEmissiveAO;        // RT2: emissive.rgb + AO

uniform vec3 uAlbedo;
uniform float uMetallic;
uniform float uRoughness;
uniform float uAO;
uniform vec3 uEmissive;
uniform int uHasAlbedoTexture;
uniform sampler2D uAlbedoTexture;

// issue #160: texture-backed PBR material slots. metallicRoughness follows
// the glTF packing (G = roughness, B = metallic) so opaque draws stay
// within the free sampler-unit budget shared with shadows/IBL. Opacity here
// only ever drives an alpha-test discard (uAlphaMode == 1, "mask"): opacity
// < 1 already routes a draw to the forward transparent pass upstream
// (command_buffer_flush.cpp's opaque/transparent partition), so this
// (opaque-only) shader never blends.
uniform sampler2D uMetallicRoughnessTexture;
uniform int uHasMetallicRoughnessTexture;
uniform sampler2D uEmissiveTexture;
uniform int uHasEmissiveTexture;
uniform sampler2D uOcclusionTexture;
uniform int uHasOcclusionTexture;
uniform sampler2D uOpacityTexture;
uniform int uHasOpacityTexture;
uniform int uAlphaMode; // 0 = opaque, 1 = mask, 2 = blend (never reaches here)
uniform float uAlphaCutoff;
uniform vec2 uUvTiling;
uniform vec2 uUvOffset;

/// Runs the shader entry point for this stage.
void main() {
    vec2 uv = vTexCoord * uUvTiling + uUvOffset;
    vec3 N = normalize(vNormal);

    vec3 albedo = uAlbedo;
    if (uHasAlbedoTexture != 0) {
        albedo *= texture(uAlbedoTexture, uv).rgb;
    }

    float metallic = uMetallic;
    float roughness = uRoughness;
    if (uHasMetallicRoughnessTexture != 0) {
        vec3 mr = texture(uMetallicRoughnessTexture, uv).rgb;
        roughness *= mr.g;
        metallic *= mr.b;
    }

    float ao = uAO;
    if (uHasOcclusionTexture != 0) {
        ao *= texture(uOcclusionTexture, uv).r;
    }

    vec3 emissive = uEmissive;
    if (uHasEmissiveTexture != 0) {
        emissive *= texture(uEmissiveTexture, uv).rgb;
    }

    float opacity = 1.0;
    if (uHasOpacityTexture != 0) {
        opacity = texture(uOpacityTexture, uv).r;
    }
    if (uAlphaMode == 1 && opacity < uAlphaCutoff) {
        discard;
    }

    gAlbedoMetallic = vec4(albedo, metallic);
    gNormalRoughness = vec4(N * 0.5 + 0.5, roughness);
    gEmissiveAO = vec4(emissive, ao);
}
