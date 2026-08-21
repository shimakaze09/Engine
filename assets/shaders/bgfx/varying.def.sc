// Shared shaderc varying/attribute declarations for the Engine's bgfx
// shader ports (#138 Phase C): every .sc in this directory compiles
// against this table, so attribute semantics stay aligned with the
// backend's VertexSemantic -> bgfx::Attrib mapping.

vec4 v_color0    : COLOR0;
vec2 v_texcoord0 : TEXCOORD0;

vec3 a_position  : POSITION;
vec4 a_color0    : COLOR0;
