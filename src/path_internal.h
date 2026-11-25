// Internal: flattened paths. Built once at script time (object creation) and
// read-only afterwards; rendering never modifies or caches into them.
#ifndef ANOWN_PATH_INTERNAL_H
#define ANOWN_PATH_INTERNAL_H

#include "anown.h"
#include "arena_internal.h"

// Flattening tolerance in local units: max distance between a curve and its
// polyline. A scene flattens at 0.2 / max_zoom (docs/camera.md); this is the
// value at the default max_zoom of 4.
#define ANI_FLATNESS 0.05f

typedef struct ani_subpath {
    int first, count; // range in ani_path.pts / .s
    bool closed;      // the closing segment (last -> first) is part of it
    float len;        // arc length, including the closing segment
    float s0;         // arc length of the whole path before this subpath
} ani_subpath;

// The brush (width profile), copied into the scene arena at object creation.
typedef struct ani_brush {
    float taper_in, taper_out; // local arc length
    float overshoot;           // in stroke widths
    const float *pressure;
    int pressure_count;
} ani_brush;

typedef struct ani_path {
    an_vec2 *pts;   // local coordinates
    float *s;       // arc length from the subpath's start, per point
    int npts;
    ani_subpath *subs;
    int nsubs;
    float total;    // sum of subpath lengths: draw progress 1 == total
} ani_path;

// Builder: collects points in malloc'd scratch, then copies into the arena.
typedef struct ani_path_builder {
    an_vec2 *pts;
    int npts, cap;
    ani_subpath *subs;
    int nsubs, subcap;
    bool open; // a subpath is in progress
} ani_path_builder;

void ani_pb_init(ani_path_builder *b);
void ani_pb_free(ani_path_builder *b);
bool ani_pb_move(ani_path_builder *b, an_vec2 p);
bool ani_pb_line(ani_path_builder *b, an_vec2 p);
bool ani_pb_quad(ani_path_builder *b, an_vec2 c, an_vec2 p, float tol);
bool ani_pb_cubic(ani_path_builder *b, an_vec2 c1, an_vec2 c2, an_vec2 p, float tol);
void ani_pb_close(ani_path_builder *b);
// Copy into the arena with arc lengths computed. NULL on allocation failure.
ani_path *ani_pb_finish(ani_path_builder *b, ani_arena *a);

// Convenience builders for the built-in shapes (local coordinates).
ani_path *ani_path_segs(ani_arena *a, const an_seg *segs, int n, float tol);
ani_path *ani_path_rect(ani_arena *a, an_vec2 size);
ani_path *ani_path_circle(ani_arena *a, float radius, float tol);
ani_path *ani_path_line(ani_arena *a, an_vec2 from, an_vec2 to);
ani_path *ani_path_arrow(ani_arena *a, an_vec2 from, an_vec2 to, float head);

// Point segments for a circle of this radius at tolerance tol (deterministic).
int ani_circle_segments(float radius, float tol);

// Position and unit tangent at global arc length s (clamped to [0, total]),
// local coordinates. Also returns the subpath index and arc length within it.
void ani_path_at(const ani_path *p, double s, an_vec2 *pos, an_vec2 *tangent,
                 int *sub, double *s_in_sub);

#endif
