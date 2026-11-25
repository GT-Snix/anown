// Internal: turn a scene into one triangle list (scene pixels, per-vertex color),
// in draw order. Pure CPU code with no GPU calls; shared by preview and export.
#ifndef ANOWN_TESS_INTERNAL_H
#define ANOWN_TESS_INTERNAL_H

#include "scene_internal.h"
#include "arena_internal.h"

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
    // Stencil layout (src/render.c, docs/text.md): bit 0x80 = the current object's
    // silhouette (its shadow is cut where it's set); bits 0x70 = write-once marks
    // for translucent paints in layers; bits 0x0F = winding counter (glyph fills).
    ANI_PAINT_DIRECT,        // no stencil (opaque paints)
    ANI_PAINT_SHADOW,        // translucent shadow in a layer: outside the silhouette, write-once
    ANI_PAINT_FILL,          // layers: translucent fill, write-once (mark 0x20)
    ANI_PAINT_INK,           // layers: translucent ink, write-once (mark 0x30)
    ANI_PAINT_STENCIL_CLEAR, // layers: reset all stencil bits under an object; no color
    ANI_PAINT_BACKGROUND,    // main: the scene background (flat quad, or the paper shader)
    ANI_PAINT_SILHOUETTE,    // set the silhouette bit (fill + shifted fill + ink); no color
    ANI_PAINT_SILHOUETTE_CLEAR, // clear the silhouette bit over the same geometry; no color
    ANI_PAINT_SHADOW_CUT,    // opaque shadow: only where the silhouette bit is clear (main or layer)
    // Stencil-then-cover fills (glyphs, docs/text.md): a WIND pass draws each
    // contour as a fan from its first point, counting the winding number in bits
    // 0x0F (front faces +1, back faces -1, mod 16), color off; then a COVER pass
    // draws one quad per glyph that paints where the winding is nonzero and zeroes
    // it, so every sample is painted at most once (write-once by construction).
    ANI_PAINT_WIND,              // count winding everywhere
    ANI_PAINT_WIND_SIL,          // the same, for a silhouette (skipped where silhouettes are)
    ANI_PAINT_WIND_SHADOW_CUT,   // count only outside the silhouette (opaque shadow)
    ANI_PAINT_WIND_SHADOW,       // layers: outside the silhouette and not yet shadowed
    ANI_PAINT_COVER,             // paint where the winding is nonzero; zero it
    ANI_PAINT_COVER_SHADOW,      // layers: the same, and set the shadow mark (translucent shadow)
    ANI_PAINT_COVER_SIL,         // set the silhouette bit where the winding is nonzero; zero it; no color
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
    // letters waiting to be emitted as one batch (tess.c), reused across frames
    void *glyphs;
    int glyph_count, glyph_cap;
    // Per-object tessellation cache (tess.c, docs/layers.md "Memoized
    // tessellation"): an object's (or a text batch's) vertices and commands,
    // keyed by everything they depend on. Never changes the output.
    // the camera this frame (docs/camera.md). A pure translation isn't put into
    // the world transforms: it's this per-draw offset (scene px), so cache keys
    // don't change while the camera pans or shakes.
    float cam_dx, cam_dy;
    bool cam_fast_off;      // benchmarks: always put the camera in the world transform
    // the largest scale on screen of any drawn object this frame (largest
    // singular value of its world transform), for the max_zoom warning
    float max_screen_scale;
    int snapped_letters;    // letters snapped to whole pixels this frame (tests)
    // per-frame scratch for geometry resolved while tessellating (edges); reset
    // every frame, never part of any object
    ani_arena frame_arena;
    struct ani_memo *memo;
    bool memo_off;          // tessellate everything every frame (tests, benchmarks)
    size_t memo_budget;     // bytes of cached vertices and commands; 0: ANI_MEMO_BUDGET
    int memo_hits, memo_misses; // the last frame
} ani_mesh;

#define ANI_MEMO_BUDGET ((size_t)128 * 1024 * 1024)

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
// The verb transform (docs/camera.md) applied inside a local transform, about
// its pivot. Exactly `local` when the verb is the identity.
ani_affine ani_verb_local(ani_affine local, an_vec2 vscale, float vrot, an_vec2 pivot);
// Local transform of an object's BASE state.
ani_affine ani_obj_local(const an_obj *o);
ani_affine ani_affine_mul(ani_affine parent, ani_affine child);
an_vec2 ani_affine_apply(ani_affine m, an_vec2 p);
// World transform of the BASE state: product of base locals from the top down.
ani_affine ani_obj_world(const an_obj *o);

// Boil bucket spacing (local units) for an object at a time: the style's
// spacing, coarsened by powers of two below 6 px on screen (docs/camera.md).
typedef struct ani_time ani_time;
float ani_boil_spacing(const an_scene *s, an_obj *c, ani_time at);

// graph.c: an edge's resolved geometry at a time, in its parent's space: start,
// end (each `gap` off its node's outline), the bend control point and the unit
// direction at the end. False if an endpoint doesn't exist yet.
bool ani_edge_resolve(const an_obj *e, ani_time at, an_vec2 *p0, an_vec2 *p1, an_vec2 *ctrl, an_vec2 *dir);

// Append one triangle. Skips triangles with near-zero area or non-finite
// coordinates (so the mesh never contains either). False on allocation failure.
bool ani_mesh_tri(ani_mesh *m, an_vec2 p0, an_vec2 p1, an_vec2 p2, an_color c);

#endif
