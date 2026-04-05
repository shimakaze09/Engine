#version 450 core

out vec2 vTexCoord;

void main() {
  // Fullscreen triangle from gl_VertexID — no VAO data needed.
  // Vertex 0: (-1, -1)  UV (0, 0)
  // Vertex 1: ( 3, -1)  UV (2, 0)
  // Vertex 2: (-1,  3)  UV (0, 2)
  vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
  );

  gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
  vTexCoord = positions[gl_VertexID] * 0.5 + 0.5;
}
