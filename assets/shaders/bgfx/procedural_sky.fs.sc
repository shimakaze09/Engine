$input v_dir

// Procedural scatter sky (bgfx port of procedural_sky.frag, #138):
// Rayleigh/Mie-style gradient with a sun disc and halo. Scalar GL
// uniforms become vec4 read through .x/.xyz.

#include <bgfx_shader.sh>

uniform vec4 u_sunDirection; // .xyz
uniform vec4 u_turbidity;    // .x
uniform vec4 u_groundAlbedo; // .x

float mie_phase(float cosTheta, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) /
           (4.0 * 3.14159265 * pow(max(denom, 0.0001), 1.5));
}

vec3 scatter_sky(vec3 direction, vec3 sunDirection, float turbidity,
                 float groundAlbedo) {
    vec3 kRayleighWeight = vec3(0.175, 0.409, 1.0);
    vec3 viewDir = normalize(direction);
    vec3 sunDir = normalize(sunDirection);
    float cosSun = dot(viewDir, sunDir);

    float upness = clamp(viewDir.y, -1.0, 1.0);
    float airMass = 1.0 / max(upness * 0.9 + 0.14, 0.02);

    float T = clamp(turbidity, 1.7, 10.0);
    float haze = (T - 1.7) / 8.3;

    vec3 extinction =
        exp(-kRayleighWeight * airMass * (0.32 + 0.14 * haze));
    vec3 skyChroma = (vec3_splat(1.0) - extinction);

    float sunHeight = clamp(sunDir.y, -0.1, 1.0);
    float daylight = smoothstep(-0.08, 0.15, sunHeight);
    float sunset = 1.0 - smoothstep(0.05, 0.35, sunHeight);
    vec3 sunTint =
        mix(vec3_splat(1.0), vec3(1.0, 0.55, 0.28), sunset * 0.85);

    vec3 sky = skyChroma * mix(2.0, 1.2, haze) * daylight;
    sky *= mix(vec3_splat(1.0), sunTint, 0.35 + 0.5 * sunset);

    vec3 zenithGrade = mix(vec3(1.04, 1.0, 0.96), vec3(0.62, 0.80, 1.22),
                           clamp(upness * 1.5, 0.0, 1.0));
    sky *= zenithGrade;

    float skyLuma = dot(sky, vec3(0.2126, 0.7152, 0.0722));
    float chromaBoost = mix(1.2, 2.1, clamp(upness * 1.8, 0.0, 1.0));
    sky = max(vec3_splat(skyLuma) +
                  (sky - vec3_splat(skyLuma)) * chromaBoost,
              vec3_splat(0.0));

    float halo = mie_phase(cosSun, mix(0.76, 0.65, haze));
    sky += sunTint * halo * (0.35 + 0.65 * haze) * daylight;

    float sunDisk = smoothstep(cos(0.0093), cos(0.0046), cosSun);
    sky += sunTint * sunDisk * 40.0 * daylight;

    float horizonBlend = smoothstep(0.0, -0.12, upness);
    vec3 groundColor = mix(vec3(0.42, 0.44, 0.47), vec3(0.60, 0.62, 0.66),
                           groundAlbedo) *
                       (0.25 + 0.6 * daylight);
    sky = mix(sky, groundColor, horizonBlend);

    sky += vec3(0.004, 0.005, 0.009) * (1.0 - daylight);
    return sky;
}

void main() {
    vec3 skyColor = scatter_sky(v_dir, u_sunDirection.xyz, u_turbidity.x,
                                u_groundAlbedo.x);
    gl_FragColor = vec4(skyColor, 1.0);
}
