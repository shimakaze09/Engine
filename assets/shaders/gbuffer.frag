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

/// Runs the shader entry point for this stage.
void main() {
    vec3 N = normalize(vNormal);

    vec3 albedo = uAlbedo;
    if (uHasAlbedoTexture != 0) {
        albedo *= texture(uAlbedoTexture, vTexCoord).rgb;
    }

    gAlbedoMetallic = vec4(albedo, uMetallic);
    gNormalRoughness = vec4(N * 0.5 + 0.5, uRoughness);
    gEmissiveAO = vec4(uEmissive, uAO);
}
