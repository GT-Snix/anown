#include "tess_internal.h"

#include <math.h>
#include <stdlib.h>

#define ANI_PI 3.14159265358979323846f

void ani_mesh_init(ani_mesh *m) {
    *m = (ani_mesh){ 0 };
}

void ani_mesh_free(ani_mesh *m) {
    free(m->v);
    free(m->scratch);
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

ani_affine ani_obj_local(const an_obj *o) {
    const float cs = cosf(o->rotation), sn = sinf(o->rotation);
    return (ani_affine){
        .a = cs * o->scale.x, .b = sn * o->scale.x,
        .c = -sn * o->scale.y, .d = cs * o->scale.y,
        .tx = o->pos.x, .ty = o->pos.y,
    };
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

static bool tri(ani_mesh *m, an_vec2 p0, an_vec2 p1, an_vec2 p2, an_color c) {
    if (!grow((void **)&m->v, &m->cap, m->count + 3, sizeof(ani_vertex))) return false;
    const an_vec2 ps[3] = { p0, p1, p2 };
    for (int i = 0; i < 3; i++) {
        m->v[m->count++] = (ani_vertex){ ps[i].x, ps[i].y, c.r, c.g, c.b, c.a };
    }
    return true;
}

static bool quad(ani_mesh *m, an_vec2 p0, an_vec2 p1, an_vec2 p2, an_vec2 p3, an_color c) {
    return tri(m, p0, p1, p2, c) && tri(m, p0, p2, p3, c);
}

int ani_circle_segments(float radius_px) {
    // roughly one segment per 4 px of circumference, clamped
    const float n = ceilf(2.0f * ANI_PI * fabsf(radius_px) / 4.0f);
    if (n < 24.0f) return 24;
    if (n > 512.0f) return 512;
    return (int)n;
}

static float max_axis_scale(ani_affine w) {
    const float sx = sqrtf(w.a * w.a + w.b * w.b);
    const float sy = sqrtf(w.c * w.c + w.d * w.d);
    return sx > sy ? sx : sy;
}

static bool emit(ani_mesh *m, const an_obj *o, ani_affine w) {
    switch (o->kind) {
    case AN_KIND_RECT: {
        const float hx = o->u.rect.size.x * 0.5f, hy = o->u.rect.size.y * 0.5f;
        return quad(m, ani_affine_apply(w, (an_vec2){ -hx, -hy }),
                    ani_affine_apply(w, (an_vec2){ hx, -hy }),
                    ani_affine_apply(w, (an_vec2){ hx, hy }),
                    ani_affine_apply(w, (an_vec2){ -hx, hy }), o->fill);
    }
    case AN_KIND_CIRCLE: {
        const float r = o->u.circle.radius;
        const int n = ani_circle_segments(r * max_axis_scale(w));
        const an_vec2 center = ani_affine_apply(w, (an_vec2){ 0, 0 });
        an_vec2 prev = ani_affine_apply(w, (an_vec2){ r, 0 });
        for (int i = 1; i <= n; i++) {
            const float t = 2.0f * ANI_PI * (float)i / (float)n;
            const an_vec2 cur = (i == n) ? ani_affine_apply(w, (an_vec2){ r, 0 })
                                         : ani_affine_apply(w, (an_vec2){ r * cosf(t), r * sinf(t) });
            if (!tri(m, center, prev, cur, o->fill)) return false;
            prev = cur;
        }
        return true;
    }
    case AN_KIND_LINE: {
        // endpoints transformed to world, then a butt-capped quad of the stroke
        // width (scaled by the object's overall scale) around the segment
        const an_vec2 a = ani_affine_apply(w, o->u.line.from);
        const an_vec2 b = ani_affine_apply(w, o->u.line.to);
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len = sqrtf(dx * dx + dy * dy);
        if (len <= 0.0f) return true;
        const float half = 0.5f * o->stroke_width * sqrtf(fabsf(w.a * w.d - w.b * w.c));
        const float nx = -dy / len * half, ny = dx / len * half;
        return quad(m, (an_vec2){ a.x + nx, a.y + ny }, (an_vec2){ b.x + nx, b.y + ny },
                    (an_vec2){ b.x - nx, b.y - ny }, (an_vec2){ a.x - nx, a.y - ny }, o->stroke);
    }
    case AN_KIND_TEXT:  // stub: no font rendering yet
    case AN_KIND_GROUP: // no geometry of its own
        return true;
    }
    return true;
}

// Sibling order: z ascending, then creation id. Ids are unique, so this is a
// total order and qsort's instability doesn't matter.
static int cmp_draw(const void *pa, const void *pb) {
    const an_obj *a = *(an_obj *const *)pa, *b = *(an_obj *const *)pb;
    if (a->z != b->z) return a->z < b->z ? -1 : 1;
    return a->id < b->id ? -1 : (a->id > b->id);
}

static bool visit_children(ani_mesh *m, const an_obj *parent, ani_affine pw) {
    // push this node's children onto the scratch stack, sort that slice
    const int base = m->scratch_count;
    for (an_obj *c = parent->first_child; c; c = c->next_sibling) {
        if (!grow((void **)&m->scratch, &m->scratch_cap, m->scratch_count + 1, sizeof(an_obj *)))
            return false;
        m->scratch[m->scratch_count++] = c;
    }
    const int n = m->scratch_count - base;
    qsort(m->scratch + base, (size_t)n, sizeof(an_obj *), cmp_draw);

    for (int i = 0; i < n; i++) {
        const an_obj *c = m->scratch[base + i]; // re-read: scratch may have moved
        const ani_affine w = ani_affine_mul(pw, ani_obj_local(c));
        if (!emit(m, c, w)) return false;
        if (c->first_child && !visit_children(m, c, w)) return false;
    }
    m->scratch_count = base;
    return true;
}

bool ani_tess_scene(ani_mesh *m, const an_scene *s) {
    m->count = 0;
    m->scratch_count = 0;
    const float w = (float)s->width, h = (float)s->height;
    if (!quad(m, (an_vec2){ 0, 0 }, (an_vec2){ w, 0 }, (an_vec2){ w, h }, (an_vec2){ 0, h },
              s->background))
        return false;
    return visit_children(m, &s->root, (ani_affine){ 1, 0, 0, 1, 0, 0 });
}
