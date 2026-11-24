#include "scene_internal.h"

#include <stdlib.h>

// ---- scene ------------------------------------------------------------------

an_scene *an_scene_create(const an_scene_desc *desc) {
    const an_scene_desc d = desc ? *desc : (an_scene_desc){ 0 };
    an_scene *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->width = d.width > 0 ? d.width : 1920;
    s->height = d.height > 0 ? d.height : 1080;
    s->fps = d.fps > 0 ? d.fps : 60;
    s->seed = d.seed;
    s->background = d.background.a > 0.0f ? d.background : AN_CREAM;
    ani_arena_init(&s->arena, 0);
    s->root.kind = AN_KIND_GROUP;
    s->root.scene = s;
    s->root.scale = (an_vec2){ 1, 1 };
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

typedef struct common_opts {
    AN_OBJ_FIELDS
} common_opts;

static an_obj *make(an_scene *s, an_kind kind, const common_opts *c) {
    if (!s) return NULL;
    an_obj *p = resolve_parent(s, c->parent);
    if (!p) return NULL;
    an_obj *o = ANI_NEW(&s->arena, an_obj);
    if (!o) return NULL;
    o->kind = kind;
    o->id = ++s->count;
    o->scene = s;
    o->pos = c->pos;
    o->scale = (c->scale.x == 0.0f && c->scale.y == 0.0f) ? (an_vec2){ 1, 1 } : c->scale;
    o->rotation = c->rotation;
    o->z = c->z;
    o->fill = c->fill.a > 0.0f ? c->fill : AN_BLACK;
    o->stroke = c->stroke.a > 0.0f ? c->stroke : AN_BLACK;
    o->stroke_width = c->stroke_width > 0.0f ? c->stroke_width : 4.0f;
    append_child(p, o);
    return o;
}

// Copy the shared leading fields of any *_opts struct (all begin with
// AN_OBJ_FIELDS, so they share a common initial sequence).
#define COMMON(o) (&(common_opts){                                        \
    .parent = (o)->parent, .pos = (o)->pos, .scale = (o)->scale,         \
    .rotation = (o)->rotation, .z = (o)->z, .fill = (o)->fill,           \
    .stroke = (o)->stroke, .stroke_width = (o)->stroke_width })

an_obj *an_rect(an_scene *s, const an_rect_opts *opts) {
    const an_rect_opts o = opts ? *opts : (an_rect_opts){ 0 };
    an_obj *r = make(s, AN_KIND_RECT, COMMON(&o));
    if (r) r->u.rect.size = (o.size.x > 0 || o.size.y > 0) ? o.size : (an_vec2){ 100, 100 };
    return r;
}

an_obj *an_circle(an_scene *s, const an_circle_opts *opts) {
    const an_circle_opts o = opts ? *opts : (an_circle_opts){ 0 };
    an_obj *c = make(s, AN_KIND_CIRCLE, COMMON(&o));
    if (c) c->u.circle.radius = o.radius > 0 ? o.radius : 50.0f;
    return c;
}

an_obj *an_line(an_scene *s, const an_line_opts *opts) {
    const an_line_opts o = opts ? *opts : (an_line_opts){ 0 };
    an_obj *l = make(s, AN_KIND_LINE, COMMON(&o));
    if (l) {
        l->u.line.from = o.from;
        l->u.line.to = o.to;
    }
    return l;
}

an_obj *an_text(an_scene *s, const an_text_opts *opts) {
    const an_text_opts o = opts ? *opts : (an_text_opts){ 0 };
    if (!s) return NULL;
    const char *str = ani_arena_strdup(&s->arena, o.text); // before make(): no half-built object
    if (!str) return NULL;
    an_obj *t = make(s, AN_KIND_TEXT, COMMON(&o));
    if (t) {
        t->u.text.str = str;
        t->u.text.size = o.size > 0 ? o.size : 48.0f;
    }
    return t;
}

an_obj *an_group(an_scene *s, const an_group_opts *opts) {
    const an_group_opts o = opts ? *opts : (an_group_opts){ 0 };
    return make(s, AN_KIND_GROUP, COMMON(&o));
}

// ---- setters / getters ------------------------------------------------------

void an_obj_set_pos(an_obj *o, an_vec2 pos) { if (o) o->pos = pos; }
void an_obj_set_scale(an_obj *o, an_vec2 scale) { if (o) o->scale = scale; }
void an_obj_set_rotation(an_obj *o, float radians) { if (o) o->rotation = radians; }
void an_obj_set_z(an_obj *o, int z) { if (o) o->z = z; }
void an_obj_set_fill(an_obj *o, an_color fill) { if (o) o->fill = fill; }

an_kind an_obj_kind(const an_obj *o) { return o->kind; }

an_obj *an_obj_parent(const an_obj *o) {
    if (!o || !o->parent || o->parent == &o->scene->root) return NULL;
    return o->parent;
}
