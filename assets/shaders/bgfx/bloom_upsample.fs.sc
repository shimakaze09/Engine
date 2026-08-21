$input v_texcoord0

// Dual-Kawase 9-tap tent upsample (bgfx port of bloom_upsample.frag,
// #138).

#include <bgfx_shader.sh>

SAMPLER2D(u_input, 0);

uniform vec4 u_texelSize; // .xy

void main() {
    vec2 ts = u_texelSize.xy;
    vec3 sum = vec3_splat(0.0);
    sum += texture2D(u_input, v_texcoord0 + vec2(-1.0,  0.0) * ts).rgb;
    sum += texture2D(u_input, v_texcoord0 + vec2( 1.0,  0.0) * ts).rgb;
    sum += texture2D(u_input, v_texcoord0 + vec2( 0.0,  1.0) * ts).rgb;
    sum += texture2D(u_input, v_texcoord0 + vec2( 0.0, -1.0) * ts).rgb;
    sum += texture2D(u_input, v_texcoord0 + vec2(-0.5, -0.5) * ts * 2.0).rgb * 2.0;
    sum += texture2D(u_input, v_texcoord0 + vec2( 0.5, -0.5) * ts * 2.0).rgb * 2.0;
    sum += texture2D(u_input, v_texcoord0 + vec2(-0.5,  0.5) * ts * 2.0).rgb * 2.0;
    sum += texture2D(u_input, v_texcoord0 + vec2( 0.5,  0.5) * ts * 2.0).rgb * 2.0;
    gl_FragColor = vec4(sum / 12.0, 1.0);
}
