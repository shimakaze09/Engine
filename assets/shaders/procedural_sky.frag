// Procedural scatter sky fragment shader: Rayleigh/Mie-style gradient with a
// sun disc and halo, driven by sun direction, turbidity, and ground albedo.

#version 330 core

in vec3 vTexCoord;

uniform vec3 u_sunDirection;
uniform float u_turbidity;
uniform float u_groundAlbedo;

out vec4 FragColor;

// Rayleigh scattering favors short wavelengths: normalized inverse-4th-power
// weights for the engine's linear-sRGB primaries.
const vec3 kRayleighWeight = vec3(0.175, 0.409, 1.0);

/// Henyey-Greenstein phase lobe for the forward-scattered sun halo.
float mie_phase(float cosTheta, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(denom, 0.0001), 1.5));
}

/// Evaluates the sky radiance for one world-space view direction.
vec3 scatter_sky(vec3 direction, vec3 sunDirection, float turbidity,
                 float groundAlbedo) {
    vec3 viewDir = normalize(direction);
    vec3 sunDir = normalize(sunDirection);
    float cosSun = dot(viewDir, sunDir);

    // Relative air mass grows toward the horizon; clamp below the horizon so
    // the ground hemisphere stays finite.
    float upness = clamp(viewDir.y, -1.0, 1.0);
    float airMass = 1.0 / max(upness * 0.9 + 0.14, 0.02);

    // Turbidity in [1.7, 10]: haze whitens the horizon and fattens the halo.
    float T = clamp(turbidity, 1.7, 10.0);
    float haze = (T - 1.7) / 8.3;

    // Rayleigh extinction along the view path selects the sky chroma: short
    // paths keep deep zenith blue, long horizon paths desaturate.
    vec3 extinction = exp(-kRayleighWeight * airMass * (0.32 + 0.14 * haze));
    vec3 skyChroma = (1.0 - extinction);

    // Sun elevation controls overall brightness and the sunset warm shift.
    float sunHeight = clamp(sunDir.y, -0.1, 1.0);
    float daylight = smoothstep(-0.08, 0.15, sunHeight);
    float sunset = 1.0 - smoothstep(0.05, 0.35, sunHeight);
    vec3 sunTint = mix(vec3(1.0), vec3(1.0, 0.55, 0.28), sunset * 0.85);

    // Base sky: chroma scaled into a daylight radiance range the exposure
    // pipeline expects, warmed near the sun by the sunset tint.
    vec3 sky = skyChroma * mix(2.0, 1.2, haze) * daylight;
    sky *= mix(vec3(1.0), sunTint, 0.35 + 0.5 * sunset);

    // Artist push: deepen and cool the zenith, leave the horizon airy, so
    // exposure adaptation cannot flatten the dome to gray.
    vec3 zenithGrade = mix(vec3(1.04, 1.0, 0.96), vec3(0.62, 0.80, 1.22),
                           clamp(upness * 1.5, 0.0, 1.0));
    sky *= zenithGrade;

    // Auto-exposure normalizes luminance, so only chroma survives it: boost
    // saturation strongly overhead, gently at the horizon.
    float skyLuma = dot(sky, vec3(0.2126, 0.7152, 0.0722));
    float chromaBoost = mix(1.2, 2.1, clamp(upness * 1.8, 0.0, 1.0));
    sky = max(vec3(skyLuma) + (sky - vec3(skyLuma)) * chromaBoost, vec3(0.0));

    // Mie forward halo around the sun; haze widens and brightens it.
    float halo = mie_phase(cosSun, mix(0.76, 0.65, haze));
    sky += sunTint * halo * (0.35 + 0.65 * haze) * daylight;

    // Sun disc (~0.53 degrees) with a soft limb.
    float sunDisk = smoothstep(cos(0.0093), cos(0.0046), cosSun);
    sky += sunTint * sunDisk * 40.0 * daylight;

    // Below the horizon: fade to a ground haze that borrows the sky's
    // horizon color and the configured ground albedo.
    float horizonBlend = smoothstep(0.0, -0.12, upness);
    vec3 groundColor =
        mix(vec3(0.42, 0.44, 0.47), vec3(0.60, 0.62, 0.66), groundAlbedo) *
        (0.25 + 0.6 * daylight);
    sky = mix(sky, groundColor, horizonBlend);

    // Night floor keeps the dome from collapsing to pure black.
    sky += vec3(0.004, 0.005, 0.009) * (1.0 - daylight);
    return sky;
}

/// Runs the shader entry point for this stage.
void main() {
    vec3 skyColor = scatter_sky(vTexCoord, u_sunDirection, u_turbidity,
                                u_groundAlbedo);
    FragColor = vec4(skyColor, 1.0);
}
