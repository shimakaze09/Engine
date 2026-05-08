#version 330 core

in vec3 vTexCoord;

uniform vec3 u_sunDirection;
uniform float u_turbidity;

out vec4 FragColor;

const float PI = 3.14159265359;

float perez(float theta, float gamma, float A, float B, float C, float D,
            float E) {
  float cosTheta = max(cos(theta), 0.01);
  float cosGamma = cos(gamma);
  return (1.0 + A * exp(B / cosTheta)) *
         (1.0 + C * exp(D * gamma) + E * cosGamma * cosGamma);
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

vec3 preetham_sky(vec3 direction, vec3 sunDirection, float turbidity) {
  float T = clamp(turbidity, 1.7, 10.0);
  vec3 sunDir = normalize(sunDirection);
  float sunY = clamp(sunDir.y, 0.01, 1.0);
  float thetaS = acos(sunY);
  float theta = acos(clamp(max(direction.y, 0.0), 0.0, 1.0));
  float gamma = acos(clamp(dot(direction, sunDir), -1.0, 1.0));

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

  float AY = 0.1787 * T - 1.4630;
  float BY = -0.3554 * T + 0.4275;
  float CY = -0.0227 * T + 5.3251;
  float DY = 0.1206 * T - 2.5771;
  float EY = -0.0670 * T + 0.3703;

  float Ax = -0.0193 * T - 0.2592;
  float Bx = -0.0665 * T + 0.0008;
  float Cx = -0.0004 * T + 0.2125;
  float Dx = -0.0641 * T - 0.8989;
  float Ex = -0.0033 * T + 0.0452;

  float Ay = -0.0167 * T - 0.2608;
  float By = -0.0950 * T + 0.0092;
  float Cy = -0.0079 * T + 0.2102;
  float Dy = -0.0441 * T - 1.6537;
  float Ey = -0.0109 * T + 0.0529;

  float denomY = max(perez(0.0, thetaS, AY, BY, CY, DY, EY), 0.0001);
  float denomX = max(perez(0.0, thetaS, Ax, Bx, Cx, Dx, Ex), 0.0001);
  float denomYChrom = max(perez(0.0, thetaS, Ay, By, Cy, Dy, Ey), 0.0001);

  float Y = zenithY * perez(theta, gamma, AY, BY, CY, DY, EY) / denomY;
  float x = zenithX * perez(theta, gamma, Ax, Bx, Cx, Dx, Ex) / denomX;
  float y = zenithYChrom * perez(theta, gamma, Ay, By, Cy, Dy, Ey) /
            denomYChrom;

  vec3 sky = xyY_to_rgb(x, y, Y * 0.06);
  float sunDisk =
      smoothstep(cos(0.012), cos(0.004), dot(direction, sunDir));
  sky += vec3(24.0, 20.0, 14.0) * sunDisk;
  return sky;
}

void main() {
  vec3 direction = normalize(vTexCoord);
  vec3 skyColor = preetham_sky(direction, u_sunDirection, u_turbidity);
  FragColor = vec4(skyColor, 1.0);
}
