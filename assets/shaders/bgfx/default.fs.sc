$input v_normal

// Default fragment stage (bgfx port of default.frag, #138): the simple
// pulsing-lambert fallback material. Scalar GL uniforms become vec4
// read through .x/.xyz per bgfx's uniform model.

#include <bgfx_shader.sh>

uniform vec4 u_time;   // .x: seconds
uniform vec4 u_albedo; // .xyz: material color

void main() {
    vec3 lightDirection = normalize(vec3(0.4, 1.0, 0.6));
    float diffuse = max(dot(normalize(v_normal), lightDirection), 0.0);
    float pulse = 0.95 + (0.05 * sin(u_time.x * 0.5));
    vec3 albedo = u_albedo.xyz;
    gl_FragColor = vec4(albedo * (diffuse * 0.9 + 0.1) * pulse, 1.0);
}
