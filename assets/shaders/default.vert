#version 450 core
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
uniform mat4 u_mvp;
uniform mat3 u_normalMatrix;
out vec3 vNormal;

void main() {
  gl_Position = u_mvp * vec4(inPosition, 1.0);
  vNormal = u_normalMatrix * inNormal;
}
