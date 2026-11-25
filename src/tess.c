#include "tess_internal.h"
#include "stroke_internal.h"
#include "text_internal.h"
#include "timeline_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>


static void memo_free(struct ani_memo *mm);

void ani_mesh_init(ani_mesh *m) {
    *m = (ani_mesh){ 0 };
    ani_arena_init(&m->frame_arena, 0);
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
    free(m->glyphs);
    memo_free(m->memo);
    ani_arena_release(&m->frame_arena);
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

ani_affine ani_verb_local(ani_affine local, an_vec2 vscale, float vrot, an_vec2 pivot) {
    if (vscale.x == 1.0f && vscale.y == 1.0f && vrot == 0.0f) return local; // exactly: no verb
    // local . translate(pivot) . rotate(vrot) . scale(vscale) . translate(-pivot)
    const ani_affine v = ani_affine_mul(ani_local(pivot, vscale, vrot), (ani_affine){ 1, 0, 0, 1, -pivot.x, -pivot.y });
    return ani_affine_mul(local, v);
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
    return (o->kind == AN_KIND_RECT || o->kind == AN_KIND_CIRCLE || o->kind == AN_KIND_GLYPH ||
            o->kind == AN_KIND_POLYGON) &&
           !o->no_fill && st->fill.a > 0.0f;
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

// Zoomed out, the bucket spacing on screen shrinks while the amplitude stays:
// below 6 px the wobble turns to fuzz. Coarsen by the smallest power of two that
// keeps it >= 6 px, from the object's scale at the START of the current boil
// step, so it only changes when the boil re-rolls anyway (docs/camera.md, (c)).
float ani_boil_spacing(const an_scene *s, an_obj *c, ani_time at) {
    const int64_t step = (at.num * 12) / at.den;
    const double sc = ani_obj_area_scale_at(c, (ani_time){ step, 12 });
    float spacing = s->style.boil_spacing;
    for (int k = 0; k < 10 && (double)spacing * sc < 6.0; k++) spacing *= 2.0f;
    return spacing;
}

// The paint style of a drawable object at `at`, with effective chaos `chaos`.
static emit_style sample_style(const an_scene *s, an_obj *c, float chaos, ani_time at) {
    const an_style *sty = &s->style;
    emit_style st = {
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
        const uint64_t step = (uint64_t)((at.num * 12) / at.den);
        st.boil = (ani_boil){ .amp = sty->boil * chaos, .spacing = ani_boil_spacing(s, c, at),
                              .seed = s->seed, .stream = c->rng_key, .step = step };
    }
    return st;
}

// ---- letters (docs/text.md) ---------------------------------------------------------

typedef struct bbox {
    float x0, y0, x1, y1;
} bbox;

#define BB_EMPTY ((bbox){ INFINITY, INFINITY, -INFINITY, -INFINITY })

static void bb_add(bbox *b, an_vec2 p) {
    b->x0 = fminf(b->x0, p.x);
    b->y0 = fminf(b->y0, p.y);
    b->x1 = fmaxf(b->x1, p.x);
    b->y1 = fmaxf(b->y1, p.y);
}

static void bb_union(bbox *b, bbox o) {
    b->x0 = fminf(b->x0, o.x0);
    b->y0 = fminf(b->y0, o.y0);
    b->x1 = fmaxf(b->x1, o.x1);
    b->y1 = fmaxf(b->y1, o.y1);
}

typedef struct glyph_item {
    an_obj *o;
    ani_affine w;     // world transform (snapped when calm)
    ani_affine pw;    // the text's world transform: pattern space, shared by its letters
    emit_style st;
    bool fill_on;     // the fill shows (draw-on pops it in when the outline closes)
    bool ink_on;      // the outline shows (has ink, some of it drawn)
    an_vec2 soff;     // shadow offset, scene px (clears the ink)
    int iv0, iv1;     // its ink triangles, once emitted
    bbox box;         // scratch: the box of its latest fans
} glyph_item;

// One letter at `at`: size factor on chaos, snapping, style, draw progress.
// Returns the effective chaos (for its children).
// rest: the letter's area scale on screen without verbs (the size factor's and
// the riso scale's input); plain: its own transform is a pure translation
static float sample_glyph(ani_mesh *m, const an_scene *s, an_obj *c, ani_affine pw, ani_affine w, float rest,
                          bool plain, float chaos, ani_time at, glyph_item *g) {
    const an_style *sty = &s->style;
    const float wscale = sqrtf(fabsf(w.a * w.d - w.b * w.c)); // with verbs: the ink's scale
    chaos *= ani_text_size_factor(c, rest, sty->boil_off ? 0.0f : sty->boil);
    an_obj *text = c->u.glyph.text;
    const bool in_text = c->parent == text;
    // calm letters sit on whole pixels relative to the text's origin
    if (chaos == 0.0f && in_text && plain && ani_text_snap(pw, &w, c->u.glyph.pen, 0.0f, (an_vec2){ 1, 1 }))
        m->snapped_letters++;
    *g = (glyph_item){ .o = c, .w = w, .pw = in_text ? pw : w };
    emit_style st = sample_style(s, c, chaos, at);
    const float rs = ani_text_riso_scale(c, rest);
    st.riso = (an_vec2){ st.riso.x * rs, st.riso.y * rs };
    // draw progress: the text's, spread over its letters, and the letter's own
    const float D = (float)ani_sample(ani_obj_prop(text, AN_PROP_DRAW), at).v[0];
    const float own = st.draw;
    bool shown;
    if (text->u.text.inked) {
        float local = 1.0f;
        if (D < 1.0f) {
            const float len = c->path ? c->path->total : 0.0f;
            local = len > 0.0f ? (D * text->u.text.total_arc - c->u.glyph.arc0) / len : (D > 0.0f ? 1.0f : 0.0f);
            local = local < 0.0f ? 0.0f : local > 1.0f ? 1.0f : local;
        }
        st.draw = own < local ? own : local;
        shown = st.draw > 0.0f;
    } else {
        // typewriter: whole letters in order; the letter's own draw must be complete
        const int n = text->u.text.nglyphs;
        shown = (D >= 1.0f || D * (float)n >= (float)(c->u.glyph.index + 1)) && own >= 1.0f;
    }
    g->fill_on = shown && has_fill(c, &st) && st.draw >= 1.0f;
    g->ink_on = shown && has_ink(c, &st);
    g->st = st;
    // the shadow clears the ink, as for shapes
    g->soff = st.shadow_offset;
    if (st.shadow && g->ink_on) {
        const float wpx = st.stroke_width * wscale * st.chaos;
        g->soff.x += st.shadow_offset.x > 0 ? wpx : (st.shadow_offset.x < 0 ? -wpx : 0.0f);
        g->soff.y += st.shadow_offset.y > 0 ? wpx : (st.shadow_offset.y < 0 ? -wpx : 0.0f);
    }
    return chaos;
}

static bool push_cmd(ani_mesh *m, int target, ani_paint paint, int first, int end) {
    if (end <= first) return true;
    if (!grow((void **)&m->cmds, &m->cmd_cap, m->ncmds + 1, sizeof(ani_cmd))) return false;
    m->cmds[m->ncmds++] = (ani_cmd){ .kind = ANI_CMD_TRIS, .target = target, .paint = paint, .first = first,
                                     .count = end - first };
    return true;
}

// Winding fans of the letter's contours (each a fan from its first point),
// boiled like its outline, translated by `off`. Extends *bb.
static bool glyph_fans(ani_mesh *m, const glyph_item *g, an_vec2 off, an_color col, bbox *bb) {
    const ani_path *p = g->o->path;
    for (int k = 0; k < p->nsubs; k++) {
        const ani_subpath *sp = &p->subs[k];
        if (sp->count < 3) continue;
        if (g->st.boil.amp > 0.0f) {
            const an_vec2 *ring; // mesh scratch, valid until the next stroke call
            const int n = ani_boil_ring_sub(m, p, k, g->w, &g->st.boil, &ring);
            if (n < 0) return false;
            if (n < 3) continue;
            const an_vec2 r0 = add(ring[0], off);
            bb_add(bb, r0);
            for (int i = 1; i + 1 < n; i++) {
                const an_vec2 a = add(ring[i], off), b = add(ring[i + 1], off);
                bb_add(bb, a);
                bb_add(bb, b);
                if (!ani_mesh_tri(m, r0, a, b, col)) return false;
            }
        } else {
            const an_vec2 r0 = add(ani_affine_apply(g->w, p->pts[sp->first]), off);
            an_vec2 a = add(ani_affine_apply(g->w, p->pts[sp->first + 1]), off);
            bb_add(bb, r0);
            bb_add(bb, a);
            for (int i = 2; i < sp->count; i++) {
                const an_vec2 b = add(ani_affine_apply(g->w, p->pts[sp->first + i]), off);
                bb_add(bb, b);
                if (!ani_mesh_tri(m, r0, a, b, col)) return false;
                a = b;
            }
        }
    }
    return true;
}

// The cover quad over a box (1 px margin: the cover only paints where the
// winding is nonzero, so it must reach every sample the fans touched).
static bool cover_quad(ani_mesh *m, bbox b, an_color col) {
    if (!(b.x1 >= b.x0)) return true;
    return quad(m, (an_vec2){ b.x0 - 1, b.y0 - 1 }, (an_vec2){ b.x1 + 1, b.y0 - 1 }, (an_vec2){ b.x1 + 1, b.y1 + 1 },
                (an_vec2){ b.x0 - 1, b.y1 + 1 }, col);
}

// Copy triangles [v0, v1), translated and recolored (pattern off). Extends *bb.
static bool copy_tris(ani_mesh *m, int v0, int v1, an_vec2 off, an_color c, bbox *bb) {
    const int n = v1 - v0;
    if (n <= 0) return true;
    if (!grow((void **)&m->v, &m->cap, m->count + n, sizeof(ani_vertex))) return false;
    for (int i = 0; i < n; i++) {
        const ani_vertex v = m->v[v0 + i];
        m->v[m->count] = (ani_vertex){ .x = v.x + off.x, .y = v.y + off.y, .r = c.r, .g = c.g, .b = c.b, .a = c.a };
        if (bb) bb_add(bb, (an_vec2){ m->v[m->count].x, m->v[m->count].y });
        m->count++;
    }
    return true;
}

static bool same_fill(const glyph_item *a, const glyph_item *b) {
    const an_color p = a->st.fill, q = b->st.fill;
    const an_pattern *x = &a->o->pattern, *y = &b->o->pattern;
    if (p.r != q.r || p.g != q.g || p.b != q.b || p.a != q.a || x->kind != y->kind) return false;
    if (x->kind == AN_PATTERN_SOLID) return true;
    return x->size == y->size && x->angle == y->angle && x->density == y->density &&
           memcmp(&a->pw, &b->pw, sizeof a->pw) == 0;
}

// Fans for a set of letters, then one cover per letter: wind + cover vertex
// ranges [*w0, *w1) and [*w1, *c1). pick(g) says which letters take part; offset
// and color per letter come from off_of / col_of. Covers get the fill pattern
// when `pattern` is set.
typedef enum fan_kind { FANS_FILL, FANS_SHADOW, FANS_SIL } fan_kind;

static bool fans_and_covers(ani_mesh *m, glyph_item *gs, int i0, int i1, fan_kind kind, an_color paper,
                            int *w0, int *w1, int *c1, bbox *all) {
    *w0 = m->count;
    for (int k = i0; k < i1; k++) {
        glyph_item *g = &gs[k];
        g->box = BB_EMPTY;
        if (!g->fill_on) continue;
        if (kind == FANS_SHADOW && !(g->st.shadow && g->st.shadow_color.a > 0.0f)) continue;
        bool ok;
        switch (kind) {
        case FANS_FILL: ok = glyph_fans(m, g, g->st.riso, g->st.fill, &g->box); break;
        case FANS_SHADOW: ok = glyph_fans(m, g, add(g->st.riso, g->soff), g->st.shadow_color, &g->box); break;
        default: // the silhouette: the fill where it's registered and where riso moved it
            ok = glyph_fans(m, g, (an_vec2){ 0, 0 }, paper, &g->box);
            if (ok && (g->st.riso.x != 0.0f || g->st.riso.y != 0.0f)) ok = glyph_fans(m, g, g->st.riso, paper, &g->box);
            break;
        }
        if (!ok) return false;
    }
    *w1 = m->count;
    for (int k = i0; k < i1; k++) {
        glyph_item *g = &gs[k];
        if (!(g->box.x1 >= g->box.x0)) continue;
        const int cv = m->count;
        const an_color col = kind == FANS_FILL ? g->st.fill : kind == FANS_SHADOW ? g->st.shadow_color : paper;
        if (!cover_quad(m, g->box, col)) return false;
        if (kind == FANS_FILL) set_pattern(m, cv, &g->o->pattern, g->pw, g->st.riso);
        if (kind == FANS_SIL) {
            for (int v = cv; v < m->count; v++) m->v[v].a = 1.0f;
        }
        if (all) bb_union(all, g->box);
    }
    *c1 = m->count;
    return true;
}

// A batch of letters drawn as ONE object into `target` (docs/text.md, problems 1
// and 3): [silhouette] all shadows [silhouette clear] [paper] all fills, all
// outlines. Fills are stencil-then-cover, non-zero, in runs of one fill color and
// pattern (a cover quad paints every nonzero sample under it, so letters with
// different fills must not share a wind/cover pair).
static bool emit_glyphs(ani_mesh *m, glyph_item *gs, int n, int target) {
    if (n <= 0) return true;
    const bool in_layer = target != ANI_TARGET_MAIN;
    const int v0 = m->count, cmd_pos = m->ncmds;
    bool any_shadow = false, any_paper = false, any_visible = false, ink_tr = false;
    an_color shadow_col = { 0 }, paper = { 0 };
    for (int i = 0; i < n; i++) {
        const glyph_item *g = &gs[i];
        if (!g->fill_on && !g->ink_on) continue;
        any_visible = true;
        if (g->st.shadow && g->st.shadow_color.a > 0.0f) {
            any_shadow = true;
            shadow_col = g->st.shadow_color; // a style color: the same for every letter
        }
        any_paper = any_paper || g->st.paper_under;
        ink_tr = ink_tr || (in_layer && g->ink_on && g->st.stroke.a < 1.0f);
        paper = g->st.paper;
    }
    if (!any_visible) return true;
    paper.a = 1.0f;
    const bool shadow_st = in_layer && any_shadow && shadow_col.a < 1.0f;
    const bool stencil = shadow_st || ink_tr;
    const bool cut = any_shadow, sil = cut || any_paper;

    // outlines: opaque, then translucent (write-once marks in a layer)
    int ink[3];
    for (int i = 0; i < n; i++) gs[i].iv0 = gs[i].iv1 = 0;
    ink[0] = m->count;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            glyph_item *g = &gs[i];
            if (!g->ink_on || (in_layer && g->st.stroke.a < 1.0f) != (pass == 1)) continue;
            g->iv0 = m->count;
            if (!emit_ink(m, g->o, g->w, (an_vec2){ 0, 0 }, &g->st, g->st.stroke)) return false;
            g->iv1 = m->count;
        }
        ink[pass + 1] = m->count;
    }

    // fills: runs of one fill, each [fans][covers]; ranges kept for the commands
    typedef struct fill_run { int w0, w1, c1; } fill_run;
    fill_run runs_local[16], *runs = runs_local;
    int nruns = 0, runs_cap = 16;
    for (int i = 0; i < n;) {
        if (!gs[i].fill_on) {
            i++;
            continue;
        }
        int j = i + 1;
        while (j < n && (!gs[j].fill_on || same_fill(&gs[i], &gs[j]))) j++;
        if (nruns == runs_cap) {
            fill_run *nr = malloc(sizeof *nr * (size_t)runs_cap * 2);
            if (!nr) goto fail;
            memcpy(nr, runs, sizeof *nr * (size_t)nruns);
            if (runs != runs_local) free(runs);
            runs = nr;
            runs_cap *= 2;
        }
        fill_run *r = &runs[nruns++];
        if (!fans_and_covers(m, gs, i, j, FANS_FILL, paper, &r->w0, &r->w1, &r->c1, NULL)) goto fail;
        i = j;
    }

    // shadows: the fills' (fans + covers) and the outlines' (copies)
    int sw0 = 0, sw1 = 0, sc1 = 0, si0 = 0, si1 = 0;
    if (any_shadow) {
        if (!fans_and_covers(m, gs, 0, n, FANS_SHADOW, paper, &sw0, &sw1, &sc1, NULL)) goto fail;
        si0 = m->count;
        for (int i = 0; i < n; i++) {
            const glyph_item *g = &gs[i];
            if (g->st.shadow && g->iv1 > g->iv0 && !copy_tris(m, g->iv0, g->iv1, g->soff, shadow_col, NULL)) goto fail;
        }
        si1 = m->count;
    }

    // silhouette: every letter's fill (registered and moved) and outline
    int lw0 = 0, lw1 = 0, lc1 = 0, li0 = 0, li1 = 0, lq0 = 0, lq1 = 0;
    if (sil) {
        bbox all = BB_EMPTY;
        if (!fans_and_covers(m, gs, 0, n, FANS_SIL, paper, &lw0, &lw1, &lc1, &all)) goto fail;
        li0 = m->count;
        for (int i = 0; i < n; i++) {
            const glyph_item *g = &gs[i];
            if (g->iv1 > g->iv0 && !copy_tris(m, g->iv0, g->iv1, (an_vec2){ 0, 0 }, paper, &all)) goto fail;
        }
        li1 = m->count;
        lq0 = m->count;
        if (cut && !cover_quad(m, all, paper)) goto fail;
        lq1 = m->count;
    }

    // an earlier translucent object in this layer left marks: reset them first
    if (stencil && m->layer_used[target] && !stencil_clear_before(m, target, v0, cmd_pos)) goto fail;

    // commands, in draw order
    bool ok = true;
    if (cut) {
        ok = ok && push_cmd(m, target, ANI_PAINT_WIND_SIL, lw0, lw1) && push_cmd(m, target, ANI_PAINT_COVER_SIL, lw1, lc1) &&
             push_cmd(m, target, ANI_PAINT_SILHOUETTE, li0, li1);
    }
    if (any_shadow) {
        ok = ok && push_cmd(m, target, shadow_st ? ANI_PAINT_WIND_SHADOW : ANI_PAINT_WIND_SHADOW_CUT, sw0, sw1) &&
             push_cmd(m, target, shadow_st ? ANI_PAINT_COVER_SHADOW : ANI_PAINT_COVER, sw1, sc1) &&
             push_cmd(m, target, shadow_st ? ANI_PAINT_SHADOW : ANI_PAINT_SHADOW_CUT, si0, si1);
    }
    if (cut) ok = ok && push_cmd(m, target, ANI_PAINT_SILHOUETTE_CLEAR, lq0, lq1);
    if (any_paper) {
        ok = ok && push_cmd(m, target, ANI_PAINT_WIND, lw0, lw1) && push_cmd(m, target, ANI_PAINT_COVER, lw1, lc1) &&
             push_cmd(m, target, ANI_PAINT_DIRECT, li0, li1);
    }
    for (int r = 0; ok && r < nruns; r++) {
        ok = push_cmd(m, target, ANI_PAINT_WIND, runs[r].w0, runs[r].w1) &&
             push_cmd(m, target, ANI_PAINT_COVER, runs[r].w1, runs[r].c1);
    }
    ok = ok && push_cmd(m, target, ANI_PAINT_DIRECT, ink[0], ink[1]) &&
         push_cmd(m, target, in_layer ? ANI_PAINT_INK : ANI_PAINT_DIRECT, ink[1], ink[2]);
    if (runs != runs_local) free(runs);
    if (!ok) return false;
    if (stencil) m->layer_used[target] = true;
    m->merge_barrier = true;
    return true;
fail:
    if (runs != runs_local) free(runs);
    return false;
}

// ---- memoized tessellation ---------------------------------------------------------
// An object's emitted vertices and commands depend only on: the object (its
// immutable path, brush, pattern, flags: identified by scene serial + id), its
// sampled world transform and style at the frame (colors, width, draw, riso,
// shadow, chaos, boil incl. its 12 Hz step), whether it draws into a layer, and
// whether that layer's stencil is already dirty. Those, serialized field by
// field, are the key; a hit replays the stored vertices and commands. Every
// object starts a fresh command run (with or without the cache), and
// merge_runs() joins adjacent runs at the end of the frame, so the output is the
// same either way. One entry per object; an entry is stored only when the
// object didn't also miss the previous frame with another key (a continuously
// moving object costs only the key compare). Bounded by memo_budget bytes;
// entries not used this frame are evicted first.

typedef struct memo_entry {
    uint64_t serial;
    uint32_t id;
    uint8_t kind;             // 0: object, 1: letter batch (id: its first letter)
    bool stored, missed_last;
    unsigned char *key;
    int key_len, key_cap;
    ani_vertex *v;
    int nv;
    ani_cmd *cmds;            // first relative to the object's first vertex
    int ncmds;
    bool sets_layer_used, barrier_after;
    int64_t stamp;            // last frame it was used
} memo_entry;

struct ani_memo {
    memo_entry *e;
    int n, cap;
    int *table;               // open addressing, entry index + 1; 0 empty
    int tcap;
    unsigned char *kb;        // the key being built
    int kb_len, kb_cap;
    size_t bytes;
    int64_t stamp;
};

static void memo_drop(struct ani_memo *mm, memo_entry *e) {
    if (!e->stored) return;
    mm->bytes -= (size_t)e->nv * sizeof(ani_vertex) + (size_t)e->ncmds * sizeof(ani_cmd);
    free(e->v);
    free(e->cmds);
    e->v = NULL;
    e->cmds = NULL;
    e->nv = e->ncmds = 0;
    e->stored = false;
}

static void memo_free(struct ani_memo *mm) {
    if (!mm) return;
    for (int i = 0; i < mm->n; i++) {
        memo_drop(mm, &mm->e[i]);
        free(mm->e[i].key);
    }
    free(mm->e);
    free(mm->table);
    free(mm->kb);
    free(mm);
}

static bool kb_put(struct ani_memo *mm, const void *p, size_t n) {
    if (!grow((void **)&mm->kb, &mm->kb_cap, mm->kb_len + (int)n, 1)) return false;
    memcpy(mm->kb + mm->kb_len, p, n);
    mm->kb_len += (int)n;
    return true;
}
#define KB(x) kb_put(mm, &(x), sizeof(x))

static uint64_t memo_hash(uint64_t serial, uint32_t id, uint8_t kind) {
    uint64_t h = serial * 0x9E3779B97F4A7C15ull ^ ((uint64_t)id << 8 | kind);
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    return h ^ (h >> 29);
}

static memo_entry *memo_find(struct ani_memo *mm, uint64_t serial, uint32_t id, uint8_t kind) {
    if ((mm->n + 1) * 2 > mm->tcap) { // keep the table at most half full
        const int nc = mm->tcap ? mm->tcap * 2 : 1024;
        int *t = calloc((size_t)nc, sizeof *t);
        if (!t) return NULL;
        for (int i = 0; i < mm->n; i++) {
            size_t h = (size_t)memo_hash(mm->e[i].serial, mm->e[i].id, mm->e[i].kind) & (size_t)(nc - 1);
            while (t[h]) h = (h + 1) & (size_t)(nc - 1);
            t[h] = i + 1;
        }
        free(mm->table);
        mm->table = t;
        mm->tcap = nc;
    }
    size_t h = (size_t)memo_hash(serial, id, kind) & (size_t)(mm->tcap - 1);
    while (mm->table[h]) {
        memo_entry *e = &mm->e[mm->table[h] - 1];
        if (e->serial == serial && e->id == id && e->kind == kind) return e;
        h = (h + 1) & (size_t)(mm->tcap - 1);
    }
    if (!grow((void **)&mm->e, &mm->cap, mm->n + 1, sizeof(memo_entry))) return NULL;
    mm->e[mm->n] = (memo_entry){ .serial = serial, .id = id, .kind = kind };
    mm->table[h] = ++mm->n;
    return &mm->e[mm->n - 1];
}

// Room for `need` more bytes: drop entries not used this frame
// (one sweep, in entry order), until it fits. False if it still doesn't.
static bool memo_room(struct ani_memo *mm, size_t need, size_t budget) {
    for (int i = 0; i < mm->n && mm->bytes + need > budget; i++) {
        if (mm->e[i].stored && mm->e[i].stamp < mm->stamp) memo_drop(mm, &mm->e[i]);
    }
    return mm->bytes + need <= budget;
}

typedef bool (*memo_fn)(ani_mesh *m, void *ctx, int target);

// Run fn (which emits one object, or one letter batch, into target) through the
// cache. The caller has built the object's part of the key in mm->kb.
static bool memo_run(ani_mesh *m, uint64_t serial, uint32_t id, uint8_t kind, int target, memo_fn fn, void *ctx) {
    m->merge_barrier = true; // an object never extends the previous object's run
    if (m->memo_off) return fn(m, ctx, target);
    struct ani_memo *mm = m->memo;
    const bool main = target == ANI_TARGET_MAIN, dirty = !main && m->layer_used[target];
    if (!KB(main) || !KB(dirty)) return false;
    memo_entry *e = memo_find(mm, serial, id, kind);
    if (!e) return false;
    e->stamp = mm->stamp;
    const bool same = e->key_len == mm->kb_len && memcmp(e->key, mm->kb, (size_t)mm->kb_len) == 0;
    if (same && e->stored) { // replay
        const int v0 = m->count;
        if (!grow((void **)&m->v, &m->cap, m->count + e->nv, sizeof(ani_vertex)) ||
            !grow((void **)&m->cmds, &m->cmd_cap, m->ncmds + e->ncmds, sizeof(ani_cmd)))
            return false;
        memcpy(m->v + v0, e->v, sizeof(ani_vertex) * (size_t)e->nv);
        m->count += e->nv;
        for (int i = 0; i < e->ncmds; i++) {
            ani_cmd c = e->cmds[i];
            c.first += v0;
            c.target = target;
            m->cmds[m->ncmds++] = c;
        }
        if (e->sets_layer_used) m->layer_used[target] = true;
        m->merge_barrier = e->barrier_after;
        e->missed_last = false;
        m->memo_hits++;
        return true;
    }
    m->memo_misses++;
    const int v0 = m->count, c0 = m->ncmds;
    if (!fn(m, ctx, target)) return false;
    // store, unless this object also missed last frame with a different key
    const bool store = !e->missed_last || same;
    e->missed_last = true;
    if (!same) {
        if (!grow((void **)&e->key, &e->key_cap, mm->kb_len, 1)) return false;
        memcpy(e->key, mm->kb, (size_t)mm->kb_len);
        e->key_len = mm->kb_len;
    }
    memo_drop(mm, e);
    const int nv = m->count - v0, nc = m->ncmds - c0;
    const size_t bytes = (size_t)nv * sizeof(ani_vertex) + (size_t)nc * sizeof(ani_cmd);
    const size_t budget = m->memo_budget ? m->memo_budget : ANI_MEMO_BUDGET;
    if (!store || !memo_room(mm, bytes, budget)) return true;
    e->v = malloc(sizeof(ani_vertex) * (size_t)(nv ? nv : 1));
    e->cmds = malloc(sizeof(ani_cmd) * (size_t)(nc ? nc : 1));
    if (!e->v || !e->cmds) {
        free(e->v);
        free(e->cmds);
        e->v = NULL;
        e->cmds = NULL;
        return true; // not cached; the output is already emitted
    }
    memcpy(e->v, m->v + v0, sizeof(ani_vertex) * (size_t)nv);
    for (int i = 0; i < nc; i++) {
        e->cmds[i] = m->cmds[c0 + i];
        e->cmds[i].first -= v0;
    }
    e->nv = nv;
    e->ncmds = nc;
    e->sets_layer_used = !main && !dirty && m->layer_used[target];
    e->barrier_after = m->merge_barrier;
    e->stored = true;
    mm->bytes += bytes;
    return true;
}

static bool key_style(struct ani_memo *mm, const emit_style *st) {
    return KB(st->fill) && KB(st->stroke) && KB(st->stroke_width) && KB(st->draw) && KB(st->riso) &&
           KB(st->shadow) && KB(st->shadow_offset) && KB(st->shadow_color) && KB(st->chaos) &&
           KB(st->boil.amp) && KB(st->boil.spacing) && KB(st->boil.seed) && KB(st->boil.stream) &&
           KB(st->boil.step) && KB(st->paper_under) && KB(st->paper);
}

typedef struct obj_ctx {
    const an_obj *o;
    ani_affine w;
    const emit_style *st;
} obj_ctx;

static bool emit_obj_cb(ani_mesh *m, void *ctx, int target) {
    const obj_ctx *c = ctx;
    return emit(m, c->o, c->w, c->st, target);
}

// extra / nextra: more key bytes (an edge's resolved geometry)
static bool emit_memo(ani_mesh *m, const an_scene *s, const an_obj *o, ani_affine w, const emit_style *st, int target,
                      const void *extra, size_t nextra) {
    obj_ctx ctx = { o, w, st };
    if (!m->memo_off) {
        struct ani_memo *mm = m->memo;
        mm->kb_len = 0;
        if (!KB(o->id) || !KB(w) || !key_style(mm, st)) return false;
        if (extra && !kb_put(mm, extra, nextra)) return false;
    }
    return memo_run(m, s->serial, o->id, 0, target, emit_obj_cb, &ctx);
}

typedef struct glyphs_ctx {
    glyph_item *gs;
    int n;
} glyphs_ctx;

static bool emit_glyphs_cb(ani_mesh *m, void *ctx, int target) {
    glyphs_ctx *c = ctx;
    return emit_glyphs(m, c->gs, c->n, target);
}

static bool emit_glyphs_memo(ani_mesh *m, const an_scene *s, glyph_item *gs, int n, int target) {
    if (n <= 0) return true;
    glyphs_ctx ctx = { gs, n };
    if (!m->memo_off) {
        struct ani_memo *mm = m->memo;
        mm->kb_len = 0;
        if (!KB(n)) return false;
        for (int i = 0; i < n; i++) {
            const glyph_item *g = &gs[i];
            if (!KB(g->o->id) || !KB(g->w) || !KB(g->pw) || !key_style(mm, &g->st) || !KB(g->fill_on) ||
                !KB(g->ink_on) || !KB(g->soff))
                return false;
        }
    }
    return memo_run(m, s->serial, gs[0].o->id, 1, target, emit_glyphs_cb, &ctx);
}

// Join adjacent runs of the same target and paint over contiguous vertices.
// Draws happen in list order either way, so this only saves draw calls.
static void merge_runs(ani_mesh *m) {
    int out = 0;
    for (int i = 0; i < m->ncmds; i++) {
        const ani_cmd c = m->cmds[i];
        if (out > 0) {
            ani_cmd *p = &m->cmds[out - 1];
            if (c.kind == ANI_CMD_TRIS && p->kind == ANI_CMD_TRIS && p->target == c.target && p->paint == c.paint &&
                p->first + p->count == c.first) {
                p->count += c.count;
                continue;
            }
        }
        m->cmds[out++] = c;
    }
    m->ncmds = out;
}

// Does any letter of a text have a translucent paint at `at`? (Outside a layer
// the whole text is then isolated: one layer, not one per letter.)
static bool text_translucent(const an_scene *s, const an_obj *t, ani_time at) {
    for (int i = 0; i < t->u.text.nglyphs; i++) {
        an_obj *g = t->u.text.glyphs[i];
        if (g->parent != t) continue;
        const float fa = (float)ani_sample(ani_obj_prop(g, AN_PROP_FILL), at).v[3];
        const float sa = (float)ani_sample(ani_obj_prop(g, AN_PROP_STROKE), at).v[3];
        if ((!g->no_fill && fa > 0.0f && fa < 1.0f) || (g->has_stroke && sa > 0.0f && sa < 1.0f)) return true;
        const bool shadow = g->shadow == AN_ON || (g->shadow == AN_AUTO && s->style.shadow);
        if (shadow && s->style.shadow_color.a < 1.0f) return true;
    }
    return false;
}

// Emit the pending batch of letters (m->glyphs[base..]) into target.
static bool flush_glyphs(ani_mesh *m, const an_scene *s, int base, int target) {
    const int n = m->glyph_count - base;
    m->glyph_count = base;
    return n <= 0 || emit_glyphs_memo(m, s, (glyph_item *)m->glyphs + base, n, target);
}

// pchaos: effective chaos of the parent (scene dial x multipliers down the tree)
// prest: the parent's area scale on screen without verbs (camera zoom x scales):
// the text size factor's "rest" scale (docs/camera.md, (d))
static bool visit_children(ani_mesh *m, const an_scene *s, const an_obj *parent, ani_affine pw, int target,
                           float pchaos, float prest, ani_time at) {
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

    // consecutive letters batch into one draw (docs/text.md, problem 3); anything
    // else, an isolated letter, or a letter with children ends the batch
    const int gbase = m->glyph_count;
    for (int i = 0; i < n; i++) {
        an_obj *c = m->scratch[base + i].obj; // re-read: scratch may have moved
        const float opacity = (float)ani_sample(ani_obj_prop(c, AN_PROP_OPACITY), at).v[0];
        if (opacity <= 0.0f) continue; // invisible, and so are its children
        float chaos = pchaos * (float)ani_sample(ani_obj_prop(c, AN_PROP_CHAOS), at).v[0];
        const ani_value pos = ani_sample(ani_obj_prop(c, AN_PROP_POS), at);
        const ani_value sc = ani_sample(ani_obj_prop(c, AN_PROP_SCALE), at);
        const ani_value rot = ani_sample(ani_obj_prop(c, AN_PROP_ROTATION), at);
        const an_vec2 scale = { (float)sc.v[0], (float)sc.v[1] };
        const ani_value vs = ani_sample(ani_obj_prop(c, AN_PROP_VERB_SCALE), at);
        const ani_value vr = ani_sample(ani_obj_prop(c, AN_PROP_VERB_ROTATION), at);
        const an_vec2 vscale = { (float)vs.v[0], (float)vs.v[1] };
        const bool verb = !(vscale.x == 1.0f && vscale.y == 1.0f && vr.v[0] == 0.0);
        ani_affine local = ani_local((an_vec2){ (float)pos.v[0], (float)pos.v[1] }, scale, (float)rot.v[0]);
        if (verb) {
            const ani_value vp = ani_sample(ani_obj_prop(c, AN_PROP_VERB_PIVOT), at);
            local = ani_verb_local(local, vscale, (float)vr.v[0], (an_vec2){ (float)vp.v[0], (float)vp.v[1] });
        }
        const ani_affine w = ani_affine_mul(pw, local);
        const float rest = prest * sqrtf(fabsf(scale.x * scale.y));
        if (c->path) { // drawn: its largest scale on screen (max_zoom, docs/camera.md)
            const float sq = w.a * w.a + w.b * w.b + w.c * w.c + w.d * w.d, det = w.a * w.d - w.b * w.c;
            const float sv = sqrtf(0.5f * (sq + sqrtf(fmaxf(sq * sq - 4.0f * det * det, 0.0f))));
            if (sv > m->max_screen_scale) m->max_screen_scale = sv;
        }
        if (c->kind == AN_KIND_GLYPH) {
            glyph_item g;
            chaos = sample_glyph(m, s, c, pw, w, rest, !verb && rot.v[0] == 0.0 && scale.x == 1.0f && scale.y == 1.0f, chaos, at, &g);
            // a letter fading on its own gets its own layer; outside a layer, so does
            // a translucent one that isn't in its text (a text isolates as a whole)
            const bool tr = (g.fill_on && g.st.fill.a < 1.0f) || (g.ink_on && g.st.stroke.a < 1.0f) ||
                            (g.st.shadow && g.st.shadow_color.a < 1.0f && (g.fill_on || g.ink_on));
            const bool isolate = m->force_isolate || opacity < 1.0f || (tr && target == ANI_TARGET_MAIN);
            if (isolate) {
                if (!flush_glyphs(m, s, gbase, target)) return false;
                const int t = layer_new(m, target, opacity, c);
                if (t < 0 || !emit_glyphs_memo(m, s, &g, 1, t)) return false;
                if (c->first_child && !visit_children(m, s, c, g.w, t, chaos, rest, at)) return false;
                continue;
            }
            if (!grow(&m->glyphs, &m->glyph_cap, m->glyph_count + 1, sizeof(glyph_item))) return false;
            ((glyph_item *)m->glyphs)[m->glyph_count++] = g;
            if (c->first_child) {
                if (!flush_glyphs(m, s, gbase, target)) return false;
                if (!visit_children(m, s, c, g.w, target, chaos, rest, at)) return false;
            }
            continue;
        }
        if (!flush_glyphs(m, s, gbase, target)) return false;
        if (c->kind == AN_KIND_POLYGON) {
            // filled by stencil-then-cover, like a letter (one-item batch)
            glyph_item g = { .o = c, .w = w, .pw = w };
            g.st = sample_style(s, c, chaos, at);
            g.fill_on = has_fill(c, &g.st) && g.st.draw >= 1.0f; // pops in when its outline closes
            g.ink_on = has_ink(c, &g.st) && g.st.draw > 0.0f;
            g.soff = g.st.shadow_offset;
            if (g.st.shadow && g.ink_on) {
                const float wpx = g.st.stroke_width * sqrtf(fabsf(w.a * w.d - w.b * w.c)) * g.st.chaos;
                g.soff.x += g.st.shadow_offset.x > 0 ? wpx : (g.st.shadow_offset.x < 0 ? -wpx : 0.0f);
                g.soff.y += g.st.shadow_offset.y > 0 ? wpx : (g.st.shadow_offset.y < 0 ? -wpx : 0.0f);
            }
            const bool tr = (g.fill_on && g.st.fill.a < 1.0f) || (g.ink_on && g.st.stroke.a < 1.0f) ||
                            (g.st.shadow && g.st.shadow_color.a < 1.0f && (g.fill_on || g.ink_on));
            int t = target;
            if (m->force_isolate || opacity < 1.0f || (tr && target == ANI_TARGET_MAIN)) {
                t = layer_new(m, target, opacity, c);
                if (t < 0) return false;
            }
            if (!emit_glyphs_memo(m, s, &g, 1, t)) return false;
            if (c->first_child && !visit_children(m, s, c, w, t, chaos, rest, at)) return false;
            continue;
        }
        emit_style st = { 0 };
        const bool drawable = c->kind != AN_KIND_GROUP && c->kind != AN_KIND_TEXT && c->kind != AN_KIND_CODE &&
                              c->kind != AN_KIND_ARRAY && c->kind != AN_KIND_CALLOUT && c->kind != AN_KIND_NODE &&
                              c->kind != AN_KIND_AXES && c->kind != AN_KIND_EDGE;
        if (drawable) st = sample_style(s, c, chaos, at);
        // Isolate when the node fades as a unit, or (outside any layer) when a
        // translucent paint needs stencil coverage. docs/layers.md
        const bool translucent = drawable ? ((has_fill(c, &st) && st.fill.a < 1.0f) ||
                                             (has_ink(c, &st) && st.stroke.a < 1.0f) ||
                                             (st.shadow && st.shadow_color.a < 1.0f &&
                                              (has_fill(c, &st) || has_ink(c, &st))))
                                          : c->kind == AN_KIND_TEXT && target == ANI_TARGET_MAIN &&
                                                text_translucent(s, c, at);
        const bool isolate = m->force_isolate || opacity < 1.0f || (translucent && target == ANI_TARGET_MAIN);
        int t = target;
        if (isolate) {
            t = layer_new(m, target, opacity, c);
            if (t < 0) return false;
        }
        if (c->kind == AN_KIND_EDGE) {
            // resolved from its nodes at this frame (docs/graphs.md): the path lives
            // in the frame arena, in the parent's space (so it's drawn with pw)
            an_vec2 g[4];
            if (!ani_edge_resolve(c, at, &g[0], &g[1], &g[2], &g[3])) continue;
            const an_vec2 se = c->u.edge.directed ? (an_vec2){ g[1].x - g[3].x * 10.0f, g[1].y - g[3].y * 10.0f } : g[1];
            ani_path_builder b;
            ani_pb_init(&b);
            bool ok = ani_pb_move(&b, g[0]);
            const bool bent = g[2].x != (g[0].x + g[1].x) * 0.5f || g[2].y != (g[0].y + g[1].y) * 0.5f;
            ok = ok && (bent ? ani_pb_quad(&b, g[2], se, s->flatness) : ani_pb_line(&b, se));
            ani_path *path = ok ? ani_pb_finish(&b, &m->frame_arena) : NULL;
            ani_pb_free(&b);
            if (!path) return false;
            an_obj tmp = *c;
            tmp.kind = AN_KIND_STROKE;
            tmp.path = path;
            tmp.has_stroke = true;
            emit_style est = sample_style(s, c, chaos, at);
            const bool tr = has_ink(&tmp, &est) && est.stroke.a < 1.0f;
            int t = target;
            if (m->force_isolate || opacity < 1.0f || (tr && target == ANI_TARGET_MAIN)) {
                t = layer_new(m, target, opacity, c);
                if (t < 0) return false;
            }
            if (!emit_memo(m, s, &tmp, pw, &est, t, g, sizeof g)) return false;
            // the head (and any children) ride on the tip: translate to it, rotate to the end tangent
            const ani_affine tipf = ani_affine_mul(pw, (ani_affine){ g[3].x, g[3].y, -g[3].y, g[3].x, g[1].x, g[1].y });
            if (c->first_child && !visit_children(m, s, c, tipf, t, chaos, rest, at)) return false;
            continue;
        }
        if (drawable && !emit_memo(m, s, c, w, &st, t, NULL, 0)) return false;
        if (c->first_child && !visit_children(m, s, c, w, t, chaos, rest, at)) return false;
    }
    if (!flush_glyphs(m, s, gbase, target)) return false;
    m->scratch_count = base;
    return true;
}

bool ani_tess_scene(ani_mesh *m, const an_scene *s, int64_t frame) {
    m->count = 0;
    m->ncmds = 0;
    m->nlayers = 0;
    m->scratch_count = 0;
    m->glyph_count = 0;
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
    // the camera: a pure translation becomes a per-draw offset; anything else is
    // the root of every world transform
    const ani_camera cam = ani_camera_at((an_scene *)s, at); // sampling only reads
    ani_affine root = { 1, 0, 0, 1, 0, 0 };
    m->cam_dx = m->cam_dy = 0.0f;
    m->max_screen_scale = 0.0f;
    m->snapped_letters = 0;
    // view = translate(center - shake) rotate(-rot) scale(zoom) translate(-pos): the
    // shake is in screen px, after zoom and rotation (docs/camera.md)
    if (cam.zoom == 1.0f && cam.rot == 0.0f && !m->cam_fast_off) {
        m->cam_dx = w * 0.5f - cam.shake.x - cam.pos.x;
        m->cam_dy = h * 0.5f - cam.shake.y - cam.pos.y;
    } else {
        root = ani_affine_mul(ani_local((an_vec2){ w * 0.5f - cam.shake.x, h * 0.5f - cam.shake.y },
                                        (an_vec2){ cam.zoom, cam.zoom }, -cam.rot),
                              (ani_affine){ 1, 0, 0, 1, -cam.pos.x, -cam.pos.y });
    }
    ani_arena_reset(&m->frame_arena);
    m->memo_hits = m->memo_misses = 0;
    if (!m->memo_off && !m->memo) {
        m->memo = calloc(1, sizeof *m->memo);
        if (!m->memo) return false;
    }
    if (m->memo) m->memo->stamp++;
    if (!visit_children(m, s, &s->root, root, ANI_TARGET_MAIN, chaos, cam.zoom, at)) return false;
    merge_runs(m);
    return true;
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
