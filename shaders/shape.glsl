// Flat-colored triangles in scene pixels (origin top-left, y down).
@vs vs
layout(binding=0) uniform shape_params {
    vec2 scene_size;
};
in vec2 pos;
in vec4 color0;
out vec4 color;

void main() {
    vec2 ndc = vec2(pos.x / scene_size.x * 2.0 - 1.0, 1.0 - pos.y / scene_size.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    color = color0;
}
@end

@fs fs
in vec4 color;
out vec4 frag_color;

void main() {
    frag_color = color;
}
@end

@program shape vs fs
