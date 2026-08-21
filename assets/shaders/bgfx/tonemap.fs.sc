$input v_texcoord0

// Tonemap + bloom-composite fragment stage (bgfx port of tonemap.frag,
// #138 Phase C). Uniform names match the GL source; scalar and integer
// GL uniforms become vec4 (bgfx's uniform model) read through .x, and
// the baked sampler stages (scene 0, bloom 1) must match the stages the
// flush assigns through set_param_i32.

#include <bgfx_shader.sh>

SAMPLER2D(u_sceneColor, 0);
SAMPLER2D(u_bloomTexture, 1);

uniform vec4 u_exposure;
uniform vec4 u_tonemapOperator; // .x: 0=Reinhard, 1=ACES, 2=Uncharted2
uniform vec4 u_bloomIntensity;
uniform vec4 u_bloomEnabled;

vec3 tonemap_reinhard(vec3 hdr) {
    return hdr / (hdr + vec3_splat(1.0));
}

// ACES filmic approximation (Krzysztof Narkowicz).
vec3 tonemap_aces(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Uncharted 2 filmic (John Hable).
vec3 uncharted2_curve(vec3 x) {
    float A = 0.15;
    float B = 0.50;
    float C = 0.10;
    float D = 0.20;
    float E = 0.02;
    float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) -
           E / F;
}

vec3 tonemap_uncharted2(vec3 hdr) {
    float W = 11.2;
    vec3 curr = uncharted2_curve(hdr);
    vec3 whiteScale = vec3_splat(1.0) / uncharted2_curve(vec3_splat(W));
    return curr * whiteScale;
}

void main() {
    vec3 hdr = texture2D(u_sceneColor, v_texcoord0).rgb;
    if (u_bloomEnabled.x != 0.0) {
        hdr += texture2D(u_bloomTexture, v_texcoord0).rgb *
               u_bloomIntensity.x;
    }
    hdr *= u_exposure.x;

    vec3 mapped;
    if (u_tonemapOperator.x >= 1.5) {
        mapped = tonemap_uncharted2(hdr);
    } else if (u_tonemapOperator.x >= 0.5) {
        mapped = tonemap_aces(hdr);
    } else {
        mapped = tonemap_reinhard(hdr);
    }
    gl_FragColor = vec4(pow(mapped, vec3_splat(1.0 / 2.2)), 1.0);
}
