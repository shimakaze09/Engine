// FXAA 3.11 quality fragment shader (Lottes algorithm) used by the Engine
// renderer: luma-guided edge-end search with the correct-variation guard.

#version 330 core

in vec2 vTexCoord;

uniform sampler2D u_inputTexture;
uniform vec2 u_texelSize;

out vec4 outColor;

const float kEdgeThresholdMin = 0.0312;
const float kEdgeThresholdMax = 0.125;
const float kSubpixelQuality = 0.75;
const int kSearchSteps = 12;

// Progressive search step sizes from the FXAA 3.11 quality preset.
const float kQuality[12] = float[12](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0,
                                     2.0, 2.0, 4.0, 8.0);

/// Perceptual luma from the tonemapped input.
float rgb_luma(vec3 rgb) {
    return sqrt(dot(rgb, vec3(0.299, 0.587, 0.114)));
}

/// Runs the shader entry point for this stage.
void main() {
    vec3 colorCenter = texture(u_inputTexture, vTexCoord).rgb;
    float lumaCenter = rgb_luma(colorCenter);
    float lumaDown =
        rgb_luma(textureOffset(u_inputTexture, vTexCoord, ivec2(0, -1)).rgb);
    float lumaUp =
        rgb_luma(textureOffset(u_inputTexture, vTexCoord, ivec2(0, 1)).rgb);
    float lumaLeft =
        rgb_luma(textureOffset(u_inputTexture, vTexCoord, ivec2(-1, 0)).rgb);
    float lumaRight =
        rgb_luma(textureOffset(u_inputTexture, vTexCoord, ivec2(1, 0)).rgb);

    float lumaMin =
        min(lumaCenter, min(min(lumaDown, lumaUp), min(lumaLeft, lumaRight)));
    float lumaMax =
        max(lumaCenter, max(max(lumaDown, lumaUp), max(lumaLeft, lumaRight)));
    float lumaRange = lumaMax - lumaMin;

    if (lumaRange < max(kEdgeThresholdMin, lumaMax * kEdgeThresholdMax)) {
        outColor = vec4(colorCenter, 1.0);
        return;
    }

    float lumaDownLeft =
        rgb_luma(textureOffset(u_inputTexture, vTexCoord, ivec2(-1, -1)).rgb);
    float lumaUpRight =
        rgb_luma(textureOffset(u_inputTexture, vTexCoord, ivec2(1, 1)).rgb);
    float lumaUpLeft =
        rgb_luma(textureOffset(u_inputTexture, vTexCoord, ivec2(-1, 1)).rgb);
    float lumaDownRight =
        rgb_luma(textureOffset(u_inputTexture, vTexCoord, ivec2(1, -1)).rgb);

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

    vec2 currentUv = vTexCoord;
    if (isHorizontal) {
        currentUv.y += stepLength * 0.5;
    } else {
        currentUv.x += stepLength * 0.5;
    }

    vec2 offset = isHorizontal ? vec2(u_texelSize.x, 0.0)
                               : vec2(0.0, u_texelSize.y);
    vec2 uv1 = currentUv - offset;
    vec2 uv2 = currentUv + offset;

    float lumaEnd1 = rgb_luma(texture(u_inputTexture, uv1).rgb) -
                     lumaLocalAverage;
    float lumaEnd2 = rgb_luma(texture(u_inputTexture, uv2).rgb) -
                     lumaLocalAverage;
    bool reached1 = abs(lumaEnd1) >= gradientScaled;
    bool reached2 = abs(lumaEnd2) >= gradientScaled;

    for (int i = 1; i < kSearchSteps && !(reached1 && reached2); i++) {
        if (!reached1) {
            uv1 -= offset * kQuality[i];
            lumaEnd1 = rgb_luma(texture(u_inputTexture, uv1).rgb) -
                       lumaLocalAverage;
            reached1 = abs(lumaEnd1) >= gradientScaled;
        }
        if (!reached2) {
            uv2 += offset * kQuality[i];
            lumaEnd2 = rgb_luma(texture(u_inputTexture, uv2).rgb) -
                       lumaLocalAverage;
            reached2 = abs(lumaEnd2) >= gradientScaled;
        }
    }

    float distance1 =
        isHorizontal ? (vTexCoord.x - uv1.x) : (vTexCoord.y - uv1.y);
    float distance2 =
        isHorizontal ? (uv2.x - vTexCoord.x) : (uv2.y - vTexCoord.y);
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

    vec2 finalUv = vTexCoord;
    if (isHorizontal) {
        finalUv.y += finalOffset * stepLength;
    } else {
        finalUv.x += finalOffset * stepLength;
    }

    outColor = vec4(texture(u_inputTexture, finalUv).rgb, 1.0);
}
