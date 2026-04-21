#version 330 core

layout(location = 0) in vec3 aPosition;

uniform mat4 u_lightMVP;
uniform mat4 u_model;

out vec3 vFragWorldPos;

void main() {
    vec4 worldPos = u_model * vec4(aPosition, 1.0);
    vFragWorldPos = worldPos.xyz;
    gl_Position = u_lightMVP * worldPos;
}
