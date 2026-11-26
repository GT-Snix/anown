// The plane (docs/plane.md): content in plane units mapped through a 2x2 matrix
// every frame, before stroking. All mapping goes through ani_map, so a later
// 3x3-plus-projection kind reuses the grid, vectors, lines and clipping.
#include "plane_internal.h"
#include "scene_internal.h"
#include "tess_internal.h"
#include "timeline_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

bool ani_plane_map_off = false; // tests: the identity gate maps p -> (unit x, -unit y) directly

// ---- the map -------------------------------------------------------------------------------

an_vec2 ani_map_point(const ani_map *m, an_vec2 p) {
    if (ani_plane_map_off) return (an_vec2){ m->unit * p.x, -(m->unit * p.y) };
    return (an_vec2){ m->unit * (m->m[0] * p.x + m->m[1] * p.y), -(m->unit * (m->m[2] * p.x + m->m[3] * p.y)) };
}

an_vec2 ani_map_tangent(const ani_map *m, an_vec2 p, an_vec2 d) {
    (void)p; // linear: the Jacobian is M everywhere
    if (ani_plane_map_off) return (an_vec2){ m->unit * d.x, -(m->unit * d.y) };
    return (an_vec2){ m->unit * (m->m[0] * d.x + m->m[1] * d.y), -(m->unit * (m->m[2] * d.x + m->m[3] * d.y)) };
}

// plane-local px -> plane units; false if M is (nearly) singular
bool ani_map_inverse(const ani_map *m, an_vec2 q, an_vec2 *p) {
    const double a = m->m[0], b = m->m[1], c = m->m[2], d = m->m[3], det = a * d - b * c;
    if (!(fabs(det) >= 1e-6)) return false;
    const double x = q.x / m->unit, y = -q.y / m->unit;
    *p = (an_vec2){ (float)((d * x - b * y) / det), (float)((-c * x + a * y) / det) };
    return true;
}

ani_map ani_plane_map_at(const an_obj *plane, ani_time at) {
    const ani_value v = ani_sample(ani_obj_prop((an_obj *)plane, AN_PROP_MATRIX), at);
    ani_map m = { .kind = ANI_MAP_LINEAR2, .unit = plane->u.plane.unit };
    for (int k = 0; k < 4; k++) m.m[k] = (float)v.v[k];
    return m;
}

bool ani_plane_mapped_kind(const an_obj *o) {
    return o->kind == AN_KIND_PLANE_GRID || o->kind == AN_KIND_PLANE_LINE || o->kind == AN_KIND_PLANE_VECTOR;
}

// ---- clipping (Liang-Barsky) ---------------------------------------------------------------

bool ani_clip_seg(an_vec2 *a, an_vec2 *b, an_vec2 lo, an_vec2 hi) {
    const float dx = b->x - a->x, dy = b->y - a->y;
    const float p[4] = { -dx, dx, -dy, dy }, q[4] = { a->x - lo.x, hi.x - a->x, a->y - lo.y, hi.y - a->y };
    float t0 = 0.0f, t1 = 1.0f;
    for (int k = 0; k < 4; k++) {
        if (p[k] == 0.0f) {
            if (q[k] < 0.0f) return false;
            continue;
        }
        const float r = q[k] / p[k];
        if (p[k] < 0.0f) {
            if (r > t1) return false;
            if (r > t0) t0 = r;
        } else {
            if (r < t0) return false;
            if (r < t1) t1 = r;
        }
    }
    const an_vec2 a0 = *a;
    if (t1 < 1.0f) *b = (an_vec2){ a0.x + dx * t1, a0.y + dy * t1 };
    if (t0 > 0.0f) *a = (an_vec2){ a0.x + dx * t0, a0.y + dy * t0 };
    return true;
}

// ---- geometry per frame --------------------------------------------------------------------

int ani_plane_grid(const an_obj *grid, const ani_map *m, an_vec2 lo, an_vec2 hi, ani_grid_seg *out, int cap) {
    const float step = grid->parent->u.plane.grid_step;
    if (!(step > 0.0f)) return 0;
    // the frame's range at the identity (plane units), and 4x that: the most the grid covers
    const float ix0 = lo.x / m->unit, ix1 = hi.x / m->unit, iy0 = -hi.y / m->unit, iy1 = -lo.y / m->unit;
    const float cx = (ix0 + ix1) * 0.5f, cy = (iy0 + iy1) * 0.5f, hx = (ix1 - ix0) * 2.0f, hy = (iy1 - iy0) * 2.0f;
    float x0 = cx - hx, x1 = cx + hx, y0 = cy - hy, y1 = cy + hy;
    // the frame's preimage through M, when M is invertible, clamped to that
    an_vec2 c[4];
    const an_vec2 q[4] = { lo, { hi.x, lo.y }, hi, { lo.x, hi.y } };
    bool inv = true;
    for (int k = 0; k < 4 && inv; k++) inv = ani_map_inverse(m, q[k], &c[k]);
    if (inv) {
        float a0 = c[0].x, a1 = c[0].x, b0 = c[0].y, b1 = c[0].y;
        for (int k = 1; k < 4; k++) {
            a0 = fminf(a0, c[k].x);
            a1 = fmaxf(a1, c[k].x);
            b0 = fminf(b0, c[k].y);
            b1 = fmaxf(b1, c[k].y);
        }
        x0 = fmaxf(x0, a0);
        x1 = fminf(x1, a1);
        y0 = fmaxf(y0, b0);
        y1 = fminf(y1, b1);
    }
    const int kx0 = (int)ceilf(x0 / step), kx1 = (int)floorf(x1 / step);
    const int ky0 = (int)ceilf(y0 / step), ky1 = (int)floorf(y1 / step);
    const int total = (kx1 >= kx0 ? kx1 - kx0 + 1 : 0) + (ky1 >= ky0 ? ky1 - ky0 + 1 : 0);
    const int stride = total > ANI_PLANE_MAX_LINES ? (total + ANI_PLANE_MAX_LINES - 1) / ANI_PLANE_MAX_LINES : 1;
    int n = 0;
    for (int axis = 0; axis < 2; axis++) {
        const int k0 = axis ? ky0 : kx0, k1 = axis ? ky1 : kx1;
        for (int k = k0; k <= k1 && n < cap; k++) {
            if (k % stride != 0) continue;
            const float v = (float)k * step;
            an_vec2 a = ani_map_point(m, axis ? (an_vec2){ x0, v } : (an_vec2){ v, y0 });
            an_vec2 b = ani_map_point(m, axis ? (an_vec2){ x1, v } : (an_vec2){ v, y1 });
            if (!ani_clip_seg(&a, &b, lo, hi)) continue;
            out[n++] = (ani_grid_seg){ axis, k, a, b };
        }
    }
    return n;
}

bool ani_plane_line_seg(const an_obj *line, const ani_map *m, an_vec2 lo, an_vec2 hi, an_vec2 *a, an_vec2 *b) {
    const an_vec2 P = ani_map_point(m, line->u.pmark.p), D = ani_map_tangent(m, line->u.pmark.p, line->u.pmark.dir);
    return ani_line2_seg(P, D, m->unit, lo, hi, a, b);
}

bool ani_line2_seg(an_vec2 P, an_vec2 D, float unit, an_vec2 lo, an_vec2 hi, an_vec2 *a, an_vec2 *b) {
    const float dl = sqrtf(D.x * D.x + D.y * D.y);
    if (!(dl > 1e-6f * unit)) { // crushed: a dot at P
        *a = P;
        *b = (an_vec2){ P.x + 0.01f, P.y };
        return P.x >= lo.x && P.x <= hi.x && P.y >= lo.y && P.y <= hi.y;
    }
    const float L = hypotf(hi.x - lo.x, hi.y - lo.y) + hypotf(P.x - (lo.x + hi.x) * 0.5f, P.y - (lo.y + hi.y) * 0.5f);
    *a = (an_vec2){ P.x - D.x / dl * L, P.y - D.y / dl * L };
    *b = (an_vec2){ P.x + D.x / dl * L, P.y + D.y / dl * L };
    return ani_clip_seg(a, b, lo, hi);
}

// v: the vector's pos turned clockwise (on screen; y is up in the plane) by its
// rotation and scaled by its scale, all sampled at `at`
static an_vec2 vector_v(an_obj *vec, ani_time at) {
    const ani_value p = ani_sample(ani_obj_prop(vec, AN_PROP_POS), at);
    const ani_value sc = ani_sample(ani_obj_prop(vec, AN_PROP_SCALE), at);
    const double r = ani_sample(ani_obj_prop(vec, AN_PROP_ROTATION), at).v[0];
    const double x = p.v[0] * sc.v[0], y = p.v[1] * sc.v[1];
    if (r == 0.0) return (an_vec2){ (float)x, (float)y };
    const double cr = cos(r), sr = sin(r);
    return (an_vec2){ (float)(x * cr + y * sr), (float)(-x * sr + y * cr) };
}

void ani_plane_vector_geom(an_obj *vec, const ani_map *m, ani_time at, an_vec2 *tip, an_vec2 *dir, float *head) {
    const an_vec2 v = vector_v(vec, at);
    *tip = ani_map_point(m, v);
    const float len = sqrtf(tip->x * tip->x + tip->y * tip->y);
    if (len > 1e-6f) *dir = (an_vec2){ tip->x / len, tip->y / len };
    else { // crushed: v's own direction on screen, else +x (no state between frames)
        const float vl = sqrtf(v.x * v.x + v.y * v.y);
        *dir = vl > 0.0f ? (an_vec2){ v.x / vl, -v.y / vl } : (an_vec2){ 1, 0 };
    }
    *head = len < ANI_PLANE_HEAD ? len / ANI_PLANE_HEAD : 1.0f; // the head shrinks to fit a short arrow
}

// ---- public ----------------------------------------------------------------------------------

static ani_common_opts common_of(const an_scene *s, an_obj *parent, const ani_common_opts *c) {
    (void)s;
    ani_common_opts o = *c;
    o.parent = parent;
    return o;
}

an_obj *an_plane(an_scene *s, const an_plane_opts *opts) {
    const an_plane_opts o = opts ? *opts : (an_plane_opts){ 0 };
    if (!s) return NULL;
    const an_palette *pal = s->style.palette;
    ani_common_opts c = { .name = o.name, .hidden = o.hidden, .parent = o.parent, .pos = o.pos, .scale = o.scale,
                          .rotation = o.rotation, .z = o.z };
    if (c.pos.x == 0.0f && c.pos.y == 0.0f) c.pos = (an_vec2){ (float)s->width * 0.5f, (float)s->height * 0.5f };
    an_obj *p = ani_obj_make(s, AN_KIND_PLANE, &c);
    if (!p) return NULL;
    p->u.plane.m[0] = p->u.plane.m[3] = 1.0f;
    p->u.plane.unit = o.unit > 0.0f ? o.unit : 100.0f * (float)s->height / 1080.0f;
    p->u.plane.grid_step = o.grid_step < 0.0f ? 0.0f : o.grid_step > 0.0f ? o.grid_step : 1.0f;
    if (p->u.plane.grid_step > 0.0f) {
        const an_color ink = o.stroke.a > 0.0f ? o.stroke
                                               : (an_color){ pal->ink.r + (s->background.r - pal->ink.r) * 0.75f,
                                                             pal->ink.g + (s->background.g - pal->ink.g) * 0.75f,
                                                             pal->ink.b + (s->background.b - pal->ink.b) * 0.75f, 1 };
        an_obj *g = ani_obj_make(s, AN_KIND_PLANE_GRID, &(ani_common_opts){
                                     .parent = p, .z = -10, .stroke = ink,
                                     .stroke_width = o.stroke_width > 0.0f ? o.stroke_width : 2.0f,
                                     .shadow = AN_OFF, .riso = AN_OFF, .undrawn = o.undrawn });
        if (!g) return NULL;
        ani_obj_part(g, "grid", 0);
        g->has_stroke = true;
        g->chaos = o.grid_chaos > 0.0f ? o.grid_chaos : 0.3f;
    }
    if (o.axes) {
        for (int k = 0; k < 2; k++) {
            an_obj *a = an_plane_line(p, (an_vec2){ 0, 0 }, k ? (an_vec2){ 0, 1 } : (an_vec2){ 1, 0 },
                                      &(an_line_opts){ .z = -5, .stroke_width = 4, .stroke = pal->ink, .shadow = AN_OFF,
                                                       .undrawn = o.undrawn });
            if (!a) return NULL;
            ani_obj_part(a, "axis", k);
        }
    }
    return p;
}

static bool is_plane(const an_obj *p) { return p && p->kind == AN_KIND_PLANE; }

an_anim an_plane_matrix_to(an_obj *p, an_mat2 m, const an_anim_opts *opts) {
    an_anim a = { .obj = is_plane(p) ? p : NULL, .prop = AN_PROP_MATRIX, .value = { m.a, m.b, m.c, m.d } };
    if (opts) a.opts = *opts;
    return a;
}

void an_plane_set_matrix(an_obj *p, an_mat2 m) {
    if (is_plane(p)) ani_set(ani_obj_prop(p, AN_PROP_MATRIX), (ani_value){ { m.a, m.b, m.c, m.d } });
}

an_mat2 an_plane_matrix_at_cursor(const an_obj *p) {
    if (!is_plane(p)) return AN_MAT2_IDENTITY;
    const ani_value v = ani_sample(ani_obj_prop((an_obj *)p, AN_PROP_MATRIX), ani_tick_time(p->scene->cursor));
    return (an_mat2){ (float)v.v[0], (float)v.v[1], (float)v.v[2], (float)v.v[3] };
}

an_vec2 an_plane_point_at_cursor(const an_obj *p, an_vec2 q) {
    if (!is_plane(p)) return (an_vec2){ 0, 0 };
    const ani_time at = ani_tick_time(p->scene->cursor);
    const ani_map m = ani_plane_map_at(p, at);
    return ani_affine_apply(ani_obj_world_at((an_obj *)p, at), ani_map_point(&m, q));
}

an_obj *an_plane_vector(an_obj *p, an_vec2 v, const an_arrow_opts *opts) {
    if (!is_plane(p)) return NULL;
    an_scene *s = p->scene;
    const an_arrow_opts o = opts ? *opts : (an_arrow_opts){ 0 };
    ani_common_opts c = common_of(s, p, ANI_COMMON(&o));
    c.pos = v;
    c.stroke = o.stroke.a > 0.0f ? o.stroke : s->style.palette->ink;
    c.stroke_width = o.stroke_width > 0.0f ? o.stroke_width : 5.0f;
    an_obj *a = ani_obj_make(s, AN_KIND_PLANE_VECTOR, &c);
    if (!a) return NULL;
    a->has_stroke = true;
    an_obj *h = an_polygon(s, &(an_polygon_opts){
        .parent = a, .fill = c.stroke, .undrawn = o.undrawn, .shadow = o.shadow, .riso = o.riso,
        .points = (an_vec2[]){ { 0, 0 }, { -ANI_PLANE_HEAD, -8 }, { -ANI_PLANE_HEAD, 8 } }, .count = 3 });
    if (!h) return NULL;
    ani_obj_part(h, "head", 0);
    a->u.pmark.head = h;
    return a;
}

an_obj *an_plane_line(an_obj *p, an_vec2 at, an_vec2 dir, const an_line_opts *opts) {
    if (!is_plane(p)) return NULL;
    an_scene *s = p->scene;
    const an_line_opts o = opts ? *opts : (an_line_opts){ 0 };
    ani_common_opts c = common_of(s, p, ANI_COMMON(&o));
    c.pos = (an_vec2){ 0, 0 };
    c.stroke = o.stroke.a > 0.0f ? o.stroke : s->style.palette->ink;
    c.stroke_width = o.stroke_width > 0.0f ? o.stroke_width : 3.0f;
    an_obj *l = ani_obj_make(s, AN_KIND_PLANE_LINE, &c);
    if (!l) return NULL;
    l->has_stroke = true;
    l->u.pmark.p = at;
    l->u.pmark.dir = dir;
    return l;
}

an_obj *an_plane_dot(an_obj *p, an_vec2 at, const an_circle_opts *opts) {
    if (!is_plane(p)) return NULL;
    an_circle_opts o = opts ? *opts : (an_circle_opts){ 0 };
    o.parent = p;
    o.pos = at;
    if (!(o.radius > 0.0f)) o.radius = 7.0f;
    return an_circle(p->scene, &o);
}
