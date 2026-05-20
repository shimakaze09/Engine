#version 330 core

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in mat4 inInstanceModel;

uniform mat4 u_model;
uniform mat4 u_mvp;
uniform mat4 u_viewProjection;
uniform mat3 u_normalMatrix;
uniform int uUseInstancing;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vTexCoord;

void main() {
  mat4 model = (uUseInstancing != 0) ? inInstanceModel : u_model;
  mat3 normalMatrix = (uUseInstancing != 0)
    ? transpose(inverse(mat3(model)))
    : u_normalMatrix;
  vec4 worldPos = model * vec4(inPosition, 1.0);
  vWorldPos = worldPos.xyz;
  vNormal = normalize(normalMatrix * inNormal);
  vTexCoord = inTexCoord;
  gl_Position = (uUseInstancing != 0)
    ? (u_viewProjection * worldPos)
    : (u_mvp * vec4(inPosition, 1.0));
}
