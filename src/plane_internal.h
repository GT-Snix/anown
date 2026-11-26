// Internal: the plane's mapping (docs/plane.md). Everything that maps goes
// through ani_map so a projective kind can be added later.
#ifndef ANOWN_PLANE_INTERNAL_H
#define ANOWN_PLANE_INTERNAL_H

#include "anown.h"
#include "timeline_internal.h"

enum { ANI_MAP_LINEAR2 = 0 /* later: ANI_MAP_PROJ3 */ };
typedef struct ani_map {
    int kind;
    float m[9]; // LINEAR2: a, b, c, d
    float unit; // px per plane unit (y flipped: plane y is up)
} ani_map;

#define ANI_PLANE_HEAD 18.0f     // arrowhead length, px
#define ANI_PLANE_MAX_LINES 400  // grid lines per frame, at most (every k-th past that)

extern bool ani_plane_map_off;   // tests only: map without the matrix (the identity gate)

an_vec2 ani_map_point(const ani_map *m, an_vec2 p);
an_vec2 ani_map_tangent(const ani_map *m, an_vec2 p, an_vec2 d);
bool ani_map_inverse(const ani_map *m, an_vec2 q, an_vec2 *p);
ani_map ani_plane_map_at(const an_obj *plane, ani_time at);
bool ani_plane_mapped_kind(const an_obj *o);

typedef struct ani_grid_seg { int axis, k; an_vec2 a, b; } ani_grid_seg;
// Grid lines visible in the clip box [lo, hi] (plane-local px), mapped and clipped.
int ani_plane_grid(const an_obj *grid, const ani_map *m, an_vec2 lo, an_vec2 hi, ani_grid_seg *out, int cap);
bool ani_plane_line_seg(const an_obj *line, const ani_map *m, an_vec2 lo, an_vec2 hi, an_vec2 *a, an_vec2 *b);
// shared with the space (docs/space.md): Liang-Barsky, and the infinite 2D line through
// P along D clipped to [lo, hi] (a dot at P if |D| <= 1e-6 unit)
bool ani_clip_seg(an_vec2 *a, an_vec2 *b, an_vec2 lo, an_vec2 hi);
bool ani_line2_seg(an_vec2 P, an_vec2 D, float unit, an_vec2 lo, an_vec2 hi, an_vec2 *a, an_vec2 *b);
void ani_plane_vector_geom(an_obj *vec, const ani_map *m, ani_time at, an_vec2 *tip, an_vec2 *dir, float *head);

#endif
