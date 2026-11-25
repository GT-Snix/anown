// Paper background (docs/layers.md, 5B): the palette's paper color, grain from
// an_hash per scene pixel at a 12 Hz (or per-frame) step, and a radial vignette.
// Drawn with the background quad's vertices (scene pixels), so the interpolated
// position is the scene coordinate of the fragment.
//
//   grain:    paper + (an_hash_float(seed, x | y << 16, step) - 0.5) * grain
//   vignette: * (1 - vignette * smoothstep(0.4, 1.0, r)),
//             r = |pos - center| / |half size| / sqrt(2)
// `grain` and `vignette` arrive already multiplied by the scene's chaos.
@include hash.glsl

@vs paper_vs
layout(binding=0) uniform paper_vs_params {
    vec4 view; // origin.xy, size.zw in scene pixels (as the shape shader)
};
in vec2 pos;
in vec4 color0;
out vec2 scene_pos;

void main() {
    vec2 uv = (pos - view.xy) / view.zw;
    gl_Position = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    scene_pos = pos;
}
@end

@fs paper_fs
@include_block an_hash
layout(binding=1) uniform paper_fs_params {
    vec4 paper;
    vec4 amounts;  // x: grain, y: vignette, zw: scene size
    ivec4 ids;     // x, y: seed lo/hi (bit patterns), z: grain step
};
in vec2 scene_pos;
out vec4 frag_color;

void main() {
    vec3 c = paper.rgb;
    if (amounts.x > 0.0) {
        uvec2 px = uvec2(floor(scene_pos));
        uint stream_lo = px.x | (px.y << 16u);
        float g = an_hash_float(uvec2(uint(ids.x), uint(ids.y)), uvec2(stream_lo, 0u), uvec2(uint(ids.z), 0u));
        c += (g - 0.5) * amounts.x;
    }
    if (amounts.y > 0.0) {
        vec2 half_size = amounts.zw * 0.5;
        float r = length((scene_pos - half_size) / half_size) / sqrt(2.0);
        c *= 1.0 - amounts.y * smoothstep(0.4, 1.0, r);
    }
    frag_color = vec4(clamp(c, 0.0, 1.0), 1.0);
}
@end

@program paper paper_vs paper_fs
