$input v_dir

// Skybox cubemap sample.

#include <bgfx_shader.sh>

SAMPLERCUBE(u_skybox, 0);

void main() {
    vec3 skyColor = textureCube(u_skybox, normalize(v_dir)).rgb;
    gl_FragColor = vec4(skyColor, 1.0);
}
