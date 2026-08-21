$input v_texcoord0

// Log-luminance reduction source (bgfx port of luminance.frag, #138).

#include <bgfx_shader.sh>

SAMPLER2D(u_sceneColor, 0);

void main() {
    vec3 hdr = texture2D(u_sceneColor, v_texcoord0).rgb;
    float lum = dot(hdr, vec3(0.2126, 0.7152, 0.0722));
    float logLum = log(max(lum, 0.0001));
    gl_FragColor = vec4(logLum, lum, 0.0, 1.0);
}
