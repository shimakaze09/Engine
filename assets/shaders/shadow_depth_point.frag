// Defines the shadow depth point fragment shader used by the Engine renderer.

#version 330 core

in vec3 vFragWorldPos;

uniform vec3 u_lightPos;
uniform float u_farPlane;

/// Runs the shader entry point for this stage.
void main() {
    float dist = length(vFragWorldPos - u_lightPos);
    gl_FragDepth = dist / u_farPlane;
}
