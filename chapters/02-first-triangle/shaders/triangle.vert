#version 450

const vec2 corners[3] = vec2[3](
    vec2( 0.0, -0.65),
    vec2(-0.6,  0.55),
    vec2( 0.6,  0.55)
);

void main() {
    gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0);
}
