// Internal: the stroke triangulator. Turns a flattened path into a
// variable-width ribbon with round caps, round joins and optional corner
// overshoot. Pure function of its inputs; see docs/strokes.md.
#ifndef ANOWN_STROKE_INTERNAL_H
#define ANOWN_STROKE_INTERNAL_H

#include "path_internal.h"
#include "tess_internal.h"

// Width multiplier at arc length s within a subpath of length len.
double ani_brush_factor(const ani_brush *b, double s, double len);

// Line boil (docs/layers.md, 5C): centerline points displaced along their normal
// by a value noise over arc length, re-rolled at 12 Hz. amp 0 = off: no
// subdivision and no displacement, so the output is byte-identical to no boil.
typedef struct ani_boil {
    float amp;        // scene px (style boil x effective chaos)
    float spacing;    // arc length between noise samples, local units
    uint64_t seed;    // scene seed
    uint64_t stream;  // the object's RNG key (name hash or creation id)
    uint64_t step;    // (frame * 12) / fps
} ani_boil;

// Displacement at arc length s of subpath `sub` (length len). Closed subpaths use
// a spacing that tiles the loop exactly, so the noise is continuous at the seam.
double ani_boil_disp(const ani_boil *b, int sub, double s, double len, bool closed);

// The first subpath as a closed ring in scene space, subdivided at boil
// buckets and displaced like the outline (for fills). Returns the point count and
// the points in *out, which live in m's scratch until the next stroke call;
// -1 on allocation failure.
int ani_boil_ring(ani_mesh *m, const ani_path *p, ani_affine w, const ani_boil *b, const an_vec2 **out);

typedef struct ani_stroke_in {
    const ani_path *path;
    ani_affine world;   // local -> scene
    float width;        // stroke width, local units (scaled by the world transform)
    float progress;     // draw progress 0..1 along total arc length
    an_color color;
    const ani_brush *brush;
    ani_boil boil;      // zero = no boil
} ani_stroke_in;

// For tests: one rib per centerline point that made it into the ribbon.
typedef struct ani_rib {
    an_vec2 p;   // centerline point, scene space
    an_vec2 n;   // unit normal used for the segment quads at this point
    float h;     // half width, scene pixels
    float s;     // arc length within the subpath (local units)
    int sub;     // subpath index
} ani_rib;

typedef struct ani_rib_log {
    ani_rib *ribs;
    int count, cap;
} ani_rib_log;

// Append the stroke's triangles to m. log may be NULL. False on allocation failure.
bool ani_stroke_emit(ani_mesh *m, const ani_stroke_in *in, ani_rib_log *log);

// Joins sharper than this (radians of turning) get an overshoot spur.
#define ANI_OVERSHOOT_MIN_ANGLE 0.436332313f /* 25 degrees */

#endif
