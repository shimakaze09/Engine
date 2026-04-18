#version 330 core

in vec2 vTexCoord;

out vec4 FragColor;

uniform sampler2D uGBufferAlbedo;
uniform sampler2D uGBufferNormal;
uniform sampler2D uGBufferEmissive;
uniform sampler2D uGBufferDepth;

// 0 = albedo, 1 = normals, 2 = metallic, 3 = roughness, 4 = emissive, 5 = AO, 6 = depth
uniform int uDebugMode;

void main() {
    vec4 albedoMetallic = texture(uGBufferAlbedo, vTexCoord);
    vec4 normalRoughness = texture(uGBufferNormal, vTexCoord);
    vec4 emissiveAO = texture(uGBufferEmissive, vTexCoord);
    float depth = texture(uGBufferDepth, vTexCoord).r;

    vec3 result = vec3(0.0);

    if (uDebugMode == 0) {
        result = albedoMetallic.rgb;
    } else if (uDebugMode == 1) {
        result = normalRoughness.rgb;
    } else if (uDebugMode == 2) {
        result = vec3(albedoMetallic.a);
    } else if (uDebugMode == 3) {
        result = vec3(normalRoughness.a);
    } else if (uDebugMode == 4) {
        result = emissiveAO.rgb;
    } else if (uDebugMode == 5) {
        result = vec3(emissiveAO.a);
    } else if (uDebugMode == 6) {
        // Linearize depth for visualization.
        float near = 0.1;
        float far = 1000.0;
        float ndc = depth * 2.0 - 1.0;
        float linearDepth = (2.0 * near * far) / (far + near - ndc * (far - near));
        result = vec3(linearDepth / far);
    }

    FragColor = vec4(result, 1.0);
}
