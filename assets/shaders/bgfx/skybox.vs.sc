$input a_position
$output v_dir

// Skybox vertex stage (bgfx port of skybox.vert, #138): the view's
// rotation only (translation dropped via w = 0) and depth pinned to the
// far plane through clip.xyww. u_viewMat, not u_view — bgfx reserves
// the latter as a predefined uniform.

#include <bgfx_shader.sh>

uniform mat4 u_viewMat;
uniform mat4 u_projection;

void main() {
    v_dir = a_position;
    vec3 rotated = mul(u_viewMat, vec4(a_position, 0.0)).xyz;
    vec4 clipPos = mul(u_projection, vec4(rotated, 1.0));
    gl_Position = clipPos.xyww;
}
