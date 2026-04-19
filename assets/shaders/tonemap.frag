#version 330 core

in vec2 vTexCoord;

uniform sampler2D u_sceneColor;
uniform float u_exposure;
uniform int u_tonemapOperator; // 0=Reinhard, 1=ACES, 2=Uncharted2
uniform sampler2D u_bloomTexture;
uniform float u_bloomIntensity;
uniform int u_bloomEnabled;

out vec4 outColor;

vec3 reinhard(vec3 hdr) {
  return hdr / (hdr + vec3(1.0));
}

// ACES filmic approximation (Krzysztof Narkowicz).
vec3 aces(vec3 x) {
  const float a = 2.51;
  const float b = 0.03;
  const float c = 2.43;
  const float d = 0.59;
  const float e = 0.14;
  return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Uncharted 2 filmic (John Hable).
vec3 uncharted2Tonemap(vec3 x) {
  const float A = 0.15;
  const float B = 0.50;
  const float C = 0.10;
  const float D = 0.20;
  const float E = 0.02;
  const float F = 0.30;
  return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F;
}

vec3 uncharted2(vec3 hdr) {
  const float W = 11.2;
  vec3 curr = uncharted2Tonemap(hdr);
  vec3 whiteScale = vec3(1.0) / uncharted2Tonemap(vec3(W));
  return curr * whiteScale;
}

void main() {
  vec3 hdr = texture(u_sceneColor, vTexCoord).rgb;

  // Add bloom contribution before tone mapping.
  if (u_bloomEnabled != 0) {
    hdr += texture(u_bloomTexture, vTexCoord).rgb * u_bloomIntensity;
  }

  // Apply exposure.
  hdr *= u_exposure;

  // Select tone mapping operator.
  vec3 mapped;
  if (u_tonemapOperator == 2) {
    mapped = uncharted2(hdr);
  } else if (u_tonemapOperator == 1) {
    mapped = aces(hdr);
  } else {
    mapped = reinhard(hdr);
  }

  // sRGB gamma correction (approximate).
  outColor = vec4(pow(mapped, vec3(1.0 / 2.2)), 1.0);
}
