#version 450 core

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

uniform mat4 u_model;
uniform mat4 u_mvp;
uniform mat3 u_normalMatrix;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vTexCoord;

void main() {
  vec4 worldPos = u_model * vec4(inPosition, 1.0);
  vWorldPos = worldPos.xyz;
  vNormal = normalize(u_normalMatrix * inNormal);
  vTexCoord = inTexCoord;
  gl_Position = u_mvp * vec4(inPosition, 1.0);
}
