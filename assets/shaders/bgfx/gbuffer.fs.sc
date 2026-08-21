$input v_worldpos, v_normal, v_texcoord0

// G-buffer fragment stage (bgfx port of gbuffer.frag, #138): writes the
// three MRT targets (albedo+metallic, packed normal+roughness,
// emissive+AO) from the five material texture slots (baked stages 0-4
// matching the flush's unit assignment) with the opaque-path alpha-test
// contract. Scalar and integer GL uniforms become vec4 read through .x.

#include <bgfx_shader.sh>

SAMPLER2D(uAlbedoTexture, 0);
SAMPLER2D(uMetallicRoughnessTexture, 1);
SAMPLER2D(uEmissiveTexture, 2);
SAMPLER2D(uOcclusionTexture, 3);
SAMPLER2D(uOpacityTexture, 4);

uniform vec4 uAlbedo;      // .xyz
uniform vec4 uMetallic;    // .x
uniform vec4 uRoughness;   // .x
uniform vec4 uAO;          // .x
uniform vec4 uEmissive;    // .xyz
uniform vec4 uHasAlbedoTexture;
uniform vec4 uHasMetallicRoughnessTexture;
uniform vec4 uHasEmissiveTexture;
uniform vec4 uHasOcclusionTexture;
uniform vec4 uHasOpacityTexture;
uniform vec4 uAlphaMode;   // .x: 0 opaque, 1 mask (blend never reaches here)
uniform vec4 uAlphaCutoff; // .x
uniform vec4 uUvTiling;    // .xy
uniform vec4 uUvOffset;    // .xy

void main() {
    vec2 uv = v_texcoord0 * uUvTiling.xy + uUvOffset.xy;
    vec3 N = normalize(v_normal);

    vec3 albedo = uAlbedo.xyz;
    if (uHasAlbedoTexture.x != 0.0) {
        albedo *= texture2D(uAlbedoTexture, uv).rgb;
    }

    float metallic = uMetallic.x;
    float roughness = uRoughness.x;
    if (uHasMetallicRoughnessTexture.x != 0.0) {
        vec3 mr = texture2D(uMetallicRoughnessTexture, uv).rgb;
        roughness *= mr.g;
        metallic *= mr.b;
    }

    float ao = uAO.x;
    if (uHasOcclusionTexture.x != 0.0) {
        ao *= texture2D(uOcclusionTexture, uv).r;
    }

    vec3 emissive = uEmissive.xyz;
    if (uHasEmissiveTexture.x != 0.0) {
        emissive *= texture2D(uEmissiveTexture, uv).rgb;
    }

    float opacity = 1.0;
    if (uHasOpacityTexture.x != 0.0) {
        opacity = texture2D(uOpacityTexture, uv).r;
    }
    if ((int(uAlphaMode.x) == 1) && (opacity < uAlphaCutoff.x)) {
        discard;
    }

    gl_FragData[0] = vec4(albedo, metallic);
    gl_FragData[1] = vec4(N * 0.5 + 0.5, roughness);
    gl_FragData[2] = vec4(emissive, ao);
}
