// Internal: turn a scene into one triangle list (scene pixels, per-vertex color),
// in draw order. Pure CPU code with no GPU calls; shared by preview and export.
#ifndef ANOWN_TESS_INTERNAL_H
#define ANOWN_TESS_INTERNAL_H

#include "scene_internal.h"

typedef struct ani_vertex {
    float x, y;       // scene pixels
    float r, g, b, a; // color
    float pu, pv;     // fill pattern coordinates (cell units, object space); 0 otherwise
    float pat;        // pattern kind + density * 0.999 (0 = solid)
} ani_vertex;

// 2D affine transform: x' = a*x + c*y + tx, y' = b*x + d*y + ty
typedef struct ani_affine {
    float a, b, c, d, tx, ty;
} ani_affine;

typedef struct ani_draw_item {
    an_obj *obj;
    int z;
} ani_draw_item;

// ---- command lists (docs/layers.md) ------------------------------------------------
// Vertices go into one array in traversal order. Commands say which target
// (the main target, or an isolated layer) draws which vertex range, with which
// paint, and where layers are composited.

#define ANI_TARGET_MAIN (-1)

typedef enum ani_paint {
    // Stencil layout: bit 0x80 = the current object's silhouette (its shadow is cut
    // where it's set); bits 0x7F = write-once marks for translucent paints in layers.
    ANI_PAINT_DIRECT,        // no stencil (opaque paints)
    ANI_PAINT_SHADOW,        // translucent shadow in a layer: outside the silhouette, write-once
    ANI_PAINT_FILL,          // layers: translucent fill, write-once (ref 2 in the low bits)
    ANI_PAINT_INK,           // layers: translucent ink, write-once (ref 3 in the low bits)
    ANI_PAINT_STENCIL_CLEAR, // layers: reset all stencil bits under an object; no color
    ANI_PAINT_BACKGROUND,    // main: the scene background (flat quad, or the paper shader)
    ANI_PAINT_SILHOUETTE,    // set the silhouette bit (fill + shifted fill + ink); no color
    ANI_PAINT_SILHOUETTE_CLEAR, // clear the silhouette bit over the same geometry; no color
    ANI_PAINT_SHADOW_CUT,    // opaque shadow: only where the silhouette bit is clear (main or layer)
    ANI_PAINT_COUNT
} ani_paint;

typedef enum ani_cmd_kind { ANI_CMD_TRIS, ANI_CMD_COMPOSITE } ani_cmd_kind;

typedef struct ani_cmd {
    ani_cmd_kind kind;
    int target;       // ANI_TARGET_MAIN or a layer index
    ani_paint paint;  // TRIS
    int first, count; // TRIS: vertex range
    int layer;        // COMPOSITE: the layer to composite into `target`
} ani_cmd;

typedef struct ani_layer {
    int parent;     // ANI_TARGET_MAIN or the enclosing layer
    float opacity;  // applied when compositing
    an_obj *obj;    // the object or group that owns it (for tests/debugging)
} ani_layer;

// Reusable point scratch for the stroke triangulator (no malloc per stroke).
typedef struct ani_pts {
    an_vec2 *q;
    float *h, *s;
    int n, cap;
} ani_pts;

typedef struct ani_mesh {
    ani_vertex *v;
    int count, cap;
    ani_cmd *cmds;
    int ncmds, cmd_cap;
    ani_layer *layers;
    int nlayers, layer_cap;
    // per-layer flag during tessellation: has a translucent (stencil) paint
    // already written stencil marks into it?
    bool *layer_used;
    int layer_used_cap;
    // scratch for sorting siblings during traversal (object + its z at the frame)
    struct ani_draw_item *scratch;
    int scratch_count, scratch_cap;
    // testing: isolate every object and group, even at opacity 1
    bool force_isolate;
    // tessellation-internal: the next cmd_begin must not extend the previous run
    bool merge_barrier;
    // stroke triangulator scratch, reused across strokes and frames
    ani_pts pts_a, pts_b;
} ani_mesh;

void ani_mesh_init(ani_mesh *m);
void ani_mesh_free(ani_mesh *m);

// Rebuild m from the scene sampled at `frame`: background quad, then every
// object that exists, depth-first, siblings ordered by (z at the frame, creation
// id). Objects and groups at opacity < 1, and objects with translucent paint
// colors (outside a layer), are isolated into layers. Pure function of
// (scene, frame, m->force_isolate). Returns false on allocation failure.
bool ani_tess_scene(ani_mesh *m, const an_scene *s, int64_t frame);

// An upper-ish estimate of the most vertices (and commands) any frame of the
// scene will need, for reserving capacity up front (no doubling mid-sequence).
size_t ani_estimate_vertices(const an_scene *s, size_t *cmds);
bool ani_mesh_reserve(ani_mesh *m, int verts, int cmds);

// Local transform from a position, scale and rotation (translate * rotate * scale).
ani_affine ani_local(an_vec2 pos, an_vec2 scale, float rotation);
// Local transform of an object's BASE state.
ani_affine ani_obj_local(const an_obj *o);
ani_affine ani_affine_mul(ani_affine parent, ani_affine child);
an_vec2 ani_affine_apply(ani_affine m, an_vec2 p);
// World transform of the BASE state: product of base locals from the top down.
ani_affine ani_obj_world(const an_obj *o);

// Append one triangle. Skips triangles with near-zero area or non-finite
// coordinates (so the mesh never contains either). False on allocation failure.
bool ani_mesh_tri(ani_mesh *m, an_vec2 p0, an_vec2 p1, an_vec2 p2, an_color c);

#endif
