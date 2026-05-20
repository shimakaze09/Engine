#version 330 core

in vec3 vTexCoord;

uniform vec3 u_sunDirection;
uniform float u_turbidity;
uniform float u_groundAlbedo;

out vec4 FragColor;

const float PI = 3.14159265359;

float hosek_chi(float h, float gamma) {
  float cosGamma = cos(gamma);
  float denom = max(1.0 + h * h - 2.0 * h * cosGamma, 0.0001);
  return (1.0 + cosGamma * cosGamma) / pow(denom, 1.5);
}

vec3 xyY_to_rgb(float x, float y, float Y) {
  y = max(y, 0.0001);
  float X = (x / y) * Y;
  float Z = ((1.0 - x - y) / y) * Y;

  vec3 rgb;
  rgb.r = 3.2406 * X - 1.5372 * Y - 0.4986 * Z;
  rgb.g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
  rgb.b = 0.0557 * X - 0.2040 * Y + 1.0570 * Z;
  return max(rgb, vec3(0.0));
}

vec3 hosek_distribution(float theta, float gamma, vec3 A, vec3 B, vec3 C,
                        vec3 D, vec3 E, vec3 F, vec3 G, vec3 H, vec3 I) {
  float cosTheta = max(cos(theta), 0.01);
  float cosGamma = cos(gamma);
  vec3 chi = vec3(hosek_chi(H.r, gamma), hosek_chi(H.g, gamma),
                  hosek_chi(H.b, gamma));
  return (vec3(1.0) + A * exp(B / cosTheta)) *
         (C + D * exp(E * gamma) + F * cosGamma * cosGamma + G * chi +
          I * sqrt(cosTheta));
}

void hosek_coefficients(float turbidity, float albedo, out vec3 A, out vec3 B,
                        out vec3 C, out vec3 D, out vec3 E, out vec3 F,
                        out vec3 G, out vec3 H, out vec3 I) {
  float T = clamp(turbidity, 1.7, 10.0);
  float a = clamp(albedo, 0.0, 1.0);
  float t = (T - 1.7) / 8.3;

  vec3 hazeTint = mix(vec3(0.95, 1.02, 1.14), vec3(1.12, 1.04, 0.92), t);
  vec3 albedoLift = vec3(0.20, 0.24, 0.30) * a;

  A = mix(vec3(-1.18, -1.08, -0.92), vec3(-0.72, -0.78, -0.86), t);
  B = mix(vec3(-0.34, -0.31, -0.28), vec3(-0.12, -0.14, -0.18), t);
  C = mix(vec3(0.48, 0.62, 0.86), vec3(0.78, 0.86, 0.96), t) * hazeTint +
      albedoLift;
  D = mix(vec3(-0.72, -0.64, -0.52), vec3(-0.36, -0.32, -0.30), t);
  E = mix(vec3(0.16, 0.20, 0.28), vec3(0.42, 0.50, 0.62), t);
  F = mix(vec3(0.28, 0.38, 0.58), vec3(0.54, 0.56, 0.50), t);
  G = mix(vec3(0.055, 0.050, 0.040), vec3(0.20, 0.16, 0.10), t);
  H = mix(vec3(0.72, 0.70, 0.66), vec3(0.82, 0.80, 0.76), t);
  I = mix(vec3(0.08, 0.12, 0.20), vec3(0.30, 0.32, 0.30), t) + albedoLift;
}

vec3 hosek_wilkie_sky(vec3 direction, vec3 sunDirection, float turbidity,
                      float groundAlbedo) {
  vec3 sunDir = normalize(sunDirection);
  float sunY = clamp(sunDir.y, 0.01, 1.0);
  float thetaS = acos(sunY);
  float theta = acos(clamp(max(direction.y, 0.0), 0.0, 1.0));
  float gamma = acos(clamp(dot(direction, sunDir), -1.0, 1.0));

  vec3 A;
  vec3 B;
  vec3 C;
  vec3 D;
  vec3 E;
  vec3 F;
  vec3 G;
  vec3 H;
  vec3 I;
  hosek_coefficients(turbidity, groundAlbedo, A, B, C, D, E, F, G, H, I);

  vec3 denom = max(hosek_distribution(0.0, thetaS, A, B, C, D, E, F, G, H, I),
                   vec3(0.0001));
  vec3 distribution =
      hosek_distribution(theta, gamma, A, B, C, D, E, F, G, H, I) / denom;

  float T = clamp(turbidity, 1.7, 10.0);
  float thetaS2 = thetaS * thetaS;
  float thetaS3 = thetaS2 * thetaS;
  float T2 = T * T;
  float chi = (4.0 / 9.0 - T / 120.0) * (PI - 2.0 * thetaS);
  float zenithY = max((4.0453 * T - 4.9710) * tan(chi) - 0.2155 * T +
                          2.4192,
                      0.0);
  float zenithX =
      (0.00165 * thetaS3 - 0.00374 * thetaS2 + 0.00208 * thetaS) * T2 +
      (-0.02902 * thetaS3 + 0.06377 * thetaS2 - 0.03202 * thetaS + 0.00394) *
          T +
      (0.11693 * thetaS3 - 0.21196 * thetaS2 + 0.06052 * thetaS + 0.25885);
  float zenithYChrom =
      (0.00275 * thetaS3 - 0.00610 * thetaS2 + 0.00317 * thetaS) * T2 +
      (-0.04214 * thetaS3 + 0.08970 * thetaS2 - 0.04153 * thetaS + 0.00516) *
          T +
      (0.15346 * thetaS3 - 0.26756 * thetaS2 + 0.06670 * thetaS + 0.26688);

  vec3 zenithRgb = xyY_to_rgb(zenithX, zenithYChrom, zenithY * 0.065);
  vec3 sky = zenithRgb * max(distribution, vec3(0.0));
  float sunDisk =
      smoothstep(cos(0.011), cos(0.0035), dot(direction, sunDir));
  sky += vec3(32.0, 27.0, 18.0) * sunDisk;
  return sky;
}

void main() {
  vec3 direction = normalize(vTexCoord);
  vec3 skyColor =
      hosek_wilkie_sky(direction, u_sunDirection, u_turbidity, u_groundAlbedo);
  FragColor = vec4(skyColor, 1.0);
}
