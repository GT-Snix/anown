// Composite a resolved, premultiplied layer into the current target
// (docs/layers.md). The quad covers the layer's rectangle in the current
// target's pixels; texels are fetched 1:1 (no filtering, no resampling).
// Output is premultiplied x opacity; blend is ONE, ONE_MINUS_SRC_ALPHA.
@vs comp_vs
layout(binding=0) uniform comp_vs_params {
    vec4 rect;      // x, y, w, h in current-target pixels
    vec2 target;    // current target size in pixels (the viewport)
};

void main() {
    const vec2 corners[6] = vec2[6](
        vec2(0, 0), vec2(1, 0), vec2(0, 1),
        vec2(1, 0), vec2(1, 1), vec2(0, 1));
    vec2 px = rect.xy + corners[gl_VertexIndex] * rect.zw;
    gl_Position = vec4(px.x / target.x * 2.0 - 1.0, 1.0 - px.y / target.y * 2.0, 0.0, 1.0);
}
@end

@fs comp_fs
layout(binding=1) uniform comp_fs_params {
    vec2 src_origin; // where texel (0,0) of the layer sits, in current-target pixels
    float opacity;
};
layout(binding=0) uniform texture2D layer_tex;
layout(binding=0) uniform sampler layer_smp;
out vec4 frag_color;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy) - ivec2(src_origin);
    frag_color = texelFetch(sampler2D(layer_tex, layer_smp), p, 0) * opacity;
}
@end

@program composite comp_vs comp_fs
