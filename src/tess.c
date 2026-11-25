#include "tess_internal.h"
#include "stroke_internal.h"
#include "timeline_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>


void ani_mesh_init(ani_mesh *m) {
    *m = (ani_mesh){ 0 };
}

void ani_mesh_free(ani_mesh *m) {
    free(m->v);
    free(m->cmds);
    free(m->layers);
    free(m->layer_used);
    free(m->scratch);
    free(m->pts_a.q);
    free(m->pts_a.h);
    free(m->pts_a.s);
    free(m->pts_b.q);
    free(m->pts_b.h);
    free(m->pts_b.s);
    *m = (ani_mesh){ 0 };
}

static bool grow(void **p, int *cap, int need, size_t elem) {
    if (need <= *cap) return true;
    int n = *cap ? *cap : 256;
    while (n < need) {
        if (n > (1 << 28)) return false;
        n *= 2;
    }
    void *q = realloc(*p, (size_t)n * elem);
    if (!q) return false;
    *p = q;
    *cap = n;
    return true;
}

// ---- transforms ---------------------------------------------------------------

ani_affine ani_local(an_vec2 pos, an_vec2 scale, float rotation) {
    const float cs = cosf(rotation), sn = sinf(rotation);
    return (ani_affine){
        .a = cs * scale.x, .b = sn * scale.x,
        .c = -sn * scale.y, .d = cs * scale.y,
        .tx = pos.x, .ty = pos.y,
    };
}

ani_affine ani_obj_local(const an_obj *o) {
    return ani_local(o->pos, o->scale, o->rotation);
}

ani_affine ani_affine_mul(ani_affine p, ani_affine q) {
    return (ani_affine){
        .a = p.a * q.a + p.c * q.b,
        .b = p.b * q.a + p.d * q.b,
        .c = p.a * q.c + p.c * q.d,
        .d = p.b * q.c + p.d * q.d,
        .tx = p.a * q.tx + p.c * q.ty + p.tx,
        .ty = p.b * q.tx + p.d * q.ty + p.ty,
    };
}

an_vec2 ani_affine_apply(ani_affine m, an_vec2 p) {
    return (an_vec2){ m.a * p.x + m.c * p.y + m.tx, m.b * p.x + m.d * p.y + m.ty };
}

ani_affine ani_obj_world(const an_obj *o) {
    // the root has the identity transform; it isn't part of the product
    if (!o->parent) return (ani_affine){ 1, 0, 0, 1, 0, 0 };
    return ani_affine_mul(ani_obj_world(o->parent), ani_obj_local(o));
}

// ---- emitting -------------------------------------------------------------------

// Smaller than this (px^2) a triangle covers nothing: skipped. So are any with
// non-finite coordinates. Every triangle in the mesh comes through here.
#define ANI_MIN_TRI_AREA 1e-6f

bool ani_mesh_tri(ani_mesh *m, an_vec2 p0, an_vec2 p1, an_vec2 p2, an_color c) {
    const float area2 = (p1.x - p0.x) * (p2.y - p0.y) - (p1.y - p0.y) * (p2.x - p0.x);
    if (!(fabsf(area2) >= 2.0f * ANI_MIN_TRI_AREA)) return true; // also catches NaN
    if (!isfinite(p0.x) || !isfinite(p0.y) || !isfinite(p1.x) || !isfinite(p1.y) ||
        !isfinite(p2.x) || !isfinite(p2.y))
        return true;
    if (!grow((void **)&m->v, &m->cap, m->count + 3, sizeof(ani_vertex))) return false;
    const an_vec2 ps[3] = { p0, p1, p2 };
    for (int i = 0; i < 3; i++) {
        m->v[m->count++] = (ani_vertex){ .x = ps[i].x, .y = ps[i].y, .r = c.r, .g = c.g, .b = c.b, .a = c.a };
    }
    return true;
}

static bool quad(ani_mesh *m, an_vec2 p0, an_vec2 p1, an_vec2 p2, an_vec2 p3, an_color c) {
    return ani_mesh_tri(m, p0, p1, p2, c) && ani_mesh_tri(m, p0, p2, p3, c);
}

// ---- commands ---------------------------------------------------------------------

// Open a triangle run for (target, paint) at the current vertex count. Extends
// the previous command when it's the same target and paint and contiguous.
static int cmd_begin(ani_mesh *m, int target, ani_paint paint) {
    const bool barrier = m->merge_barrier;
    m->merge_barrier = false;
    if (m->ncmds > 0 && !barrier) {
        ani_cmd *last = &m->cmds[m->ncmds - 1];
        if (last->kind == ANI_CMD_TRIS && last->target == target && last->paint == paint &&
            last->first + last->count == m->count)
            return m->ncmds - 1;
    }
    if (!grow((void **)&m->cmds, &m->cmd_cap, m->ncmds + 1, sizeof(ani_cmd))) return -1;
    m->cmds[m->ncmds] = (ani_cmd){ .kind = ANI_CMD_TRIS, .target = target, .paint = paint, .first = m->count };
    return m->ncmds++;
}

// Close a run: its count is everything emitted since it began. Empty runs go.
static void cmd_end(ani_mesh *m, int idx) {
    ani_cmd *c = &m->cmds[idx];
    c->count = m->count - c->first;
    if (c->count == 0 && idx == m->ncmds - 1) m->ncmds--;
}

static int layer_new(ani_mesh *m, int parent, float opacity, an_obj *obj) {
    if (!grow((void **)&m->layers, &m->layer_cap, m->nlayers + 1, sizeof(ani_layer))) return -1;
    if (!grow((void **)&m->layer_used, &m->layer_used_cap, m->nlayers + 1, sizeof(bool))) return -1;
    m->layers[m->nlayers] = (ani_layer){ .parent = parent, .opacity = opacity, .obj = obj };
    m->layer_used[m->nlayers] = false;
    // the composite goes into the parent's command stream at this point in draw order
    if (!grow((void **)&m->cmds, &m->cmd_cap, m->ncmds + 1, sizeof(ani_cmd))) return -1;
    m->cmds[m->ncmds++] = (ani_cmd){ .kind = ANI_CMD_COMPOSITE, .target = parent, .layer = m->nlayers };
    return m->nlayers++;
}

// Reset the stencil to 0 over the bounding box of vertices [v0, m->count)
// (one object's paints), and place that command at index `pos`, i.e. before the
// object's paint commands. Only an object's own area is touched, so the cost is
// proportional to its size, not the layer's.
static bool stencil_clear_before(ani_mesh *m, int target, int v0, int pos) {
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (int i = v0; i < m->count; i++) {
        x0 = fminf(x0, m->v[i].x);
        y0 = fminf(y0, m->v[i].y);
        x1 = fmaxf(x1, m->v[i].x);
        y1 = fmaxf(y1, m->v[i].y);
    }
    if (!(x1 >= x0)) return true;
    x0 -= 1;
    y0 -= 1;
    x1 += 1;
    y1 += 1;
    if (!grow((void **)&m->cmds, &m->cmd_cap, m->ncmds + 1, sizeof(ani_cmd))) return false;
    const int first = m->count;
    if (!quad(m, (an_vec2){ x0, y0 }, (an_vec2){ x1, y0 }, (an_vec2){ x1, y1 }, (an_vec2){ x0, y1 },
              (an_color){ 0, 0, 0, 0 }))
        return false;
    memmove(&m->cmds[pos + 1], &m->cmds[pos], sizeof(ani_cmd) * (size_t)(m->ncmds - pos));
    m->cmds[pos] = (ani_cmd){ .kind = ANI_CMD_TRIS, .target = target, .paint = ANI_PAINT_STENCIL_CLEAR,
                              .first = first, .count = m->count - first };
    m->ncmds++;
    return true;
}

// ---- emitting objects -------------------------------------------------------------

// Sampled per-frame style of one object.
typedef struct emit_style {
    an_color fill, stroke; // paint colors (their own alpha; opacity is applied at composite)
    float stroke_width;    // local units
    float draw;            // draw progress
    an_vec2 riso;          // fill offset, scene px (style offset x effective chaos), or 0
    bool shadow;           // draw the hard shadow
    an_vec2 shadow_offset; // scene px (style offset x effective chaos)
    an_color shadow_color;
    float chaos;           // effective chaos of the object
    ani_boil boil;         // line boil for this object at this frame (amp 0 = off)
    bool paper_under;      // fill the silhouette with `paper` before the object's paints
    an_color paper;        // the scene background color
} emit_style;

static an_vec2 add(an_vec2 a, an_vec2 b) { return (an_vec2){ a.x + b.x, a.y + b.y }; }

// Fill of a closed convex shape: the first subpath's points, so the fill and the
// outline share one polygon. Rects are a single quad. `off` is a scene-space
// translation applied after the world transform (riso, shadow): it doesn't
// rotate or scale with the object.
// Give fill vertices [v0, m->count) their pattern coordinates: object space (the
// inverse world transform of the final position, minus the fill's scene-space
// offset), rotated by -angle and divided by the cell size. Affine in object space,
// so interpolation across each triangle is exact, and the pattern stays attached
// to the object under boil, riso, motion, rotation and scale.
static void set_pattern(ani_mesh *m, int v0, const an_pattern *pt, ani_affine w, an_vec2 off) {
    if (pt->kind == AN_PATTERN_SOLID) return;
    const float det = w.a * w.d - w.b * w.c;
    if (!(fabsf(det) > 0.0f)) return;
    const float cs = cosf(pt->angle), sn = sinf(pt->angle);
    const float inv = 1.0f / pt->size;
    const float d = pt->density >= 1.0f ? 0.999f : pt->density * 0.999f;
    const float pat = (float)pt->kind + d;
    for (int i = v0; i < m->count; i++) {
        const float px = m->v[i].x - off.x - w.tx, py = m->v[i].y - off.y - w.ty;
        const float lx = (w.d * px - w.c * py) / det, ly = (-w.b * px + w.a * py) / det;
        m->v[i].pu = (cs * lx + sn * ly) * inv;
        m->v[i].pv = (-sn * lx + cs * ly) * inv;
        m->v[i].pat = pat;
    }
}

static bool emit_fill(ani_mesh *m, const an_obj *o, ani_affine w, an_vec2 off, an_color fill, const ani_boil *boil) {
    const ani_path *p = o->path;
    if (!p || p->nsubs == 0 || p->subs[0].count < 3) return true;
    const ani_subpath *sp = &p->subs[0];
    if (boil && boil->amp > 0.0f) {
        // the same ring the outline boils along, so fill and ink wobble together
        const an_vec2 *ring; // lives in the mesh's scratch until the next stroke call
        const int n = ani_boil_ring(m, p, w, boil, &ring);
        if (n < 0) return false;
        const an_vec2 center = add(ani_affine_apply(w, (an_vec2){ 0, 0 }), off);
        bool ok = true;
        for (int i = 0; ok && i < n; i++) {
            ok = ani_mesh_tri(m, center, add(ring[(i + n - 1) % n], off), add(ring[i], off), fill);
        }
        return ok;
    }
    if (o->kind == AN_KIND_RECT && sp->count == 4) {
        return quad(m, add(ani_affine_apply(w, p->pts[sp->first]), off), add(ani_affine_apply(w, p->pts[sp->first + 1]), off),
                    add(ani_affine_apply(w, p->pts[sp->first + 2]), off), add(ani_affine_apply(w, p->pts[sp->first + 3]), off), fill);
    }
    const an_vec2 center = add(ani_affine_apply(w, (an_vec2){ 0, 0 }), off);
    an_vec2 prev = add(ani_affine_apply(w, p->pts[sp->first + sp->count - 1]), off);
    for (int i = 0; i < sp->count; i++) {
        const an_vec2 cur = add(ani_affine_apply(w, p->pts[sp->first + i]), off);
        if (!ani_mesh_tri(m, center, prev, cur, fill)) return false;
        prev = cur;
    }
    return true;
}

// The ink stroke, translated by a scene-space offset (0 for the ink itself).
static bool emit_ink(ani_mesh *m, const an_obj *o, ani_affine w, an_vec2 off, const emit_style *st, an_color color) {
    w.tx += off.x;
    w.ty += off.y;
    const ani_stroke_in in = {
        .path = o->path,
        .world = w,
        .width = st->stroke_width,
        .progress = st->draw,
        .color = color,
        .brush = &o->brush,
        .boil = st->boil,
    };
    return ani_stroke_emit(m, &in, NULL);
}

static bool has_fill(const an_obj *o, const emit_style *st) {
    return (o->kind == AN_KIND_RECT || o->kind == AN_KIND_CIRCLE) && !o->no_fill && st->fill.a > 0.0f;
}

static bool has_ink(const an_obj *o, const emit_style *st) {
    return o->path && o->has_stroke && st->stroke.a > 0.0f;
}

// Draw one object's paints into `target`: shadow, fill, ink. Opaque paints never
// need write-once coverage (overlapping triangles of one opaque color write the
// same value), so they use the plain pipeline even in a layer; only translucent
// paints in a layer are stencil-tested. The shadow is ONE paint: the union of the
// fill and ink silhouettes, so in a layer it covers each sample once.
static bool emit(ani_mesh *m, const an_obj *o, ani_affine w, const emit_style *st, int target) {
    const bool fill = has_fill(o, st), ink = has_ink(o, st);
    if (!fill && !ink) return true;
    const bool shadow = st->shadow && st->shadow_color.a > 0.0f;
    const bool in_layer = target != ANI_TARGET_MAIN;
    const bool shadow_st = in_layer && shadow && st->shadow_color.a < 1.0f;
    const bool fill_st = in_layer && fill && st->fill.a < 1.0f;
    const bool ink_st = in_layer && ink && st->stroke.a < 1.0f;
    const bool stencil = shadow_st || fill_st || ink_st;
    const int cmd_pos = m->ncmds, v0 = m->count;
    // its runs must start fresh: a stencil clear or the silhouette goes before them
    if (stencil || shadow || st->paper_under) m->merge_barrier = true;
    // The shadow clears the ink: its offset is measured from the ink's outer
    // edge, so each axis of the style offset gets the ink width added (all x chaos).
    an_vec2 soff = st->shadow_offset;
    if (shadow && ink) {
        const float wpx = st->stroke_width * sqrtf(fabsf(w.a * w.d - w.b * w.c)) * st->chaos;
        soff.x += st->shadow_offset.x > 0 ? wpx : (st->shadow_offset.x < 0 ? -wpx : 0.0f);
        soff.y += st->shadow_offset.y > 0 ? wpx : (st->shadow_offset.y < 0 ? -wpx : 0.0f);
    }
    const ani_paint shadow_paint = shadow_st ? ANI_PAINT_SHADOW : ANI_PAINT_SHADOW_CUT;
    if (shadow && fill) {
        const int c = cmd_begin(m, target, shadow_paint);
        if (c < 0 || !emit_fill(m, o, w, add(st->riso, soff), st->shadow_color, &st->boil)) return false;
        cmd_end(m, c);
    }
    // fill and ink start fresh runs, so the shadow's ink copy can go right before them
    if (shadow) m->merge_barrier = true;
    const int paints_pos = m->ncmds;
    int ink_v0 = 0, ink_v1 = 0;
    if (fill) {
        const int c = cmd_begin(m, target, fill_st ? ANI_PAINT_FILL : ANI_PAINT_DIRECT);
        const int fv0 = m->count;
        if (c < 0 || !emit_fill(m, o, w, st->riso, st->fill, &st->boil)) return false;
        set_pattern(m, fv0, &o->pattern, w, st->riso);
        cmd_end(m, c);
    }
    if (ink) {
        const int c = cmd_begin(m, target, ink_st ? ANI_PAINT_INK : ANI_PAINT_DIRECT);
        ink_v0 = m->count;
        if (c < 0 || !emit_ink(m, o, w, (an_vec2){ 0, 0 }, st, st->stroke)) return false;
        ink_v1 = m->count;
        cmd_end(m, c);
    }
    // The shadow of the ink: the ink's triangles, built once above, drawn again
    // translated and in the shadow color, in a command placed before the fill.
    if (shadow && ink && ink_v1 > ink_v0) {
        const int n = ink_v1 - ink_v0;
        if (!grow((void **)&m->v, &m->cap, m->count + n, sizeof(ani_vertex))) return false;
        if (!grow((void **)&m->cmds, &m->cmd_cap, m->ncmds + 1, sizeof(ani_cmd))) return false;
        const int first = m->count;
        const an_color sc = st->shadow_color;
        for (int i = 0; i < n; i++) {
            const ani_vertex v = m->v[ink_v0 + i];
            m->v[m->count++] = (ani_vertex){ .x = v.x + soff.x, .y = v.y + soff.y, .r = sc.r, .g = sc.g, .b = sc.b, .a = sc.a };
        }
        memmove(&m->cmds[paints_pos + 1], &m->cmds[paints_pos], sizeof(ani_cmd) * (size_t)(m->ncmds - paints_pos));
        m->cmds[paints_pos] = (ani_cmd){ .kind = ANI_CMD_TRIS, .target = target, .paint = shadow_paint,
                                         .first = first, .count = n };
        m->ncmds++;
        m->merge_barrier = true; // nothing may extend a run that's out of vertex order
    }
    // The silhouette: the fill where it's registered AND where riso moved it, plus
    // the ink.
    // - The shadow cut: it sets a stencil bit before the shadow commands and clears
    //   it after, so the shadow never shows where the object is (per MSAA sample).
    //   A halftone gap or the riso sliver shows what's under the object instead.
    // - paper_under: the same geometry drawn in the background color right before
    //   the object's own paints, so those gaps show paper even over other objects.
    // The vertices are colored paper; the stencil passes ignore color.
    const int shadow_end = paints_pos + (shadow && ink && ink_v1 > ink_v0 ? 1 : 0);
    const bool cut = shadow && shadow_end > cmd_pos;
    if (cut || st->paper_under) {
        const int sv0 = m->count;
        if (fill) {
            if (!emit_fill(m, o, w, (an_vec2){ 0, 0 }, st->fill, &st->boil)) return false;
            if ((st->riso.x != 0.0f || st->riso.y != 0.0f) && !emit_fill(m, o, w, st->riso, st->fill, &st->boil))
                return false;
        }
        if (ink_v1 > ink_v0) {
            const int n = ink_v1 - ink_v0;
            if (!grow((void **)&m->v, &m->cap, m->count + n, sizeof(ani_vertex))) return false;
            memcpy(&m->v[m->count], &m->v[ink_v0], sizeof(ani_vertex) * (size_t)n);
            m->count += n;
        }
        for (int i = sv0; i < m->count; i++) {
            m->v[i].r = st->paper.r;
            m->v[i].g = st->paper.g;
            m->v[i].b = st->paper.b;
            m->v[i].a = 1.0f;
            m->v[i].pat = 0.0f;
        }
        const ani_cmd sil = { .kind = ANI_CMD_TRIS, .target = target, .paint = ANI_PAINT_SILHOUETTE,
                              .first = sv0, .count = m->count - sv0 };
        ani_cmd clr = sil, paper = sil;
        clr.paint = ANI_PAINT_SILHOUETTE_CLEAR;
        paper.paint = ANI_PAINT_DIRECT;
        if (!grow((void **)&m->cmds, &m->cmd_cap, m->ncmds + 3, sizeof(ani_cmd))) return false;
        // order: [silhouette set] shadows [silhouette clear] [paper] fill ink
        int at = shadow_end; // where the object's own paints begin
        if (cut) {
            memmove(&m->cmds[at + 1], &m->cmds[at], sizeof(ani_cmd) * (size_t)(m->ncmds - at));
            m->cmds[at++] = clr;
            m->ncmds++;
        }
        if (st->paper_under) {
            memmove(&m->cmds[at + 1], &m->cmds[at], sizeof(ani_cmd) * (size_t)(m->ncmds - at));
            m->cmds[at] = paper;
            m->ncmds++;
        }
        if (cut) {
            memmove(&m->cmds[cmd_pos + 1], &m->cmds[cmd_pos], sizeof(ani_cmd) * (size_t)(m->ncmds - cmd_pos));
            m->cmds[cmd_pos] = sil;
            m->ncmds++;
        }
        m->merge_barrier = true;
    }
    if (stencil) {
        // an earlier translucent object in this layer left stencil marks: reset them
        // under this object first, or its paints would be blocked there
        if (m->layer_used[target] && !stencil_clear_before(m, target, v0, cmd_pos)) return false;
        m->layer_used[target] = true; // the layer's stencil is now dirty
        m->merge_barrier = true;      // and the next object must not extend these runs
    }
    return true;
}

// Sibling order: z at the frame ascending, then creation id. Ids are unique, so
// this is a total order and qsort's instability doesn't matter.
static int cmp_draw(const void *pa, const void *pb) {
    const ani_draw_item *a = pa, *b = pb;
    if (a->z != b->z) return a->z < b->z ? -1 : 1;
    return a->obj->id < b->obj->id ? -1 : (a->obj->id > b->obj->id);
}

static an_color to_col(ani_value c) {
    return (an_color){ (float)c.v[0], (float)c.v[1], (float)c.v[2], (float)c.v[3] };
}

// pchaos: effective chaos of the parent (scene dial x multipliers down the tree)
static bool visit_children(ani_mesh *m, const an_scene *s, const an_obj *parent, ani_affine pw, int target,
                           float pchaos, ani_time at) {
    // push this node's live children (with their z at the frame), sort that slice
    const int base = m->scratch_count;
    for (an_obj *c = parent->first_child; c; c = c->next_sibling) {
        if (!ani_obj_exists(c, at)) continue; // not created yet: skip the subtree
        if (!grow((void **)&m->scratch, &m->scratch_cap, m->scratch_count + 1, sizeof(ani_draw_item)))
            return false;
        const int z = (int)ani_sample(ani_obj_prop(c, AN_PROP_Z), at).v[0];
        m->scratch[m->scratch_count++] = (ani_draw_item){ c, z };
    }
    const int n = m->scratch_count - base;
    qsort(m->scratch + base, (size_t)n, sizeof(ani_draw_item), cmp_draw);

    const an_style *sty = &s->style;
    for (int i = 0; i < n; i++) {
        an_obj *c = m->scratch[base + i].obj; // re-read: scratch may have moved
        const float opacity = (float)ani_sample(ani_obj_prop(c, AN_PROP_OPACITY), at).v[0];
        if (opacity <= 0.0f) continue; // invisible, and so are its children
        const float chaos = pchaos * (float)ani_sample(ani_obj_prop(c, AN_PROP_CHAOS), at).v[0];
        const ani_value pos = ani_sample(ani_obj_prop(c, AN_PROP_POS), at);
        const ani_value sc = ani_sample(ani_obj_prop(c, AN_PROP_SCALE), at);
        const ani_value rot = ani_sample(ani_obj_prop(c, AN_PROP_ROTATION), at);
        const ani_affine w = ani_affine_mul(
            pw, ani_local((an_vec2){ (float)pos.v[0], (float)pos.v[1] },
                          (an_vec2){ (float)sc.v[0], (float)sc.v[1] }, (float)rot.v[0]));
        emit_style st = { 0 };
        const bool drawable = c->kind != AN_KIND_GROUP && c->kind != AN_KIND_TEXT;
        if (drawable) {
            st = (emit_style){
                .fill = to_col(ani_sample(ani_obj_prop(c, AN_PROP_FILL), at)),
                .stroke = to_col(ani_sample(ani_obj_prop(c, AN_PROP_STROKE), at)),
                .stroke_width = (float)ani_sample(ani_obj_prop(c, AN_PROP_STROKE_WIDTH), at).v[0],
                .draw = (float)ani_sample(ani_obj_prop(c, AN_PROP_DRAW), at).v[0],
            };
            // riso: only objects with ink, unless forced on/off per object
            const bool riso = !sty->riso_off && c->riso != AN_OFF && (c->riso == AN_ON || c->has_stroke);
            if (riso) st.riso = (an_vec2){ sty->riso_offset.x * chaos, sty->riso_offset.y * chaos };
            // shadow: the scene default unless forced per object; no shadow at chaos 0
            const bool shadow = c->shadow == AN_ON || (c->shadow == AN_AUTO && sty->shadow);
            st.shadow = shadow && chaos > 0.0f;
            st.shadow_offset = (an_vec2){ sty->shadow_offset.x * chaos, sty->shadow_offset.y * chaos };
            st.shadow_color = sty->shadow_color;
            st.chaos = chaos;
            st.paper_under = c->paper_under;
            st.paper = s->background;
            // boil: keyed by the object's RNG key, arc-length buckets, and a 12 Hz step
            // in integer math: (N * 12) / fps, so 24, 30 and 60 fps all step at 12 Hz
            if (!sty->boil_off && chaos > 0.0f) {
                st.boil = (ani_boil){ .amp = sty->boil * chaos, .spacing = sty->boil_spacing,
                                      .seed = s->seed, .stream = c->rng_key,
                                      .step = (uint64_t)((at.num * 12) / at.den) };
            }
        }
        // Isolate when the node fades as a unit, or (outside any layer) when a
        // translucent paint needs stencil coverage. docs/layers.md
        const bool translucent = drawable && ((has_fill(c, &st) && st.fill.a < 1.0f) ||
                                              (has_ink(c, &st) && st.stroke.a < 1.0f) ||
                                              (st.shadow && st.shadow_color.a < 1.0f &&
                                               (has_fill(c, &st) || has_ink(c, &st))));
        const bool isolate = m->force_isolate || opacity < 1.0f || (translucent && target == ANI_TARGET_MAIN);
        int t = target;
        if (isolate) {
            t = layer_new(m, target, opacity, c);
            if (t < 0) return false;
        }
        if (drawable && !emit(m, c, w, &st, t)) return false;
        if (c->first_child && !visit_children(m, s, c, w, t, chaos, at)) return false;
    }
    m->scratch_count = base;
    return true;
}

bool ani_tess_scene(ani_mesh *m, const an_scene *s, int64_t frame) {
    m->count = 0;
    m->ncmds = 0;
    m->nlayers = 0;
    m->scratch_count = 0;
    m->merge_barrier = false;
    const float w = (float)s->width, h = (float)s->height;
    // the background is its own paint: drawn flat, or replaced by the paper shader
    const int c = cmd_begin(m, ANI_TARGET_MAIN, ANI_PAINT_BACKGROUND);
    if (c < 0 || !quad(m, (an_vec2){ 0, 0 }, (an_vec2){ w, 0 }, (an_vec2){ w, h }, (an_vec2){ 0, h },
                       s->background))
        return false;
    cmd_end(m, c);
    m->merge_barrier = true; // objects never extend the background run
    const ani_time at = ani_frame_time(s, frame);
    const float chaos = (float)ani_scene_chaos_at((an_scene *)s, at); // sampling only reads
    return visit_children(m, s, &s->root, (ani_affine){ 1, 0, 0, 1, 0, 0 }, ANI_TARGET_MAIN, chaos, at);
}

// ---- capacity estimate ---------------------------------------------------------------

static size_t estimate_obj(const an_obj *o, const an_style *sty, size_t *cmds) {
    size_t v = 0;
    *cmds += 9; // up to: stencil clear, silhouette set/clear, 2 shadows, paper, fill, ink, composite
    if (o->path) {
        const ani_path *p = o->path;
        // centerline points: the path's, plus taper profile samples (every 3 px)
        // and boil samples (every boil spacing)
        double pts = p->npts;
        pts += ((double)o->brush.taper_in + (double)o->brush.taper_out) / 3.0 * (p->nsubs ? p->nsubs : 1);
        if (o->brush.pressure_count > 1) pts += p->total / 3.0;
        if (!sty->boil_off && sty->boil > 0.0f && sty->boil_spacing > 0.0f) pts += p->total / sty->boil_spacing + p->nsubs;
        const double per_pt = 12.0; // segment quad + join fan, on average
        const bool shadow = o->shadow == AN_ON || (o->shadow == AN_AUTO && sty->shadow);
        // shadowed: the ink, its shadow copy and its silhouette copy; the fill, its
        // shadow and its silhouette at the registered and the riso position
        const bool sil = shadow || o->paper_under; // the silhouette: 2 fills + an ink copy
        v += (size_t)(pts * per_pt * (1.0 + (shadow ? 1.0 : 0.0) + (sil ? 1.0 : 0.0)));
        v += (size_t)p->npts * 3 * (size_t)(1 + (shadow ? 1 : 0) + (sil ? 2 : 0)); // fills
    }
    for (const an_obj *c = o->first_child; c; c = c->next_sibling) v += estimate_obj(c, sty, cmds);
    return v;
}

size_t ani_estimate_vertices(const an_scene *s, size_t *cmds) {
    *cmds = 64;
    return 1024 + estimate_obj(&s->root, &s->style, cmds);
}

bool ani_mesh_reserve(ani_mesh *m, int verts, int cmds) {
    return grow((void **)&m->v, &m->cap, verts, sizeof(ani_vertex)) &&
           grow((void **)&m->cmds, &m->cmd_cap, cmds, sizeof(ani_cmd));
}
