// Presents the post chain's final image on the back buffer: a plain
// fullscreen sample used by player mode, where no editor overlay draws
// the scene texture (#138).

#version 330 core

in vec2 vTexCoord;

uniform sampler2D u_inputTexture;

out vec4 outColor;

/// Runs the shader entry point for this stage.
void main() {
    outColor = vec4(texture(u_inputTexture, vTexCoord).rgb, 1.0);
}
