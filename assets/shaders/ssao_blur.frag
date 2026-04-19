#version 330 core

in vec2 vTexCoord;

uniform sampler2D u_ssaoInput;
uniform vec2 u_texelSize;

out float outAO;

void main() {
    float result = 0.0;
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            vec2 offset = vec2(float(x), float(y)) * u_texelSize;
            result += texture(u_ssaoInput, vTexCoord + offset).r;
        }
    }
    outAO = result / 25.0;
}
