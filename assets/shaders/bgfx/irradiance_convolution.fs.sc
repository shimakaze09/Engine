$input v_dir

// Cosine-hemisphere irradiance convolution (bgfx port of
// irradiance_convolution.frag, #138).

#include <bgfx_shader.sh>

SAMPLERCUBE(u_environmentMap, 0);

float radical_inverse_vdc(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 hammersley(uint index, uint count) {
    return vec2(float(index) / float(count), radical_inverse_vdc(index));
}

vec3 cosine_sample_hemisphere(vec2 xi, vec3 normal) {
    float phi = 2.0 * 3.14159265359 * xi.x;
    float cosTheta = sqrt(1.0 - xi.y);
    float sinTheta = sqrt(xi.y);
    vec3 localDir =
        vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0)
                                    : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, normal));
    vec3 bitangent = cross(normal, tangent);
    return normalize(tangent * localDir.x + bitangent * localDir.y +
                     normal * localDir.z);
}

void main() {
    vec3 normal = normalize(v_dir);
    vec3 irradiance = vec3_splat(0.0);
    for (uint i = 0u; i < 128u; ++i) {
        vec3 sampleDir =
            cosine_sample_hemisphere(hammersley(i, 128u), normal);
        irradiance += textureCube(u_environmentMap, sampleDir).rgb;
    }
    irradiance = (3.14159265359 / 128.0) * irradiance;
    gl_FragColor = vec4(irradiance, 1.0);
}
