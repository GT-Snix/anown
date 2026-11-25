// Flat-colored triangles in scene pixels (origin top-left, y down), with optional
// halftone / hatch fill patterns (docs/layers.md, 5C).
// `view` maps scene pixels to the current target: the main viewport (origin 0,
// size = scene) or an isolated layer's rectangle (docs/layers.md).
// `alpha_mul` is 1 except when a layer that couldn't get memory is replayed
// directly into its parent with its opacity (the over-budget fallback).
//
// Patterns: pattern0.xy are pattern coordinates in cell units (object space,
// rotated, divided by the cell size; affine, so interpolation is exact);
// pattern0.z = kind + density * 0.999 (0 = solid).
//   halftone: dot at every cell center, radius sqrt(density / pi) cells, so the
//             dot area is `density` of the cell
//   hatch:    lines along pattern-x at every cell row, width = density cells
// Edges are anti-aliased with fwidth (MSAA runs the fragment shader per pixel).
@vs shape_vs
layout(binding=0) uniform shape_params {
    vec4 view;      // origin.xy, size.zw, in scene pixels
    float alpha_mul;
};
in vec2 pos;
in vec4 color0;
in vec3 pattern0;
out vec4 color;
out vec3 pattern;

void main() {
    vec2 uv = (pos - view.xy) / view.zw;
    gl_Position = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    color = vec4(color0.rgb, color0.a * alpha_mul);
    pattern = pattern0;
}
@end

@fs shape_fs
in vec4 color;
in vec3 pattern;
out vec4 frag_color;

void main() {
    float cov = 1.0;
    float kind = floor(pattern.z + 0.0001);
    if (kind > 0.5) {
        float density = (pattern.z - kind) / 0.999;
        if (kind < 1.5) {
            // halftone
            vec2 f = fract(pattern.xy) - 0.5;
            float d = length(f);
            float r = sqrt(density / 3.14159265);
            // AA width from the pattern coordinate, which is continuous: fwidth(d)
            // would jump at every cell edge (fract), smearing coverage into the gaps
            // in a way that depends on the 2x2 quad grid, so a layer (different
            // pixel parity) and the main target would disagree.
            vec2 dx = dFdx(pattern.xy), dy = dFdy(pattern.xy);
            float px = sqrt(abs(dx.x * dy.y - dx.y * dy.x)); // one pixel, in cells
            float aa = max(px, 1e-4) * 0.5;
            cov = 1.0 - smoothstep(r - aa, r + aa, d);
        } else {
            // hatch
            float d = abs(fract(pattern.y) - 0.5);
            float hw = density * 0.5;
            float aa = max(fwidth(pattern.y), 1e-4) * 0.5;
            cov = 1.0 - smoothstep(hw - aa, hw + aa, d);
        }
    }
    frag_color = vec4(color.rgb, color.a * cov);
}
@end

@program shape shape_vs shape_fs
