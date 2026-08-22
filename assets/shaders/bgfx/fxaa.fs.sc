$input v_texcoord0

// FXAA 3.11 quality stage (bgfx port of fxaa.frag, #138): luma-guided
// edge-end search with the correct-variation guard. GL textureOffset
// calls become explicit texel-size offsets (identical sampling — the
// offsets are whole texels at the same mip). The edge-end search
// samples at explicit LOD 0 (single-mip input, identical texels)
// because the dx11 profile's fxc rejects gradient sampling in the
// dynamic-exit search loop.

#include <bgfx_shader.sh>

SAMPLER2D(u_inputTexture, 0);

uniform vec4 u_texelSize; // .xy

#define kEdgeThresholdMin 0.0312
#define kEdgeThresholdMax 0.125
#define kSubpixelQuality 0.75
#define kSearchSteps 12

float rgb_luma(vec3 rgb) {
    return sqrt(dot(rgb, vec3(0.299, 0.587, 0.114)));
}

float luma_at(vec2 uv, vec2 texelOffset) {
    return rgb_luma(
        texture2D(u_inputTexture, uv + texelOffset * u_texelSize.xy).rgb);
}

void main() {
    vec2 uv = v_texcoord0;
    vec3 colorCenter = texture2D(u_inputTexture, uv).rgb;
    float lumaCenter = rgb_luma(colorCenter);
    float lumaDown = luma_at(uv, vec2(0.0, -1.0));
    float lumaUp = luma_at(uv, vec2(0.0, 1.0));
    float lumaLeft = luma_at(uv, vec2(-1.0, 0.0));
    float lumaRight = luma_at(uv, vec2(1.0, 0.0));

    float lumaMin = min(lumaCenter,
                        min(min(lumaDown, lumaUp), min(lumaLeft, lumaRight)));
    float lumaMax = max(lumaCenter,
                        max(max(lumaDown, lumaUp), max(lumaLeft, lumaRight)));
    float lumaRange = lumaMax - lumaMin;

    if (lumaRange < max(kEdgeThresholdMin, lumaMax * kEdgeThresholdMax)) {
        gl_FragColor = vec4(colorCenter, 1.0);
        return;
    }

    float lumaDownLeft = luma_at(uv, vec2(-1.0, -1.0));
    float lumaUpRight = luma_at(uv, vec2(1.0, 1.0));
    float lumaUpLeft = luma_at(uv, vec2(-1.0, 1.0));
    float lumaDownRight = luma_at(uv, vec2(1.0, -1.0));

    float lumaDownUp = lumaDown + lumaUp;
    float lumaLeftRight = lumaLeft + lumaRight;
    float lumaLeftCorners = lumaDownLeft + lumaUpLeft;
    float lumaDownCorners = lumaDownLeft + lumaDownRight;
    float lumaRightCorners = lumaDownRight + lumaUpRight;
    float lumaUpCorners = lumaUpRight + lumaUpLeft;

    float edgeHorizontal = abs(-2.0 * lumaLeft + lumaLeftCorners) +
                           abs(-2.0 * lumaCenter + lumaDownUp) * 2.0 +
                           abs(-2.0 * lumaRight + lumaRightCorners);
    float edgeVertical = abs(-2.0 * lumaUp + lumaUpCorners) +
                         abs(-2.0 * lumaCenter + lumaLeftRight) * 2.0 +
                         abs(-2.0 * lumaDown + lumaDownCorners);
    bool isHorizontal = edgeHorizontal >= edgeVertical;

    float luma1 = isHorizontal ? lumaDown : lumaLeft;
    float luma2 = isHorizontal ? lumaUp : lumaRight;
    float gradient1 = luma1 - lumaCenter;
    float gradient2 = luma2 - lumaCenter;
    bool is1Steepest = abs(gradient1) >= abs(gradient2);
    float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));

    float stepLength = isHorizontal ? u_texelSize.y : u_texelSize.x;
    float lumaLocalAverage;
    if (is1Steepest) {
        stepLength = -stepLength;
        lumaLocalAverage = 0.5 * (luma1 + lumaCenter);
    } else {
        lumaLocalAverage = 0.5 * (luma2 + lumaCenter);
    }

    vec2 currentUv = uv;
    if (isHorizontal) {
        currentUv.y += stepLength * 0.5;
    } else {
        currentUv.x += stepLength * 0.5;
    }

    vec2 offset = isHorizontal ? vec2(u_texelSize.x, 0.0)
                               : vec2(0.0, u_texelSize.y);
    vec2 uv1 = currentUv - offset;
    vec2 uv2 = currentUv + offset;

    float lumaEnd1 =
        rgb_luma(texture2DLod(u_inputTexture, uv1, 0.0).rgb) - lumaLocalAverage;
    float lumaEnd2 =
        rgb_luma(texture2DLod(u_inputTexture, uv2, 0.0).rgb) - lumaLocalAverage;
    bool reached1 = abs(lumaEnd1) >= gradientScaled;
    bool reached2 = abs(lumaEnd2) >= gradientScaled;

    // Progressive search step sizes from the FXAA 3.11 quality preset.
    float quality[12];
    quality[0] = 1.0; quality[1] = 1.0; quality[2] = 1.0; quality[3] = 1.0;
    quality[4] = 1.0; quality[5] = 1.5; quality[6] = 2.0; quality[7] = 2.0;
    quality[8] = 2.0; quality[9] = 2.0; quality[10] = 4.0;
    quality[11] = 8.0;

    for (int i = 1; i < kSearchSteps; i++) {
        if (reached1 && reached2) {
            break;
        }
        if (!reached1) {
            uv1 -= offset * quality[i];
            lumaEnd1 = rgb_luma(texture2DLod(u_inputTexture, uv1, 0.0).rgb) -
                       lumaLocalAverage;
            reached1 = abs(lumaEnd1) >= gradientScaled;
        }
        if (!reached2) {
            uv2 += offset * quality[i];
            lumaEnd2 = rgb_luma(texture2DLod(u_inputTexture, uv2, 0.0).rgb) -
                       lumaLocalAverage;
            reached2 = abs(lumaEnd2) >= gradientScaled;
        }
    }

    float distance1 = isHorizontal ? (uv.x - uv1.x) : (uv.y - uv1.y);
    float distance2 = isHorizontal ? (uv2.x - uv.x) : (uv2.y - uv.y);
    bool isDirection1 = distance1 < distance2;
    float distanceFinal = min(distance1, distance2);
    float edgeThickness = distance1 + distance2;
    float pixelOffset = -distanceFinal / edgeThickness + 0.5;

    bool isLumaCenterSmaller = lumaCenter < lumaLocalAverage;
    bool correctVariation =
        ((isDirection1 ? lumaEnd1 : lumaEnd2) < 0.0) != isLumaCenterSmaller;
    float finalOffset = correctVariation ? pixelOffset : 0.0;

    float lumaAverage = (1.0 / 12.0) * (2.0 * (lumaDownUp + lumaLeftRight) +
                                        lumaLeftCorners + lumaRightCorners);
    float subPixelOffset1 =
        clamp(abs(lumaAverage - lumaCenter) / lumaRange, 0.0, 1.0);
    float subPixelOffset2 =
        (-2.0 * subPixelOffset1 + 3.0) * subPixelOffset1 * subPixelOffset1;
    float subPixelOffsetFinal =
        subPixelOffset2 * subPixelOffset2 * kSubpixelQuality;
    finalOffset = max(finalOffset, subPixelOffsetFinal);

    vec2 finalUv = uv;
    if (isHorizontal) {
        finalUv.y += finalOffset * stepLength;
    } else {
        finalUv.x += finalOffset * stepLength;
    }

    gl_FragColor = vec4(texture2D(u_inputTexture, finalUv).rgb, 1.0);
}
