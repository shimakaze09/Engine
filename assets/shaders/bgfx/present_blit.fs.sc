$input v_texcoord0

// Present pass (#138): samples the post chain's final image onto the
// back buffer as a fullscreen draw. Player mode uses it in place of the
// editor's ImGui viewport image, which is the path that otherwise
// carries the scene texture to the swapchain.

#include <bgfx_shader.sh>

SAMPLER2D(u_inputTexture, 0);

void main() {
    gl_FragColor = vec4(texture2D(u_inputTexture, v_texcoord0).rgb, 1.0);
}
