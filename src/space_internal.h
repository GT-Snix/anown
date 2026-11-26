// Internal: the 3D space's view (docs/space.md). Projection and near-plane
// cutting, in double; only final local px are floats.
#ifndef ANOWN_SPACE_INTERNAL_H
#define ANOWN_SPACE_INTERNAL_H

#include "anown.h"
#include "timeline_internal.h"

// The composed map at one frame: camera coordinates of a space point p are
// C p + o (x right, y up, z = depth - D), with C = [r; u; f] M.
typedef struct ani_view {
    double C[9], o[3];
    double D;      // the camera's distance
    double unit;   // px per unit at the target's depth
    double near;   // the near plane: depth >= near (0.05 D)
    bool ortho;
} ani_view;

typedef struct ani_cam3 { double x, y, depth; } ani_cam3;

ani_view ani_space_view_at(const an_obj *space, ani_time at);
ani_cam3 ani_view_cam(const ani_view *v, const double p[3]);
bool ani_view_visible(const ani_view *v, ani_cam3 c);
// local px of a camera point (in front of the near plane, or ortho); -0 becomes +0
an_vec2 ani_view_px(const ani_view *v, ani_cam3 c);
// cut a segment to depth >= near; false if nothing is left
bool ani_view_cut(const ani_view *v, ani_cam3 *a, ani_cam3 *b);

bool ani_space_mapped_kind(const an_obj *o);
// a space child's 3D position at `at`: (pos.x, pos.y, pos_z)
void ani_space_pos3(an_obj *o, ani_time at, double p[3]);

typedef struct ani_grid_seg ani_grid_seg; // plane_internal.h
int ani_space_grid(const an_obj *grid, const ani_view *v, an_vec2 lo, an_vec2 hi, ani_grid_seg *out, int cap);
bool ani_space_line_seg(const an_obj *line, const ani_view *v, an_vec2 lo, an_vec2 hi, an_vec2 *a, an_vec2 *b);
// a vector: false if nothing of it is in front of the camera. tail/tip in local px;
// has_tip: the tip itself is visible (the head is drawn)
bool ani_space_vector_geom(an_obj *vec, const ani_view *v, ani_time at, an_vec2 *tail, an_vec2 *tip, an_vec2 *dir,
                           float *head, bool *has_tip);
// a path's visible pieces as move/line runs: up to cap points; starts[k] = 1 begins a
// subpath. Returns the number of points; *closed: one unbroken closed loop
int ani_space_path_px(const an_obj *path, const ani_view *v, an_vec2 *out, unsigned char *starts, int cap, bool *closed);

#endif
