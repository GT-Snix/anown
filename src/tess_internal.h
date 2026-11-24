// Internal: turn a scene into one triangle list (scene pixels, per-vertex color),
// in draw order. Pure CPU code with no GPU calls; shared by preview and export.
#ifndef ANOWN_TESS_INTERNAL_H
#define ANOWN_TESS_INTERNAL_H

#include "scene_internal.h"

typedef struct ani_vertex {
    float x, y;       // scene pixels
    float r, g, b, a; // color
} ani_vertex;

// 2D affine transform: x' = a*x + c*y + tx, y' = b*x + d*y + ty
typedef struct ani_affine {
    float a, b, c, d, tx, ty;
} ani_affine;

typedef struct ani_mesh {
    ani_vertex *v;
    int count, cap;
    // scratch for sorting siblings during traversal
    an_obj **scratch;
    int scratch_count, scratch_cap;
} ani_mesh;

void ani_mesh_init(ani_mesh *m);
void ani_mesh_free(ani_mesh *m);

// Rebuild m from the scene: a background quad, then every object, depth-first,
// siblings ordered by (z, creation id). Returns false on allocation failure.
bool ani_tess_scene(ani_mesh *m, const an_scene *s);

// Local transform of one object (translate * rotate * scale).
ani_affine ani_obj_local(const an_obj *o);
ani_affine ani_affine_mul(ani_affine parent, ani_affine child);
an_vec2 ani_affine_apply(ani_affine m, an_vec2 p);
// World transform: product of local transforms from the top level down.
ani_affine ani_obj_world(const an_obj *o);

// Circle segment count for a radius in pixels (deterministic).
int ani_circle_segments(float radius_px);

#endif
