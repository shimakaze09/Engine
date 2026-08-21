// Shared shaderc varying/attribute declarations for the Engine's bgfx
// shader ports (#138 Phase C): every .sc in this directory compiles
// against this table, so attribute semantics stay aligned with the
// backend's VertexSemantic -> bgfx::Attrib mapping.

vec4 v_color0    : COLOR0;
vec2 v_texcoord0 : TEXCOORD0;
vec3 v_normal    : NORMAL;
vec3 v_worldpos  : TEXCOORD7;
vec3 v_dir       : TEXCOORD1;

vec3 a_position  : POSITION;
vec3 a_normal    : NORMAL;
vec2 a_texcoord0 : TEXCOORD0;
vec4 a_color0    : COLOR0;
vec4 a_indices   : BLENDINDICES;
vec4 a_weight    : BLENDWEIGHT;

vec4 i_data0     : TEXCOORD31;
vec4 i_data1     : TEXCOORD30;
vec4 i_data2     : TEXCOORD29;
vec4 i_data3     : TEXCOORD28;
vec4 i_data4     : TEXCOORD27;
