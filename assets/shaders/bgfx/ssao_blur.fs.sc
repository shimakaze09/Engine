$input v_texcoord0

// SSAO 5x5 box blur (bgfx port of ssao_blur.frag, #138).

#include <bgfx_shader.sh>

SAMPLER2D(u_ssaoInput, 0);

uniform vec4 u_texelSize; // .xy

void main() {
    float result = 0.0;
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            vec2 offset = vec2(float(x), float(y)) * u_texelSize.xy;
            result += texture2D(u_ssaoInput, v_texcoord0 + offset).r;
        }
    }
    gl_FragColor = vec4_splat(result / 25.0);
}
