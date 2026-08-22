$input v_texcoord0

// SSAO hemisphere-kernel stage (bgfx port of ssao.frag, #138): the
// kernel arrives as the flat vec4 array shared with GLSL (xyz used);
// sampler stages baked to the flush's unit assignment (depth 0,
// normal 1, noise 2). Scalar GL uniforms become vec4 read through .x.
// The kernel loop's depth sample uses explicit LOD 0 (single-mip
// input, identical texel) so the dx11 profile's fxc keeps the loop
// rolled instead of force-unrolling around a gradient instruction.

#include <bgfx_shader.sh>

// Clip-depth convention: the GLSL-family profiles (glsl/essl) run on GL
// APIs with NDC z in [-1, 1]; every other profile (spirv/metal) runs a
// zero-to-one API. The CPU builds matching projection matrices
// (DeviceCaps::depthZeroToOne), so depth<->NDC mapping keys off the
// shader language.
#if BGFX_SHADER_LANGUAGE_GLSL
#define ENGINE_DEPTH_TO_NDC(d) ((d) * 2.0 - 1.0)
#define ENGINE_CLIP_Z_TO_DEPTH(z) ((z) * 0.5 + 0.5)
#else
#define ENGINE_DEPTH_TO_NDC(d) (d)
#define ENGINE_CLIP_Z_TO_DEPTH(z) (z)
#endif


SAMPLER2D(u_gBufferDepth, 0);
SAMPLER2D(u_gBufferNormal, 1);
SAMPLER2D(u_noiseTexture, 2);

uniform vec4 u_samples[32];  // xyz: hemisphere kernel
uniform mat4 u_projection;
uniform mat4 u_invProjection;
uniform mat4 u_viewMat;
uniform vec4 u_noiseScale;   // .xy
uniform vec4 u_radius;       // .x
uniform vec4 u_bias;         // .x

vec3 reconstruct_view_pos(vec2 uv, float depth) {
    float z = ENGINE_DEPTH_TO_NDC(depth);
    vec4 clip = vec4(uv * 2.0 - 1.0, z, 1.0);
    vec4 view = mul(u_invProjection, clip);
    return view.xyz / view.w;
}

void main() {
    float depth = texture2D(u_gBufferDepth, v_texcoord0).r;
    if (depth >= 1.0) {
        gl_FragColor = vec4_splat(1.0);
        return;
    }

    vec3 fragPos = reconstruct_view_pos(v_texcoord0, depth);
    vec3 worldNormal =
        texture2D(u_gBufferNormal, v_texcoord0).rgb * 2.0 - 1.0;
    mat3 view3 = mat3(u_viewMat[0].xyz, u_viewMat[1].xyz, u_viewMat[2].xyz);
    vec3 normal = normalize(mul(view3, worldNormal));

    vec3 randomVec =
        texture2D(u_noiseTexture, v_texcoord0 * u_noiseScale.xy).rgb;
    vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN = mat3(tangent, bitangent, normal);

    float occlusion = 0.0;
    for (int i = 0; i < 32; ++i) {
        vec3 samplePos =
            fragPos + mul(TBN, u_samples[i].xyz) * u_radius.x;
        vec4 offset = mul(u_projection, vec4(samplePos, 1.0));
        offset.xyz /= offset.w;
        offset.xyz = offset.xyz * 0.5 + 0.5;
        float sampleDepth = texture2DLod(u_gBufferDepth, offset.xy, 0.0).r;
        vec3 sampleViewPos = reconstruct_view_pos(offset.xy, sampleDepth);
        float rangeCheck = smoothstep(
            0.0, 1.0, u_radius.x / abs(fragPos.z - sampleViewPos.z));
        occlusion +=
            ((sampleViewPos.z >= samplePos.z + u_bias.x) ? 1.0 : 0.0) *
            rangeCheck;
    }
    gl_FragColor = vec4_splat(1.0 - (occlusion / 32.0));
}
