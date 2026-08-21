$input v_texcoord0

// G-buffer debug view (bgfx port of gbuffer_debug.frag, #138): channel
// visualization of the deferred targets, selected by r_gbuffer_debug.

#include <bgfx_shader.sh>

SAMPLER2D(uGBufferAlbedo, 0);
SAMPLER2D(uGBufferNormal, 1);
SAMPLER2D(uGBufferEmissive, 2);
SAMPLER2D(uGBufferDepth, 3);

uniform vec4 uDebugMode; // .x: 0 albedo, 1 normals, 2 metallic,
                         // 3 roughness, 4 emissive, 5 AO, 6 depth

void main() {
    vec4 albedoMetallic = texture2D(uGBufferAlbedo, v_texcoord0);
    vec4 normalRoughness = texture2D(uGBufferNormal, v_texcoord0);
    vec4 emissiveAO = texture2D(uGBufferEmissive, v_texcoord0);
    float depth = texture2D(uGBufferDepth, v_texcoord0).r;

    int mode = int(uDebugMode.x);
    vec3 result = vec3_splat(0.0);
    if (mode == 0) {
        result = albedoMetallic.rgb;
    } else if (mode == 1) {
        result = normalRoughness.rgb;
    } else if (mode == 2) {
        result = vec3_splat(albedoMetallic.a);
    } else if (mode == 3) {
        result = vec3_splat(normalRoughness.a);
    } else if (mode == 4) {
        result = emissiveAO.rgb;
    } else if (mode == 5) {
        result = vec3_splat(emissiveAO.a);
    } else if (mode == 6) {
        float near = 0.1;
        float far = 1000.0;
        float ndc = depth * 2.0 - 1.0;
        float linearDepth =
            (2.0 * near * far) / (far + near - ndc * (far - near));
        result = vec3_splat(linearDepth / far);
    }
    gl_FragColor = vec4(result, 1.0);
}
