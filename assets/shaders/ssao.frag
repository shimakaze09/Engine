#version 330 core

in vec2 vTexCoord;

uniform sampler2D u_gBufferDepth;
uniform sampler2D u_gBufferNormal;
uniform sampler2D u_noiseTexture;
uniform vec3 u_samples[32];
uniform mat4 u_projection;
uniform vec2 u_noiseScale;
uniform float u_radius;
uniform float u_bias;

out float outAO;

vec3 reconstructViewPos(vec2 uv, float depth) {
    float z = depth * 2.0 - 1.0;
    vec4 clip = vec4(uv * 2.0 - 1.0, z, 1.0);
    vec4 view = inverse(u_projection) * clip;
    return view.xyz / view.w;
}

void main() {
    float depth = texture(u_gBufferDepth, vTexCoord).r;
    if (depth >= 1.0) { outAO = 1.0; return; }

    vec3 fragPos = reconstructViewPos(vTexCoord, depth);
    vec3 normal = normalize(texture(u_gBufferNormal, vTexCoord).rgb * 2.0 - 1.0);

    vec3 randomVec = texture(u_noiseTexture, vTexCoord * u_noiseScale).rgb;
    vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN = mat3(tangent, bitangent, normal);

    float occlusion = 0.0;
    for (int i = 0; i < 32; ++i) {
        vec3 samplePos = fragPos + TBN * u_samples[i] * u_radius;

        vec4 offset = u_projection * vec4(samplePos, 1.0);
        offset.xyz /= offset.w;
        offset.xyz = offset.xyz * 0.5 + 0.5;

        float sampleDepth = texture(u_gBufferDepth, offset.xy).r;
        vec3 sampleViewPos = reconstructViewPos(offset.xy, sampleDepth);

        float rangeCheck = smoothstep(0.0, 1.0, u_radius / abs(fragPos.z - sampleViewPos.z));
        occlusion += (sampleViewPos.z >= samplePos.z + u_bias ? 1.0 : 0.0) * rangeCheck;
    }

    outAO = 1.0 - (occlusion / 32.0);
}
