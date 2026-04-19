#version 330 core

in vec2 vTexCoord;

uniform sampler2D u_inputTexture;
uniform vec2 u_texelSize;

out vec4 outColor;

float luminance(vec3 c) {
  return dot(c, vec3(0.299, 0.587, 0.114));
}

void main() {
  vec3 rgbM = texture(u_inputTexture, vTexCoord).rgb;
  float lumaM = luminance(rgbM);
  float lumaN = luminance(textureOffset(u_inputTexture, vTexCoord, ivec2( 0, 1)).rgb);
  float lumaS = luminance(textureOffset(u_inputTexture, vTexCoord, ivec2( 0,-1)).rgb);
  float lumaE = luminance(textureOffset(u_inputTexture, vTexCoord, ivec2( 1, 0)).rgb);
  float lumaW = luminance(textureOffset(u_inputTexture, vTexCoord, ivec2(-1, 0)).rgb);

  float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
  float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
  float lumaRange = lumaMax - lumaMin;

  // Skip low-contrast pixels.
  if (lumaRange < max(0.0312, lumaMax * 0.125)) {
    outColor = vec4(rgbM, 1.0);
    return;
  }

  float lumaNW = luminance(textureOffset(u_inputTexture, vTexCoord, ivec2(-1, 1)).rgb);
  float lumaNE = luminance(textureOffset(u_inputTexture, vTexCoord, ivec2( 1, 1)).rgb);
  float lumaSW = luminance(textureOffset(u_inputTexture, vTexCoord, ivec2(-1,-1)).rgb);
  float lumaSE = luminance(textureOffset(u_inputTexture, vTexCoord, ivec2( 1,-1)).rgb);

  float lumaH = lumaN + lumaS;
  float lumaV = lumaE + lumaW;
  float lumaNWNE = lumaNW + lumaNE;
  float lumaSWSE = lumaSW + lumaSE;

  bool isHorizontal =
    abs(-2.0 * lumaN + lumaNWNE) +
    abs(-2.0 * lumaM + lumaH) * 2.0 +
    abs(-2.0 * lumaS + lumaSWSE)
    >=
    abs(-2.0 * lumaE + lumaNE + lumaSE) +
    abs(-2.0 * lumaM + lumaV) * 2.0 +
    abs(-2.0 * lumaW + lumaNW + lumaSW);

  float lengthSign = isHorizontal ? u_texelSize.y : u_texelSize.x;
  float lumaP1 = isHorizontal ? lumaN : lumaE;
  float lumaP2 = isHorizontal ? lumaS : lumaW;
  float gradP1 = abs(lumaP1 - lumaM);
  float gradP2 = abs(lumaP2 - lumaM);

  if (gradP1 < gradP2) lengthSign = -lengthSign;

  float subpixA = (lumaH * 2.0 + lumaV * 2.0 + lumaNWNE + lumaSWSE) / 12.0;
  float subpixB = clamp(abs(subpixA - lumaM) / lumaRange, 0.0, 1.0);
  float subpixC = (-2.0 * subpixB + 3.0) * subpixB * subpixB;
  float subpixFactor = subpixC * subpixC * 0.75;

  vec2 posM = vTexCoord;
  vec2 offNP;
  if (isHorizontal) {
    posM.y += lengthSign * 0.5;
    offNP = vec2(u_texelSize.x, 0.0);
  } else {
    posM.x += lengthSign * 0.5;
    offNP = vec2(0.0, u_texelSize.y);
  }

  // Edge search along perpendicular direction.
  vec2 posN = posM - offNP;
  vec2 posP = posM + offNP;
  float lumaEndN = luminance(texture(u_inputTexture, posN).rgb) - subpixA;
  float lumaEndP = luminance(texture(u_inputTexture, posP).rgb) - subpixA;

  bool doneN = abs(lumaEndN) >= gradP1 * 0.25;
  bool doneP = abs(lumaEndP) >= gradP1 * 0.25;

  for (int i = 0; i < 12 && !(doneN && doneP); i++) {
    if (!doneN) {
      posN -= offNP;
      lumaEndN = luminance(texture(u_inputTexture, posN).rgb) - subpixA;
      doneN = abs(lumaEndN) >= gradP1 * 0.25;
    }
    if (!doneP) {
      posP += offNP;
      lumaEndP = luminance(texture(u_inputTexture, posP).rgb) - subpixA;
      doneP = abs(lumaEndP) >= gradP1 * 0.25;
    }
  }

  float distN = isHorizontal ? (vTexCoord.x - posN.x) : (vTexCoord.y - posN.y);
  float distP = isHorizontal ? (posP.x - vTexCoord.x) : (posP.y - vTexCoord.y);
  float dist = min(distN, distP);
  float spanLen = distN + distP;
  float pixelOffset = -dist / spanLen + 0.5;

  bool goodSpan = ((distN < distP) ? lumaEndN : lumaEndP) < 0.0;
  float finalOffset = goodSpan ? max(pixelOffset, subpixFactor) : subpixFactor;

  vec2 finalUv = vTexCoord;
  if (isHorizontal) finalUv.y += finalOffset * lengthSign;
  else finalUv.x += finalOffset * lengthSign;

  outColor = vec4(texture(u_inputTexture, finalUv).rgb, 1.0);
}
