$input a_position, a_normal, a_texcoord0, i_data0, i_data1, i_data2, i_data3, i_data4
$output v_worldpos, v_normal, v_texcoord0

// Instanced PBR forward vertex stage (#138 instancing unit): the model
// matrix arrives as four instance-stream columns (i_data0..3, the
// flush's InstanceAttributes layout) and the per-instance foliage
// payload as i_data4 (.x wind phase). The normal matrix is the
// inverse-transpose of the instance model's upper 3x3, computed via the
// cofactor/determinant form because HLSL-path shaderc has no inverse().
// Shares pbr.fs.sc; a separate source because shaderc cannot guard
// $input lines.

#include <bgfx_shader.sh>

uniform mat4 u_viewProjection;
uniform vec4 u_time;                 // .x: seconds
uniform vec4 uFoliageWindStrength;   // .x
uniform vec4 uFoliageWindFrequency;  // .x

void main() {
    mat4 model = mtxFromCols(i_data0, i_data1, i_data2, i_data3);
    vec3 c0 = i_data0.xyz;
    vec3 c1 = i_data1.xyz;
    vec3 c2 = i_data2.xyz;
    // Inverse-transpose of the upper 3x3: cofactor columns over the
    // determinant (exactly transpose(inverse(mat3(model)))).
    vec3 cof0 = cross(c1, c2);
    vec3 cof1 = cross(c2, c0);
    vec3 cof2 = cross(c0, c1);
    float det = dot(c0, cof0);
    float invDet = 1.0 / ((abs(det) > 1e-8) ? det : 1e-8);
    vec3 n = (a_normal.x * cof0 + a_normal.y * cof1 + a_normal.z * cof2) *
             invDet;

    vec4 worldPos = mul(model, vec4(a_position, 1.0));
    float windStrength = uFoliageWindStrength.x;
    if (windStrength > 0.0) {
        float heightFactor = clamp(a_position.y * 2.0, 0.0, 1.0);
        float bend = heightFactor * heightFactor;
        float waveArg = ((worldPos.x + worldPos.z) *
                         uFoliageWindFrequency.x) +
                        u_time.x + i_data4.x;
        float sway = sin(waveArg) * windStrength * bend;
        worldPos.x += sway;
        worldPos.z += cos(waveArg * 0.73) * windStrength * 0.35 * bend;
    }
    v_worldpos = worldPos.xyz;
    v_normal = normalize(n);
    v_texcoord0 = a_texcoord0;
    gl_Position = mul(u_viewProjection, worldPos);
}
