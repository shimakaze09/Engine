$input v_texcoord0

// Split-sum BRDF integration LUT:
// 512-sample GGX importance sum writing scale/bias to RG.

#include <bgfx_shader.sh>

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

// GGX half vector about the +z normal this integration fixes, with the
// sample set's azimuth measured from +x, the side the view direction is
// on. The 512-sample sum is sensitive to that alignment at grazing angles:
// with the azimuth origin a quarter turn from the view, scale at
// NdotV = 0.02 lands 0.04 from the converged integral; aligned, 0.008.
vec3 importance_sample_ggx(vec2 xi, float roughness) {
    float a = roughness * roughness;
    float phi = 2.0 * 3.14159265359 * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float geometry_schlick_ggx(float nDotV, float roughness) {
    float a = roughness * roughness;
    float k = a * 0.5;
    return nDotV / (nDotV * (1.0 - k) + k);
}

float geometry_smith(float nDotV, float nDotL, float roughness) {
    return geometry_schlick_ggx(nDotV, roughness) *
           geometry_schlick_ggx(nDotL, roughness);
}

vec2 integrate_brdf(float nDotV, float roughness) {
    vec3 viewDir = vec3(sqrt(max(1.0 - nDotV * nDotV, 0.0)), 0.0, nDotV);
    float scale = 0.0;
    float bias = 0.0;
    for (uint i = 0u; i < 512u; ++i) {
        vec3 halfVector =
            importance_sample_ggx(hammersley(i, 512u), roughness);
        vec3 lightDir = normalize(
            2.0 * dot(viewDir, halfVector) * halfVector - viewDir);
        float nDotL = max(lightDir.z, 0.0);
        float nDotH = max(halfVector.z, 0.0);
        float vDotH = max(dot(viewDir, halfVector), 0.0);
        if (nDotL > 0.0) {
            float geometry = geometry_smith(nDotV, nDotL, roughness);
            float geometryVisibility =
                (geometry * vDotH) / max(nDotH * nDotV, 0.0001);
            float fresnel = pow(1.0 - vDotH, 5.0);
            scale += (1.0 - fresnel) * geometryVisibility;
            bias += fresnel * geometryVisibility;
        }
    }
    return vec2(scale, bias) / 512.0;
}

void main() {
    float nDotV = clamp(v_texcoord0.x, 0.001, 1.0);
    float roughness = clamp(v_texcoord0.y, 0.001, 1.0);
    vec2 brdf = integrate_brdf(nDotV, roughness);
    gl_FragColor = vec4(brdf, 0.0, 1.0);
}
