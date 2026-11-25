#include "scene_internal.h"
#include "rng_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- scene ------------------------------------------------------------------

an_scene *an_scene_create(const an_scene_desc *desc) {
    const an_scene_desc d = desc ? *desc : (an_scene_desc){ 0 };
    an_scene *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->width = d.width > 0 ? d.width : 1920;
    s->height = d.height > 0 ? d.height : 1080;
    s->fps = d.fps > 0 ? d.fps : 60;
    s->seed = d.seed;
    // style: zero fields take their defaults (docs/layers.md)
    an_style st = d.style;
    if (!st.palette) st.palette = &AN_PALETTE_RIOT;
    if (st.riso_offset.x == 0.0f && st.riso_offset.y == 0.0f) st.riso_offset = (an_vec2){ 4, -3 };
    if (st.shadow_offset.x == 0.0f && st.shadow_offset.y == 0.0f) st.shadow_offset = (an_vec2){ 8, 8 };
    if (!(st.shadow_color.a > 0.0f)) st.shadow_color = st.palette->shadow;
    if (!(st.grain > 0.0f)) st.grain = 0.06f;
    if (!(st.boil > 0.0f)) st.boil = 1.5f;
    if (!(st.boil_spacing > 0.0f)) st.boil_spacing = 15.0f;
    if (!(st.vignette > 0.0f)) st.vignette = 0.25f;
    s->style = st;
    s->background = d.background.a > 0.0f ? d.background : st.palette->paper;
    s->rng = an_rng_make(s->seed, ANI_RNG_SCENE_KEY);
    // identity for renderer caches (a later scene may reuse this address); it
    // never affects what's drawn
    static uint64_t next_serial = 0;
    s->serial = ++next_serial;
    s->chaos = 1.0f; // the look is on by default (docs/layers.md)
    s->cam_pos = (an_vec2){ (float)s->width * 0.5f, (float)s->height * 0.5f };
    s->cam_zoom = 1.0f;
    s->max_zoom = d.max_zoom > 0.0f ? d.max_zoom : 4.0f;
    s->flatness = 0.2f / s->max_zoom; // 0.05 at the default: the tolerance before max_zoom existed
    ani_arena_init(&s->arena, 0);
    s->root.kind = AN_KIND_GROUP;
    s->root.scene = s;
    s->root.scale = (an_vec2){ 1, 1 };
    s->root.opacity = 1.0f;
    s->root.chaos = 1.0f;
    s->root.rng_key = ANI_RNG_ROOT_KEY;
    return s;
}

void an_scene_destroy(an_scene *s) {
    if (!s) return;
    ani_arena_release(&s->arena);
    free(s);
}

// ---- tree -------------------------------------------------------------------

static void detach(an_obj *o) {
    an_obj *p = o->parent;
    if (!p) return;
    if (o->prev_sibling) o->prev_sibling->next_sibling = o->next_sibling;
    else p->first_child = o->next_sibling;
    if (o->next_sibling) o->next_sibling->prev_sibling = o->prev_sibling;
    else p->last_child = o->prev_sibling;
    o->parent = o->prev_sibling = o->next_sibling = NULL;
}

static void append_child(an_obj *p, an_obj *o) {
    o->parent = p;
    o->prev_sibling = p->last_child;
    o->next_sibling = NULL;
    if (p->last_child) p->last_child->next_sibling = o;
    else p->first_child = o;
    p->last_child = o;
}

// NULL -> root. Returns NULL if parent is from another scene.
static an_obj *resolve_parent(an_scene *s, an_obj *parent) {
    if (!parent) return &s->root;
    return parent->scene == s ? parent : NULL;
}

bool an_obj_set_parent(an_obj *o, an_obj *parent) {
    if (!o || o == &o->scene->root) return false;
    an_obj *p = resolve_parent(o->scene, parent);
    if (!p) return false;
    for (an_obj *a = p; a; a = a->parent) {
        if (a == o) return false; // p is o or a descendant of o
    }
    detach(o);
    append_child(p, o);
    return true;
}

// ---- creation ---------------------------------------------------------------

an_obj *ani_obj_make(an_scene *s, an_kind kind, const ani_common_opts *c) {
    if (!s) return NULL;
    an_obj *p = resolve_parent(s, c->parent);
    if (!p) return NULL;
    if (c->name) {
        for (const an_obj *n = s->named; n; n = n->next_named) {
            if (strcmp(n->name, c->name) == 0) {
                fprintf(stderr, "anown: an object named '%s' already exists in this scene\n", c->name);
                return NULL;
            }
        }
    }
    const char *name = c->name ? ani_arena_strdup(&s->arena, c->name) : NULL;
    if (c->name && !name) return NULL;
    an_obj *o = ANI_NEW(&s->arena, an_obj);
    if (!o) return NULL;
    o->kind = kind;
    o->id = ++s->count;
    o->scene = s;
    o->name = name;
    o->rng_key = name ? ani_rng_name_key(name) : ani_rng_child_key(p->rng_key, p->children_made);
    p->children_made++;
    o->rng = an_rng_make(s->seed, o->rng_key);
    o->birth = s->cursor;
    if (name) {
        o->next_named = s->named;
        s->named = o;
    }
    o->pos = c->pos;
    o->scale = (c->scale.x == 0.0f && c->scale.y == 0.0f) ? (an_vec2){ 1, 1 } : c->scale;
    o->rotation = c->rotation;
    o->opacity = c->hidden ? 0.0f : 1.0f;
    o->z = c->z;
    o->fill = c->fill.a > 0.0f ? c->fill : AN_BLACK;
    o->stroke = c->stroke.a > 0.0f ? c->stroke : AN_BLACK;
    o->stroke_width = c->stroke_width > 0.0f ? c->stroke_width : 4.0f;
    o->draw = c->undrawn ? 0.0f : 1.0f;
    o->chaos = 1.0f;
    o->vscale = (an_vec2){ 1, 1 };
    o->no_fill = c->no_fill;
    o->riso = c->riso;
    o->shadow = c->shadow;
    o->pattern = c->pattern;
    o->paper_under = c->paper_under == AN_ON; // AUTO is off for plain objects
    if (!(o->pattern.size > 0.0f)) o->pattern.size = 12.0f;
    if (!(o->pattern.density > 0.0f)) o->pattern.density = 0.5f;
    if (o->pattern.density > 1.0f) o->pattern.density = 1.0f;
    o->brush = (ani_brush){
        .taper_in = c->brush.taper_in > 0.0f ? c->brush.taper_in : 0.0f,
        .taper_out = c->brush.taper_out > 0.0f ? c->brush.taper_out : 0.0f,
        .overshoot = c->brush.overshoot > 0.0f ? c->brush.overshoot : 0.0f,
    };
    if (c->brush.pressure && c->brush.pressure_count > 0) {
        float *pr = ani_arena_alloc(&s->arena, sizeof(float) * (size_t)c->brush.pressure_count, _Alignof(float));
        if (!pr) return NULL;
        memcpy(pr, c->brush.pressure, sizeof(float) * (size_t)c->brush.pressure_count);
        o->brush.pressure = pr;
        o->brush.pressure_count = c->brush.pressure_count;
    }
    append_child(p, o);
    return o;
}

// Rect and circle outlines are opt-in: drawn when a stroke color or width is given.
bool ani_outline_requested(an_color stroke, float width) {
    return stroke.a > 0.0f || width > 0.0f;
}

static an_obj *with_path(an_obj *o, ani_path *path, bool stroked) {
    if (o) {
        o->path = path;
        o->has_stroke = stroked;
        // the verb pivot: the center of the path's bounding box (rects and circles
        // are centered on their origin, so it's (0, 0) for them)
        if (path && path->npts > 0 && o->kind != AN_KIND_RECT && o->kind != AN_KIND_CIRCLE) {
            float x0 = path->pts[0].x, x1 = x0, y0 = path->pts[0].y, y1 = y0;
            for (int i = 1; i < path->npts; i++) {
                x0 = fminf(x0, path->pts[i].x);
                x1 = fmaxf(x1, path->pts[i].x);
                y0 = fminf(y0, path->pts[i].y);
                y1 = fmaxf(y1, path->pts[i].y);
            }
            o->vpivot = (an_vec2){ (x0 + x1) * 0.5f, (y0 + y1) * 0.5f };
        }
    }
    return o;
}

// Each shape builds its path first (in the arena), then the object, so a failed
// allocation never leaves a half-built object in the tree.

an_obj *an_rect(an_scene *s, const an_rect_opts *opts) {
    const an_rect_opts o = opts ? *opts : (an_rect_opts){ 0 };
    if (!s) return NULL;
    const an_vec2 size = (o.size.x > 0 || o.size.y > 0) ? o.size : (an_vec2){ 100, 100 };
    ani_path *path = ani_path_rect(&s->arena, size);
    if (!path) return NULL;
    an_obj *r = with_path(ani_obj_make(s, AN_KIND_RECT, ANI_COMMON(&o)), path, ani_outline_requested(o.stroke, o.stroke_width));
    if (r) r->u.rect.size = size;
    return r;
}

an_obj *an_circle(an_scene *s, const an_circle_opts *opts) {
    const an_circle_opts o = opts ? *opts : (an_circle_opts){ 0 };
    if (!s) return NULL;
    const float radius = o.radius > 0 ? o.radius : 50.0f;
    ani_path *path = ani_path_circle(&s->arena, radius, s->flatness);
    if (!path) return NULL;
    an_obj *c = with_path(ani_obj_make(s, AN_KIND_CIRCLE, ANI_COMMON(&o)), path, ani_outline_requested(o.stroke, o.stroke_width));
    if (c) c->u.circle.radius = radius;
    return c;
}

an_obj *an_line(an_scene *s, const an_line_opts *opts) {
    const an_line_opts o = opts ? *opts : (an_line_opts){ 0 };
    if (!s) return NULL;
    ani_path *path = ani_path_line(&s->arena, o.from, o.to);
    if (!path) return NULL;
    an_obj *l = with_path(ani_obj_make(s, AN_KIND_LINE, ANI_COMMON(&o)), path, true);
    if (l) {
        l->u.line.from = o.from;
        l->u.line.to = o.to;
    }
    return l;
}

an_obj *an_arrow(an_scene *s, const an_arrow_opts *opts) {
    const an_arrow_opts o = opts ? *opts : (an_arrow_opts){ 0 };
    if (!s) return NULL;
    const float width = o.stroke_width > 0.0f ? o.stroke_width : 4.0f;
    float head = o.head > 0.0f ? o.head : 4.0f * width;
    if (head < 16.0f && !(o.head > 0.0f)) head = 16.0f;
    ani_path *path = ani_path_arrow(&s->arena, o.from, o.to, head);
    if (!path) return NULL;
    an_obj *a = with_path(ani_obj_make(s, AN_KIND_ARROW, ANI_COMMON(&o)), path, true);
    if (a) {
        a->u.line.from = o.from;
        a->u.line.to = o.to;
    }
    return a;
}

an_obj *an_stroke(an_scene *s, const an_stroke_opts *opts) {
    const an_stroke_opts o = opts ? *opts : (an_stroke_opts){ 0 };
    if (!s) return NULL;
    ani_path *path = ani_path_segs(&s->arena, o.segs, o.segs ? o.seg_count : 0, s->flatness);
    if (!path) return NULL;
    return with_path(ani_obj_make(s, AN_KIND_STROKE, ANI_COMMON(&o)), path, true);
}

an_obj *an_group(an_scene *s, const an_group_opts *opts) {
    const an_group_opts o = opts ? *opts : (an_group_opts){ 0 };
    return ani_obj_make(s, AN_KIND_GROUP, ANI_COMMON(&o));
}

// ---- getters (setters live in timeline.c) ---------------------------------

an_kind an_obj_kind(const an_obj *o) { return o->kind; }

an_rng *an_scene_rng(an_scene *s) { return &s->rng; }
an_rng *an_obj_rng(an_obj *o) { return &o->rng; }

an_obj *an_obj_parent(const an_obj *o) {
    if (!o || !o->parent || o->parent == &o->scene->root) return NULL;
    return o->parent;
}

an_obj *an_polygon(an_scene *s, const an_polygon_opts *opts) {
    const an_polygon_opts o = opts ? *opts : (an_polygon_opts){ 0 };
    if (!s || !o.points || o.count < 3) return NULL;
    ani_path_builder b;
    ani_pb_init(&b);
    bool ok = ani_pb_move(&b, o.points[0]);
    for (int k = 1; ok && k < o.count; k++) ok = ani_pb_line(&b, o.points[k]);
    ani_pb_close(&b);
    ani_path *path = ok ? ani_pb_finish(&b, &s->arena) : NULL;
    ani_pb_free(&b);
    if (!path) return NULL;
    // with_path also puts the verb pivot at the center of the points' box
    return with_path(ani_obj_make(s, AN_KIND_POLYGON, ANI_COMMON(&o)), path, ani_outline_requested(o.stroke, o.stroke_width));
}

void ani_obj_part(an_obj *o, const char *part, int index) {
    if (!o || o->name || !o->parent) return; // a user's name wins
    o->rng_key = ani_rng_part_key(o->parent->rng_key, part, (uint64_t)index);
    o->rng = an_rng_make(o->scene->seed, o->rng_key);
}
