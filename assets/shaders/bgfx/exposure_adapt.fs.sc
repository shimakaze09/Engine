$input v_texcoord0

// Auto-exposure adaptation, drawn into a 1x1 target. Averages the last
// log-luminance mip over a fixed 8x8 grid of bilinear taps (the mip chain
// ends at a few texels, not one), turns that geometric-mean luminance into
// the exposure that maps it to middle grey, and moves last frame's
// exposure toward it. Sampler stages: log luminance 0, previous exposure 1.
// Taps use explicit LOD 0 so the dx11 profile keeps the loop rolled.

#include <bgfx_shader.sh>

SAMPLER2D(u_luminance, 0);
SAMPLER2D(u_previousExposure, 1);

// x: fraction of the way to the target this frame, y: minimum exposure,
// z: maximum exposure, w: 1 when u_previousExposure holds a value.
uniform vec4 u_adapt;

void main() {
    float sumLog = 0.0;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            vec2 uv = (vec2(float(x), float(y)) + 0.5) / 8.0;
            sumLog += texture2DLod(u_luminance, uv, 0.0).r;
        }
    }
    float averageLuminance = exp(sumLog / 64.0);
    // 0.18: the middle-grey key, so a scene averaging 0.18 keeps
    // exposure 1.
    float target =
        clamp(0.18 / max(averageLuminance, 0.0001), u_adapt.y, u_adapt.z);
    float previous = texture2DLod(u_previousExposure, vec2(0.5, 0.5), 0.0).r;
    float exposure =
        (u_adapt.w > 0.5) ? mix(previous, target, u_adapt.x) : target;
    gl_FragColor = vec4(exposure, averageLuminance, 0.0, 1.0);
}
