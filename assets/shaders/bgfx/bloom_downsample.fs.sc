$input v_texcoord0

// Dual-Kawase 5-tap downsample (bgfx port of bloom_downsample.frag,
// #138).

#include <bgfx_shader.sh>

SAMPLER2D(u_input, 0);

uniform vec4 u_texelSize; // .xy

void main() {
    vec2 ts = u_texelSize.xy;
    vec3 sum = texture2D(u_input, v_texcoord0).rgb * 4.0;
    sum += texture2D(u_input, v_texcoord0 + vec2(-1.0, -1.0) * ts).rgb;
    sum += texture2D(u_input, v_texcoord0 + vec2( 1.0, -1.0) * ts).rgb;
    sum += texture2D(u_input, v_texcoord0 + vec2(-1.0,  1.0) * ts).rgb;
    sum += texture2D(u_input, v_texcoord0 + vec2( 1.0,  1.0) * ts).rgb;
    gl_FragColor = vec4(sum / 8.0, 1.0);
}
