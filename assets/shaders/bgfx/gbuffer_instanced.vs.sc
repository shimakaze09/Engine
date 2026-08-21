$input a_position, a_normal, a_texcoord0, i_data0, i_data1, i_data2, i_data3, i_data4
$output v_worldpos, v_normal, v_texcoord0

// Instanced G-buffer vertex stage (#138 instancing unit): instance
// model columns in i_data0..3, per-instance foliage phase in i_data4.x
// (the flush's InstanceAttributes layout). Normal matrix via the
// cofactor/determinant inverse-transpose (no inverse() on HLSL-path
// shaderc). Shares gbuffer.fs.sc; separate source because shaderc
// cannot guard $input lines.

#include <bgfx_shader.sh>

uniform mat4 uView;
uniform mat4 uProjection;
uniform vec4 uTime;                 // .x: seconds
uniform vec4 uFoliageWindStrength;  // .x
uniform vec4 uFoliageWindFrequency; // .x

void main() {
    mat4 model = mtxFromCols(i_data0, i_data1, i_data2, i_data3);
    vec3 c0 = i_data0.xyz;
    vec3 c1 = i_data1.xyz;
    vec3 c2 = i_data2.xyz;
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
                        uTime.x + i_data4.x;
        float sway = sin(waveArg) * windStrength * bend;
        worldPos.x += sway;
        worldPos.z += cos(waveArg * 0.73) * windStrength * 0.35 * bend;
    }
    v_worldpos = worldPos.xyz;
    v_normal = normalize(n);
    v_texcoord0 = a_texcoord0;
    gl_Position = mul(uProjection, mul(uView, worldPos));
}
