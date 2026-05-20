#version 330 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in mat4 aInstanceModel;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat3 uNormalMatrix;
uniform int uUseInstancing;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vTexCoord;

void main() {
    mat4 model = (uUseInstancing != 0) ? aInstanceModel : uModel;
    mat3 normalMatrix = (uUseInstancing != 0)
        ? transpose(inverse(mat3(model)))
        : uNormalMatrix;
    vec4 worldPos = model * vec4(aPosition, 1.0);
    vWorldPos = worldPos.xyz;
    vNormal = normalize(normalMatrix * aNormal);
    vTexCoord = aTexCoord;
    gl_Position = uProjection * uView * worldPos;
}
