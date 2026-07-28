// Shades depth-tested debug line vertices for the Engine renderer.

#version 330 core

in vec4 vColor;

out vec4 FragColor;

/// Runs the shader entry point for this stage.
void main() {
    FragColor = vColor;
}
