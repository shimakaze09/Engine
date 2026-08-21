$input v_texcoord0

// Bloom threshold stage (bgfx port of bloom_threshold.frag, #138).
// Scalar GL uniforms become vec4 read through .x.

#include <bgfx_shader.sh>

SAMPLER2D(u_sceneColor, 0);

uniform vec4 u_threshold; // .x

void main() {
    vec3 c = texture2D(u_sceneColor, v_texcoord0).rgb;
    float brightness = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float contribution = max(brightness - u_threshold.x, 0.0);
    gl_FragColor = vec4(c * contribution / max(brightness, 0.001), 1.0);
}
