// Solid-color axis-aligned rect, no vertex buffer. Used by the readback test and
// the export spike; not the real renderer.
// The rect is given in pixels with a top-left origin.
@vs vs
layout(binding=0) uniform vs_params {
    vec4 rect;      // x, y, w, h in pixels, origin top-left
    vec2 target;    // render target size in pixels
};

void main() {
    // two triangles: (0,0) (1,0) (0,1) / (1,0) (1,1) (0,1)
    const vec2 corners[6] = vec2[6](
        vec2(0, 0), vec2(1, 0), vec2(0, 1),
        vec2(1, 0), vec2(1, 1), vec2(0, 1));
    vec2 c = corners[gl_VertexIndex];
    vec2 px = rect.xy + c * rect.zw;
    vec2 ndc = vec2(px.x / target.x * 2.0 - 1.0, 1.0 - px.y / target.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
@end

@fs fs
layout(binding=1) uniform fs_params {
    vec4 color;
};
out vec4 frag_color;

void main() {
    frag_color = color;
}
@end

@program solid_rect vs fs
