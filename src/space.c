// The 3D space (docs/space.md): content in space units mapped through a 3x3
// matrix and projected by an orbit camera every frame, then stroked like the
// plane's (plane.c does the 2D clipping and the infinite 2D line). Rotations and
// the projection are computed in double; angles are reduced modulo 2 pi.
#include "space_internal.h"
#include "plane_internal.h"
#include "scene_internal.h"
#include "tess_internal.h"
#include "timeline_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const double TWO_PI = 6.28318530717958647692;

// ---- the view ------------------------------------------------------------------------------

static double reduce(double a) {
    const double r = fmod(a, TWO_PI);
    return r == 0.0 ? 0.0 : r; // fmod keeps the sign; -0 -> +0
}

// R = rotation by a (reduced) about the unit axis u, Rodrigues in double. The diagonal
// is written 1 - t (1 - u_i^2), so a rotation about z keeps (0, 0, 1) exactly.
static void rodrigues(const double u[3], double angle, double R[9]) {
    const double a = reduce(angle), c = cos(a), s = sin(a), t = 1.0 - c;
    const double x = u[0], y = u[1], z = u[2];
    R[0] = 1.0 - t * (1.0 - x * x);
    R[1] = t * x * y - s * z;
    R[2] = t * x * z + s * y;
    R[3] = t * x * y + s * z;
    R[4] = 1.0 - t * (1.0 - y * y);
    R[5] = t * y * z - s * x;
    R[6] = t * x * z - s * y;
    R[7] = t * y * z + s * x;
    R[8] = 1.0 - t * (1.0 - z * z);
}

static ani_view root_view(const an_obj *sp, ani_time at);

ani_view ani_space_view_at(const an_obj *c, ani_time at) {
    if (c->kind != AN_KIND_SPACE_FRAME) return root_view(c, at);
    // a frame: its parent's view, composed with p -> origin + F R p
    ani_view v = ani_space_view_at(c->parent, at);
    an_obj *o = (an_obj *)c;
    double F[9], R[9], L[9];
    for (int r = 0; r < 3; r++) {
        const ani_value row = ani_sample(ani_obj_prop(o, (an_prop)(AN_PROP_M3_ROW0 + r)), at);
        for (int k = 0; k < 3; k++) F[3 * r + k] = (double)(float)row.v[k];
    }
    const ani_value org = ani_sample(ani_obj_prop(o, AN_PROP_FRAME_ORIGIN), at);
    const double spin = ani_sample(ani_obj_prop(o, AN_PROP_FRAME_SPIN), at).v[0];
    const double ax[3] = { c->u.frame.axis.x, c->u.frame.axis.y, c->u.frame.axis.z };
    rodrigues(ax, spin, R);
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) L[3 * i + k] = F[3 * i] * R[k] + F[3 * i + 1] * R[3 + k] + F[3 * i + 2] * R[6 + k];
    const double t[3] = { (float)org.v[0], (float)org.v[1], (float)org.v[2] };
    double C[9], off[3];
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) C[3 * i + k] = v.C[3 * i] * L[k] + v.C[3 * i + 1] * L[3 + k] + v.C[3 * i + 2] * L[6 + k];
        off[i] = v.C[3 * i] * t[0] + v.C[3 * i + 1] * t[1] + v.C[3 * i + 2] * t[2] + v.o[i];
    }
    memcpy(v.C, C, sizeof C);
    memcpy(v.o, off, sizeof off);
    return v;
}

static ani_view root_view(const an_obj *sp, ani_time at) {
    an_obj *o = (an_obj *)sp; // sampling only reads
    double M[9];
    for (int r = 0; r < 3; r++) {
        const ani_value row = ani_sample(ani_obj_prop(o, (an_prop)(AN_PROP_M3_ROW0 + r)), at);
        for (int k = 0; k < 3; k++) M[3 * r + k] = (double)(float)row.v[k];
    }
    const ani_value orb = ani_sample(ani_obj_prop(o, AN_PROP_SPACE_ORBIT), at);
    const ani_value tg = ani_sample(ani_obj_prop(o, AN_PROP_SPACE_TARGET), at);
    const double D = ani_sample(ani_obj_prop(o, AN_PROP_SPACE_DISTANCE), at).v[0];
    const double y = reduce(orb.v[0]), t = reduce(orb.v[1]);
    const double sy = sin(y), cy = cos(y), st = sin(t), ct = cos(t);
    // the camera's frame: right r, forward f (toward the target), up u = r x f
    const double r[3] = { cy, sy, 0.0 };
    const double f[3] = { -st * sy, st * cy, -ct };
    const double u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
    const double *R[3] = { r, u, f };
    ani_view v = { .D = D, .unit = sp->u.space.unit, .near = 0.05 * D, .ortho = sp->u.space.ortho };
    const double T[3] = { (float)tg.v[0], (float)tg.v[1], (float)tg.v[2] };
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) v.C[3 * i + k] = R[i][0] * M[k] + R[i][1] * M[3 + k] + R[i][2] * M[6 + k];
        v.o[i] = -(R[i][0] * T[0] + R[i][1] * T[1] + R[i][2] * T[2]);
    }
    return v;
}

ani_cam3 ani_view_cam(const ani_view *v, const double p[3]) {
    const double x = v->C[0] * p[0] + v->C[1] * p[1] + v->C[2] * p[2] + v->o[0];
    const double y = v->C[3] * p[0] + v->C[4] * p[1] + v->C[5] * p[2] + v->o[1];
    const double z = v->C[6] * p[0] + v->C[7] * p[1] + v->C[8] * p[2] + v->o[2];
    return (ani_cam3){ x, y, v->D + z };
}

bool ani_view_visible(const ani_view *v, ani_cam3 c) { return v->ortho || c.depth >= v->near; }

an_vec2 ani_view_px(const ani_view *v, ani_cam3 c) {
    double depth = c.depth;
    if (!v->ortho && !(depth >= v->near)) depth = v->near; // callers cut first; never divide by <= 0
    const double k = v->ortho ? v->unit : v->unit * (v->D / depth);
    return (an_vec2){ (float)(k * c.x) + 0.0f, (float)(-(k * c.y)) + 0.0f };
}

bool ani_view_cut(const ani_view *v, ani_cam3 *a, ani_cam3 *b) {
    if (v->ortho) return true;
    const bool ia = a->depth >= v->near, ib = b->depth >= v->near;
    if (ia && ib) return true;
    if (!ia && !ib) return false;
    const double t = (v->near - a->depth) / (b->depth - a->depth); // denominators differ in sign: never 0
    const ani_cam3 q = { a->x + (b->x - a->x) * t, a->y + (b->y - a->y) * t, v->near };
    if (ia) *b = q;
    else *a = q;
    return true;
}

bool ani_space_mapped_kind(const an_obj *o) {
    return o->kind == AN_KIND_SPACE_GRID || o->kind == AN_KIND_SPACE_LINE || o->kind == AN_KIND_SPACE_VECTOR ||
           o->kind == AN_KIND_SPACE_PATH || o->kind == AN_KIND_SPACE_SPHERE;
}

const an_obj *ani_space_root(const an_obj *o) {
    while (o && o->kind == AN_KIND_SPACE_FRAME) o = o->parent;
    return o;
}

static const an_obj *container_root(const an_obj *o) { return ani_space_root(o); }

void ani_space_pos3(an_obj *o, ani_time at, double p[3]) {
    const ani_value pos = ani_sample(ani_obj_prop(o, AN_PROP_POS), at);
    p[0] = (float)pos.v[0];
    p[1] = (float)pos.v[1];
    p[2] = (float)ani_sample(ani_obj_prop(o, AN_PROP_POS_Z), at).v[0];
}

// ---- geometry per frame --------------------------------------------------------------------

// a 3D segment, cut, projected and clipped; false if nothing is left
static bool seg3(const ani_view *v, const double a3[3], const double b3[3], an_vec2 lo, an_vec2 hi, an_vec2 *a,
                 an_vec2 *b) {
    ani_cam3 ca = ani_view_cam(v, a3), cb = ani_view_cam(v, b3);
    if (!ani_view_cut(v, &ca, &cb)) return false;
    *a = ani_view_px(v, ca);
    *b = ani_view_px(v, cb);
    return ani_clip_seg(a, b, lo, hi);
}

int ani_space_grid(const an_obj *grid, const ani_view *v, an_vec2 lo, an_vec2 hi, ani_grid_seg *out, int cap) {
    const an_obj *sp = grid->parent;
    const double step = sp->u.space.grid_step, e = sp->u.space.grid_extent;
    if (!(step > 0.0) || !(e > 0.0)) return 0;
    const int kn = (int)floor(e / step + 1e-9);
    int n = 0;
    for (int axis = 0; axis < 2; axis++)
        for (int k = -kn; k <= kn && n < cap; k++) {
            const double c = (double)k * step;
            const double a3[3] = { axis ? -e : c, axis ? c : -e, 0.0 }, b3[3] = { axis ? e : c, axis ? c : e, 0.0 };
            an_vec2 a, b;
            if (!seg3(v, a3, b3, lo, hi, &a, &b)) continue;
            out[n++] = (ani_grid_seg){ axis, k, a, b };
        }
    return n;
}

bool ani_space_line_seg(const an_obj *line, const ani_view *v, an_vec2 lo, an_vec2 hi, an_vec2 *a, an_vec2 *b) {
    const an_vec3 p = line->u.smark.p, d = line->u.smark.dir;
    const double p3[3] = { p.x, p.y, p.z }, d3[3] = { d.x, d.y, d.z };
    const ani_cam3 cp = ani_view_cam(v, p3);
    const double cd[3] = { v->C[0] * d3[0] + v->C[1] * d3[1] + v->C[2] * d3[2],
                           v->C[3] * d3[0] + v->C[4] * d3[1] + v->C[5] * d3[2],
                           v->C[6] * d3[0] + v->C[7] * d3[1] + v->C[8] * d3[2] };
    const double len = sqrt(cd[0] * cd[0] + cd[1] * cd[1] + cd[2] * cd[2]);
    // parallel to the picture (or ortho): the projection along it is affine, so it's the
    // plane's 2D line through the projected p along the projected tangent
    if ((v->ortho || fabs(cd[2]) <= 1e-12 * len) && ani_view_visible(v, cp)) {
        const double k = v->ortho ? v->unit : v->unit * (v->D / cp.depth);
        const an_vec2 P = ani_view_px(v, cp), Dv = { (float)(k * cd[0]), (float)(-(k * cd[1])) };
        return ani_line2_seg(P, Dv, (float)v->unit, lo, hi, a, b);
    }
    if (!(len > 1e-12)) { // crushed: a dot at p
        if (!ani_view_visible(v, cp)) return false;
        *a = ani_view_px(v, cp);
        *b = (an_vec2){ a->x + 0.01f, a->y };
        return a->x >= lo.x && a->x <= hi.x && a->y >= lo.y && a->y <= hi.y;
    }
    // far enough that the image runs off the frame or to the vanishing point
    const double dl = sqrt(d3[0] * d3[0] + d3[1] * d3[1] + d3[2] * d3[2]);
    const double L = 4.0 * (v->D + container_root(line->parent)->u.space.grid_extent) +
                     sqrt(p3[0] * p3[0] + p3[1] * p3[1] + p3[2] * p3[2]);
    const double a3[3] = { p3[0] - d3[0] / dl * L, p3[1] - d3[1] / dl * L, p3[2] - d3[2] / dl * L };
    const double b3[3] = { p3[0] + d3[0] / dl * L, p3[1] + d3[1] / dl * L, p3[2] + d3[2] / dl * L };
    return seg3(v, a3, b3, lo, hi, a, b);
}

// v: pos and pos_z scaled by the scale (z by the mean of x and y), each a float as on the plane
static void vector_v(an_obj *vec, ani_time at, double v[3]) {
    const ani_value p = ani_sample(ani_obj_prop(vec, AN_PROP_POS), at);
    const ani_value sc = ani_sample(ani_obj_prop(vec, AN_PROP_SCALE), at);
    const double z = ani_sample(ani_obj_prop(vec, AN_PROP_POS_Z), at).v[0];
    v[0] = (float)(p.v[0] * sc.v[0]);
    v[1] = (float)(p.v[1] * sc.v[1]);
    v[2] = (float)(z * (sc.v[0] + sc.v[1]) * 0.5);
}

bool ani_space_vector_geom(an_obj *vec, const ani_view *v, ani_time at, an_vec2 *tail, an_vec2 *tip, an_vec2 *dir,
                           float *head, bool *has_tip) {
    double w[3];
    vector_v(vec, at, w);
    const double zero[3] = { 0, 0, 0 };
    ani_cam3 ca = ani_view_cam(v, zero), cb = ani_view_cam(v, w);
    *has_tip = ani_view_visible(v, cb);
    if (!ani_view_cut(v, &ca, &cb)) return false;
    *tail = ani_view_px(v, ca);
    *tip = ani_view_px(v, cb);
    const an_vec2 d = { tip->x - tail->x, tip->y - tail->y };
    const float len = sqrtf(d.x * d.x + d.y * d.y);
    if (len > 1e-6f) *dir = (an_vec2){ d.x / len, d.y / len };
    else { // pointing at the camera (or crushed): v's own direction on screen, else +x
        const float vl = sqrtf((float)(w[0] * w[0] + w[1] * w[1]));
        *dir = vl > 0.0f ? (an_vec2){ (float)w[0] / vl, -(float)w[1] / vl } : (an_vec2){ 1, 0 };
    }
    *head = !*has_tip ? 0.0f : len < ANI_PLANE_HEAD ? len / ANI_PLANE_HEAD : 1.0f;
    return true;
}

// ---- curves: cut at the near plane, subdivided on screen -----------------------------------

void ani_curve_at(const ani_curve *k, double t, double p[3]) {
    if (k->line) {
        for (int i = 0; i < 3; i++) p[i] = k->c[i] + t * k->a[i];
        return;
    }
    const double c = cos(t), s = sin(t);
    for (int i = 0; i < 3; i++) p[i] = k->c[i] + c * k->a[i] + s * k->b[i];
}

static void push(ani_pxbuf *b, an_vec2 p, unsigned char flag) {
    if (b->oom) return;
    if (b->n == b->cap) {
        const int cap = b->cap ? b->cap * 2 : 256;
        an_vec2 *np = realloc(b->p, sizeof(an_vec2) * (size_t)cap);
        if (np) b->p = np;
        unsigned char *ns = np ? realloc(b->st, (size_t)cap) : NULL;
        if (ns) b->st = ns;
        if (!np || !ns) {
            b->oom = true;
            return;
        }
        b->cap = cap;
    }
    b->p[b->n] = p;
    b->st[b->n++] = flag;
}

void ani_pxbuf_free(ani_pxbuf *b) {
    free(b->p);
    free(b->st);
    *b = (ani_pxbuf){ 0 };
}

typedef struct emitter {
    const ani_view *v;
    const ani_curve *k;
    ani_pxbuf *b;
    bool pen, broken;
} emitter;

static void seg_out(emitter *e, ani_cam3 ca, ani_cam3 cb) {
    const bool va = ani_view_visible(e->v, ca), vb = ani_view_visible(e->v, cb);
    if (!va || !vb) e->broken = true;
    if (!ani_view_cut(e->v, &ca, &cb)) {
        e->pen = false;
        return;
    }
    if (!e->pen) push(e->b, ani_view_px(e->v, ca), 1);
    push(e->b, ani_view_px(e->v, cb), 0);
    e->pen = vb;
}

// an edge whose ends land more than ANI_SPACE_SUBDIV px apart is split at its
// parameter midpoint, at most ANI_SPACE_SUBDIV_LEVELS times
static void refine(emitter *e, double ta, ani_cam3 ca, double tb, ani_cam3 cb, int level) {
    if (level < ANI_SPACE_SUBDIV_LEVELS && ani_view_visible(e->v, ca) && ani_view_visible(e->v, cb)) {
        const an_vec2 pa = ani_view_px(e->v, ca), pb = ani_view_px(e->v, cb);
        if (hypotf(pb.x - pa.x, pb.y - pa.y) > ANI_SPACE_SUBDIV) {
            const double tm = 0.5 * (ta + tb);
            double pm[3];
            ani_curve_at(e->k, tm, pm);
            const ani_cam3 cm = ani_view_cam(e->v, pm);
            refine(e, ta, ca, tm, cm, level + 1);
            refine(e, tm, cm, tb, cb, level + 1);
            return;
        }
    }
    seg_out(e, ca, cb);
}

static ani_cam3 cam_at(const ani_view *v, const ani_curve *k, double t) {
    double p[3];
    ani_curve_at(k, t, p);
    return ani_view_cam(v, p);
}

void ani_curve_px(const ani_view *v, const ani_curve *k, double t0, double t1, int nbase, bool closed, ani_pxbuf *b) {
    emitter e = { v, k, b, false, false };
    const int s0 = b->n;
    if (nbase < 1) nbase = 1;
    double ta = t0;
    ani_cam3 ca = cam_at(v, k, ta);
    for (int i = 1; i <= nbase; i++) {
        const double tb = i == nbase ? t1 : t0 + (t1 - t0) * (double)i / (double)nbase;
        const ani_cam3 cb = cam_at(v, k, tb);
        refine(&e, ta, ca, tb, cb, 0);
        ta = tb;
        ca = cb;
    }
    if (closed && !e.broken && b->n - s0 > 2 && !b->oom) { // one unbroken loop: its last point is its first
        b->st[s0] = 2;
        b->n--;
    }
}

void ani_space_path_runs(const an_obj *path, const ani_view *v, ani_pxbuf *b) {
    const an_vec3 *P = path->u.smark.pts;
    const int n = path->u.smark.npts, segs = path->u.smark.closed ? n : n - 1;
    ani_curve k = { .line = true };
    emitter e = { v, &k, b, false, false };
    const int s0 = b->n;
    for (int i = 0; i < segs; i++) {
        const an_vec3 a = P[i], c = P[(i + 1) % n];
        k.c[0] = a.x, k.c[1] = a.y, k.c[2] = a.z;
        k.a[0] = (double)c.x - a.x, k.a[1] = (double)c.y - a.y, k.a[2] = (double)c.z - a.z;
        refine(&e, 0.0, cam_at(v, &k, 0.0), 1.0, cam_at(v, &k, 1.0), 0);
    }
    if (path->u.smark.closed && !e.broken && b->n - s0 > 2 && !b->oom) {
        b->st[s0] = 2;
        b->n--;
    }
}

// ---- spheres -------------------------------------------------------------------------------

// The eye in the sphere's own space: the camera mapped back through the composed
// affine map (camera rows x M x every ancestor frame's F R). Front-facing is affine
// invariant there: (L^-T n) . (L w) = n . w. False: no culling (L singular, or the
// eye inside the sphere). *dir: ortho, e is the direction toward the viewer.
bool ani_sphere_eye(const an_obj *sph, const ani_view *v, double e[3], bool *dir) {
    const double *C = v->C;
    const double det = C[0] * (C[4] * C[8] - C[5] * C[7]) - C[1] * (C[3] * C[8] - C[5] * C[6]) + C[2] * (C[3] * C[7] - C[4] * C[6]);
    if (!(fabs(det) >= 1e-9)) return false;
    const double inv[9] = { (C[4] * C[8] - C[5] * C[7]) / det, (C[2] * C[7] - C[1] * C[8]) / det, (C[1] * C[5] - C[2] * C[4]) / det,
                            (C[5] * C[6] - C[3] * C[8]) / det, (C[0] * C[8] - C[2] * C[6]) / det, (C[2] * C[3] - C[0] * C[5]) / det,
                            (C[3] * C[7] - C[4] * C[6]) / det, (C[1] * C[6] - C[0] * C[7]) / det, (C[0] * C[4] - C[1] * C[3]) / det };
    // the eye in camera coordinates is (0, 0, -D) (depth = D + z); ortho: the direction (0, 0, -1)
    double q[3] = { 0, 0, -1 };
    if (!v->ortho) {
        q[0] = -v->o[0];
        q[1] = -v->o[1];
        q[2] = -v->D - v->o[2];
    }
    for (int i = 0; i < 3; i++) e[i] = inv[3 * i] * q[0] + inv[3 * i + 1] * q[1] + inv[3 * i + 2] * q[2];
    *dir = v->ortho;
    if (*dir) return true;
    const an_vec3 c = sph->u.sphere.center;
    const double dx = e[0] - c.x, dy = e[1] - c.y, dz = e[2] - c.z, r = sph->u.sphere.radius;
    return dx * dx + dy * dy + dz * dz > r * r;
}

static bool near_side(const an_obj *sph, const double e[3], bool dir, const double p[3]) {
    const an_vec3 c = sph->u.sphere.center;
    const double n[3] = { p[0] - c.x, p[1] - c.y, p[2] - c.z };
    if (dir) return n[0] * e[0] + n[1] * e[1] + n[2] * e[2] > 0.0;
    return n[0] * (e[0] - p[0]) + n[1] * (e[1] - p[1]) + n[2] * (e[2] - p[2]) > 0.0;
}

int ani_sphere_curves(const an_obj *sph) { return sph->u.sphere.meridians + sph->u.sphere.parallels; }

// curve i: meridians first (theta 0..pi from the north pole), then parallels (closed)
void ani_sphere_curve(const an_obj *sph, int i, ani_curve *k, double *t0, double *t1, int *nbase, bool *closed) {
    const an_vec3 c = sph->u.sphere.center;
    const double r = sph->u.sphere.radius, PI = 3.14159265358979323846;
    const int M = sph->u.sphere.meridians, P = sph->u.sphere.parallels;
    *k = (ani_curve){ .line = false };
    if (i < M) {
        const double phi = 2.0 * PI * (double)i / (double)M;
        *k = (ani_curve){ .c = { c.x, c.y, c.z }, .a = { 0, 0, r }, .b = { r * cos(phi), r * sin(phi), 0 } };
        *t0 = 0.0, *t1 = PI, *nbase = 32, *closed = false;
        return;
    }
    const double th = PI * (double)(i - M + 1) / (double)(P + 1), rs = r * sin(th);
    *k = (ani_curve){ .c = { c.x, c.y, c.z + r * cos(th) }, .a = { rs, 0, 0 }, .b = { 0, rs, 0 } };
    *t0 = 0.0, *t1 = 2.0 * PI, *nbase = 64, *closed = true;
}

// The spans of the wire, each wholly on the near or the far side. A change of side
// between two base samples is found by bisection (8 steps) on the curve's parameter.
int ani_sphere_spans(const an_obj *sph, const ani_view *v, ani_span *out, int cap) {
    double e[3];
    bool dir;
    const bool cull = ani_sphere_eye(sph, v, e, &dir);
    int n = 0;
    for (int i = 0; i < ani_sphere_curves(sph); i++) {
        ani_curve k;
        double t0, t1;
        int nb;
        bool closed;
        ani_sphere_curve(sph, i, &k, &t0, &t1, &nb, &closed);
        if (!cull) {
            if (n < cap) out[n++] = (ani_span){ i, t0, t1, true, closed };
            continue;
        }
        double split[64];
        int ns = 0;
        double p[3], ta = t0;
        ani_curve_at(&k, ta, p);
        bool sa = near_side(sph, e, dir, p);
        const bool s_first = sa;
        for (int j = 1; j <= nb && ns < 64; j++) {
            const double tb = j == nb ? t1 : t0 + (t1 - t0) * (double)j / (double)nb;
            ani_curve_at(&k, tb, p);
            const bool sb = near_side(sph, e, dir, p);
            if (sb != sa) {
                double lo = ta, hi = tb;
                for (int it = 0; it < 8; it++) {
                    const double mid = 0.5 * (lo + hi);
                    ani_curve_at(&k, mid, p);
                    if (near_side(sph, e, dir, p) == sa) lo = mid;
                    else hi = mid;
                }
                split[ns++] = 0.5 * (lo + hi);
            }
            ta = tb;
            sa = sb;
        }
        if (ns == 0) {
            if (n < cap) out[n++] = (ani_span){ i, t0, t1, s_first, closed };
            continue;
        }
        bool side = s_first;
        if (!closed) {
            double from = t0;
            for (int j = 0; j < ns && n < cap; j++) {
                out[n++] = (ani_span){ i, from, split[j], side, false };
                from = split[j];
                side = !side;
            }
            if (n < cap) out[n++] = (ani_span){ i, from, t1, side, false };
            continue;
        }
        // closed: the spans between splits, and the one wrapping past t1 to the first split
        side = !s_first; // after the first split
        for (int j = 0; j + 1 < ns && n < cap; j++) {
            out[n++] = (ani_span){ i, split[j], split[j + 1], side, false };
            side = !side;
        }
        if (n < cap) out[n++] = (ani_span){ i, split[ns - 1], split[0] + (t1 - t0), side, false };
    }
    return n;
}

// The exact silhouette: where the viewing cone touches the sphere, a circle with
// center c + (r^2/d^2)(e - c) and radius r sqrt(1 - r^2/d^2), in the plane
// perpendicular to e - c (ortho: the great circle facing the viewer). In the
// sphere's own space, so under a stretching frame it's the ellipsoid's exact outline.
bool ani_sphere_silhouette(const an_obj *sph, const ani_view *v, ani_curve *k) {
    double e[3];
    bool dir;
    if (!ani_sphere_eye(sph, v, e, &dir)) return false;
    const an_vec3 c = sph->u.sphere.center;
    const double r = sph->u.sphere.radius;
    double d[3] = { dir ? e[0] : e[0] - c.x, dir ? e[1] : e[1] - c.y, dir ? e[2] : e[2] - c.z };
    const double dl = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(dl > 0.0)) return false;
    const double nrm[3] = { d[0] / dl, d[1] / dl, d[2] / dl };
    const double f = dir ? 0.0 : r * r / (dl * dl), rho = dir ? r : r * sqrt(1.0 - f);
    // a basis of the circle's plane
    const double *axis = fabs(nrm[2]) < 0.9 ? (const double[3]){ 0, 0, 1 } : (const double[3]){ 1, 0, 0 };
    double e1[3] = { axis[1] * nrm[2] - axis[2] * nrm[1], axis[2] * nrm[0] - axis[0] * nrm[2], axis[0] * nrm[1] - axis[1] * nrm[0] };
    const double l1 = sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
    for (int i = 0; i < 3; i++) e1[i] /= l1;
    const double e2[3] = { nrm[1] * e1[2] - nrm[2] * e1[1], nrm[2] * e1[0] - nrm[0] * e1[2], nrm[0] * e1[1] - nrm[1] * e1[0] };
    *k = (ani_curve){ .line = false };
    for (int i = 0; i < 3; i++) {
        k->c[i] = (i == 0 ? c.x : i == 1 ? c.y : c.z) + f * d[i];
        k->a[i] = rho * e1[i];
        k->b[i] = rho * e2[i];
    }
    return true;
}

// ---- public ----------------------------------------------------------------------------------

an_mat3 an_mat3_rotation(an_vec3 axis, float radians) {
    const double l = sqrt((double)axis.x * axis.x + (double)axis.y * axis.y + (double)axis.z * axis.z);
    if (!(l > 0.0)) return AN_MAT3_IDENTITY;
    const double u[3] = { axis.x / l, axis.y / l, axis.z / l };
    double R[9];
    rodrigues(u, radians, R);
    an_mat3 m;
    for (int k = 0; k < 9; k++) m.m[k] = (float)R[k];
    return m;
}

an_mat3 an_mat3_scale(an_vec3 s) { return (an_mat3){ { s.x, 0, 0, 0, s.y, 0, 0, 0, s.z } }; }

static bool is_space(const an_obj *o) { return o && o->kind == AN_KIND_SPACE; }
static bool is_container(const an_obj *o) { return o && (o->kind == AN_KIND_SPACE || o->kind == AN_KIND_SPACE_FRAME); }

an_obj *an_space(an_scene *s, const an_space_opts *opts) {
    const an_space_opts o = opts ? *opts : (an_space_opts){ 0 };
    if (!s) return NULL;
    const an_palette *pal = s->style.palette;
    ani_common_opts c = { .name = o.name, .hidden = o.hidden, .parent = o.parent, .pos = o.pos, .scale = o.scale,
                          .rotation = o.rotation, .z = o.z };
    if (c.pos.x == 0.0f && c.pos.y == 0.0f) c.pos = (an_vec2){ (float)s->width * 0.5f, (float)s->height * 0.5f };
    an_obj *p = ani_obj_make(s, AN_KIND_SPACE, &c);
    if (!p) return NULL;
    memcpy(p->u.space.m, AN_MAT3_IDENTITY.m, sizeof p->u.space.m);
    p->u.space.yaw = o.orbit.yaw;
    p->u.space.tilt = o.orbit.tilt;
    p->u.space.distance = o.orbit.distance > 0.0f ? o.orbit.distance : 10.0f;
    p->u.space.target = o.target;
    p->u.space.unit = o.unit > 0.0f ? o.unit : 100.0f * (float)s->height / 1080.0f;
    p->u.space.grid_step = o.grid_step > 0.0f ? o.grid_step : 1.0f;
    p->u.space.grid_extent = o.grid_extent > 0.0f ? o.grid_extent : 12.0f;
    p->u.space.ortho = o.ortho;
    if (o.grid == AN_SPACE_GRID_XY) {
        const an_color ink = o.stroke.a > 0.0f ? o.stroke
                                               : (an_color){ pal->ink.r + (s->background.r - pal->ink.r) * 0.75f,
                                                             pal->ink.g + (s->background.g - pal->ink.g) * 0.75f,
                                                             pal->ink.b + (s->background.b - pal->ink.b) * 0.75f, 1 };
        an_obj *g = ani_obj_make(s, AN_KIND_SPACE_GRID, &(ani_common_opts){
                                     .parent = p, .z = -10, .stroke = ink,
                                     .stroke_width = o.stroke_width > 0.0f ? o.stroke_width : 2.0f,
                                     .shadow = AN_OFF, .riso = AN_OFF, .undrawn = o.undrawn });
        if (!g) return NULL;
        ani_obj_part(g, "grid", 0);
        g->has_stroke = true;
        g->chaos = o.grid_chaos > 0.0f ? o.grid_chaos : 0.3f;
    }
    if (o.axes) {
        static const an_vec3 dirs[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        for (int k = 0; k < 3; k++) {
            an_obj *a = an_space_line(p, (an_vec3){ 0, 0, 0 }, dirs[k],
                                      &(an_line_opts){ .z = -5, .stroke_width = 4, .stroke = pal->ink, .shadow = AN_OFF,
                                                       .undrawn = o.undrawn });
            if (!a) return NULL;
            ani_obj_part(a, "axis", k);
        }
    }
    return p;
}

an_anim an_space_matrix_to(an_obj *sp, an_mat3 m, const an_anim_opts *opts) {
    an_anim a = { .obj = is_container(sp) ? sp : NULL, .verb = AN_VERB_MATRIX3 };
    memcpy(a.m3, m.m, sizeof a.m3);
    if (opts) a.opts = *opts;
    return a;
}

void an_space_set_matrix(an_obj *sp, an_mat3 m) {
    if (!is_container(sp)) return;
    for (int r = 0; r < 3; r++)
        ani_set(ani_obj_prop(sp, (an_prop)(AN_PROP_M3_ROW0 + r)), (ani_value){ { m.m[3 * r], m.m[3 * r + 1], m.m[3 * r + 2] } });
}

an_mat3 an_space_matrix_at_cursor(const an_obj *sp) {
    if (!is_container(sp)) return AN_MAT3_IDENTITY;
    an_mat3 m;
    for (int r = 0; r < 3; r++) {
        const ani_value v = ani_sample(ani_obj_prop((an_obj *)sp, (an_prop)(AN_PROP_M3_ROW0 + r)), ani_tick_time(sp->scene->cursor));
        for (int k = 0; k < 3; k++) m.m[3 * r + k] = (float)v.v[k];
    }
    return m;
}

an_anim an_space_orbit_to(an_obj *sp, an_orbit o, const an_anim_opts *opts) {
    an_anim a = { .obj = is_space(sp) ? sp : NULL, .verb = AN_VERB_ORBIT, .value = { o.yaw, o.tilt, o.distance } };
    if (opts) a.opts = *opts;
    return a;
}

void an_space_set_orbit(an_obj *sp, an_orbit o) {
    if (!is_space(sp)) return;
    ani_set(ani_obj_prop(sp, AN_PROP_SPACE_ORBIT), (ani_value){ { o.yaw, o.tilt } });
    if (o.distance > 0.0f) ani_set(ani_obj_prop(sp, AN_PROP_SPACE_DISTANCE), (ani_value){ { o.distance } });
}

an_orbit an_space_orbit_at_cursor(const an_obj *sp) {
    if (!is_space(sp)) return (an_orbit){ 0, 0, 10 };
    const ani_time at = ani_tick_time(sp->scene->cursor);
    const ani_value o = ani_sample(ani_obj_prop((an_obj *)sp, AN_PROP_SPACE_ORBIT), at);
    const ani_value d = ani_sample(ani_obj_prop((an_obj *)sp, AN_PROP_SPACE_DISTANCE), at);
    return (an_orbit){ (float)o.v[0], (float)o.v[1], (float)d.v[0] };
}

an_anim an_space_target_to(an_obj *sp, an_vec3 t, const an_anim_opts *opts) {
    an_anim a = { .obj = is_space(sp) ? sp : NULL, .prop = AN_PROP_SPACE_TARGET, .value = { t.x, t.y, t.z } };
    if (opts) a.opts = *opts;
    return a;
}

an_anim an_move3_to(an_obj *o, an_vec3 p, const an_anim_opts *opts) {
    an_anim a = { .obj = o, .verb = AN_VERB_MOVE3, .value = { p.x, p.y, p.z } };
    if (opts) a.opts = *opts;
    return a;
}

void an_obj_set_pos3(an_obj *o, an_vec3 p) {
    if (!o) return;
    ani_set(ani_obj_prop(o, AN_PROP_POS), (ani_value){ { p.x, p.y } });
    ani_set(ani_obj_prop(o, AN_PROP_POS_Z), (ani_value){ { p.z } });
}

// The three verbs: several tracks with the same start, duration and ease. A spring's
// settle time is relative to its travel, so it's the same for every track.
int64_t ani_space_record(an_scene *s, const an_anim *a, int64_t start) {
    an_obj *o = a->obj;
    const bool spring = a->opts.ease.kind == AN_EASE_KIND_SPRING;
    int64_t duration = ani_ticks(a->opts.duration);
    if (duration == 0) duration = spring ? -1 : ANI_TICKS_PER_SEC;
    int64_t end = start, e;
#define TRACK(prop, ...)                                                                                       \
    do {                                                                                                       \
        e = ani_record(s, ani_obj_prop(o, prop), start, duration, a->opts.ease, (ani_value){ { __VA_ARGS__ } }, \
                       false, NULL);                                                                           \
        if (e > end) end = e;                                                                                  \
    } while (0)
    switch (a->verb) {
    case AN_VERB_MATRIX3:
        if (o->kind != AN_KIND_SPACE && o->kind != AN_KIND_SPACE_FRAME) return start;
        for (int r = 0; r < 3; r++) TRACK((an_prop)(AN_PROP_M3_ROW0 + r), a->m3[3 * r], a->m3[3 * r + 1], a->m3[3 * r + 2]);
        break;
    case AN_VERB_ORBIT:
        if (o->kind != AN_KIND_SPACE) return start;
        TRACK(AN_PROP_SPACE_ORBIT, a->value[0], a->value[1]);
        if (a->value[2] > 0.0f) TRACK(AN_PROP_SPACE_DISTANCE, a->value[2]);
        break;
    default: // move3
        TRACK(AN_PROP_POS, a->value[0], a->value[1]);
        TRACK(AN_PROP_POS_Z, a->value[2]);
        break;
    }
#undef TRACK
    return end;
}

an_vec2 an_space_point_at_cursor(const an_obj *sp, an_vec3 p) {
    if (!is_container(sp)) return (an_vec2){ 0, 0 };
    const ani_time at = ani_tick_time(sp->scene->cursor);
    const ani_view v = ani_space_view_at(sp, at);
    const an_vec2 q = ani_view_px(&v, ani_view_cam(&v, (const double[3]){ p.x, p.y, p.z }));
    return ani_affine_apply(ani_obj_world_at((an_obj *)ani_space_root(sp), at), q);
}

float an_space_depth_at_cursor(const an_obj *sp, an_vec3 p) {
    if (!is_container(sp)) return 0.0f;
    const ani_view v = ani_space_view_at(sp, ani_tick_time(sp->scene->cursor));
    return (float)ani_view_cam(&v, (const double[3]){ p.x, p.y, p.z }).depth;
}

static ani_common_opts child_common(an_obj *sp, const ani_common_opts *c) {
    ani_common_opts o = *c;
    o.parent = sp;
    return o;
}

an_obj *an_space_vector(an_obj *sp, an_vec3 v, const an_arrow_opts *opts) {
    if (!is_container(sp)) return NULL;
    an_scene *s = sp->scene;
    const an_arrow_opts o = opts ? *opts : (an_arrow_opts){ 0 };
    ani_common_opts c = child_common(sp, ANI_COMMON(&o));
    c.pos = (an_vec2){ v.x, v.y };
    c.stroke = o.stroke.a > 0.0f ? o.stroke : s->style.palette->ink;
    c.stroke_width = o.stroke_width > 0.0f ? o.stroke_width : 5.0f;
    an_obj *a = ani_obj_make(s, AN_KIND_SPACE_VECTOR, &c);
    if (!a) return NULL;
    a->pos_z = v.z;
    a->has_stroke = true;
    an_obj *h = an_polygon(s, &(an_polygon_opts){
        .parent = a, .fill = c.stroke, .undrawn = o.undrawn, .shadow = o.shadow, .riso = o.riso,
        .points = (an_vec2[]){ { 0, 0 }, { -ANI_PLANE_HEAD, -8 }, { -ANI_PLANE_HEAD, 8 } }, .count = 3 });
    if (!h) return NULL;
    ani_obj_part(h, "head", 0);
    a->u.smark.head = h;
    return a;
}

static an_obj *stroke_child(an_obj *sp, an_kind kind, const an_line_opts *opts, float width) {
    an_scene *s = sp->scene;
    const an_line_opts o = opts ? *opts : (an_line_opts){ 0 };
    ani_common_opts c = child_common(sp, ANI_COMMON(&o));
    c.pos = (an_vec2){ 0, 0 };
    c.stroke = o.stroke.a > 0.0f ? o.stroke : s->style.palette->ink;
    c.stroke_width = o.stroke_width > 0.0f ? o.stroke_width : width;
    an_obj *l = ani_obj_make(s, kind, &c);
    if (l) l->has_stroke = true;
    return l;
}

an_obj *an_space_line(an_obj *sp, an_vec3 at, an_vec3 dir, const an_line_opts *opts) {
    if (!is_container(sp)) return NULL;
    an_obj *l = stroke_child(sp, AN_KIND_SPACE_LINE, opts, 3.0f);
    if (!l) return NULL;
    l->u.smark.p = at;
    l->u.smark.dir = dir;
    return l;
}

an_obj *an_space_path(an_obj *sp, const an_vec3 *pts, int n, bool closed, const an_line_opts *opts) {
    if (!is_container(sp) || !pts || n < 2) return NULL;
    an_obj *l = stroke_child(sp, AN_KIND_SPACE_PATH, opts, 3.0f);
    if (!l) return NULL;
    an_vec3 *copy = ani_arena_alloc(&sp->scene->arena, sizeof(an_vec3) * (size_t)n, alignof(an_vec3));
    if (!copy) return NULL;
    memcpy(copy, pts, sizeof(an_vec3) * (size_t)n);
    l->u.smark.pts = copy;
    l->u.smark.npts = n;
    l->u.smark.closed = closed && n > 2;
    return l;
}

an_obj *an_space_dot(an_obj *sp, an_vec3 at, const an_circle_opts *opts) {
    if (!is_container(sp)) return NULL;
    an_circle_opts o = opts ? *opts : (an_circle_opts){ 0 };
    o.parent = sp;
    o.pos = (an_vec2){ at.x, at.y };
    if (!(o.radius > 0.0f)) o.radius = 7.0f;
    an_obj *d = an_circle(sp->scene, &o);
    if (d) d->pos_z = at.z;
    return d;
}

an_obj *an_space_frame(an_obj *parent, const an_frame_opts *opts) {
    if (!is_container(parent)) return NULL;
    const an_frame_opts o = opts ? *opts : (an_frame_opts){ 0 };
    ani_common_opts c = child_common(parent, ANI_COMMON(&o));
    c.pos = (an_vec2){ 0, 0 };
    an_obj *f = ani_obj_make(parent->scene, AN_KIND_SPACE_FRAME, &c);
    if (!f) return NULL;
    bool zero = true;
    for (int k = 0; k < 9; k++) zero = zero && o.matrix.m[k] == 0.0f;
    memcpy(f->u.frame.m, zero ? AN_MAT3_IDENTITY.m : o.matrix.m, sizeof f->u.frame.m);
    f->u.frame.origin = o.origin;
    const double l = sqrt((double)o.spin_axis.x * o.spin_axis.x + (double)o.spin_axis.y * o.spin_axis.y +
                          (double)o.spin_axis.z * o.spin_axis.z);
    f->u.frame.axis = l > 0.0 ? (an_vec3){ (float)(o.spin_axis.x / l), (float)(o.spin_axis.y / l), (float)(o.spin_axis.z / l) }
                              : (an_vec3){ 0, 0, 1 };
    f->u.frame.spin = o.spin;
    return f;
}

an_anim an_frame_spin_to(an_obj *f, float radians, const an_anim_opts *opts) {
    an_anim a = { .obj = f && f->kind == AN_KIND_SPACE_FRAME ? f : NULL, .prop = AN_PROP_FRAME_SPIN, .value = { radians } };
    if (opts) a.opts = *opts;
    return a;
}

an_anim an_frame_origin_to(an_obj *f, an_vec3 o, const an_anim_opts *opts) {
    an_anim a = { .obj = f && f->kind == AN_KIND_SPACE_FRAME ? f : NULL, .prop = AN_PROP_FRAME_ORIGIN, .value = { o.x, o.y, o.z } };
    if (opts) a.opts = *opts;
    return a;
}

an_obj *an_space_sphere(an_obj *parent, const an_sphere_opts *opts) {
    if (!is_container(parent)) return NULL;
    an_scene *s = parent->scene;
    const an_sphere_opts o = opts ? *opts : (an_sphere_opts){ 0 };
    ani_common_opts c = child_common(parent, ANI_COMMON(&o));
    c.pos = (an_vec2){ 0, 0 };
    c.stroke = o.stroke.a > 0.0f ? o.stroke : s->style.palette->ink;
    c.stroke_width = o.stroke_width > 0.0f ? o.stroke_width : 3.0f;
    an_obj *sph = ani_obj_make(s, AN_KIND_SPACE_SPHERE, &c);
    if (!sph) return NULL;
    sph->has_stroke = true;
    sph->no_fill = !(o.fill.a > 0.0f);
    sph->u.sphere.center = o.center;
    sph->u.sphere.radius = o.radius > 0.0f ? o.radius : 1.0f;
    sph->u.sphere.back = o.back < 0.0f ? 0.0f : o.back > 1.0f ? 1.0f : o.back;
    sph->u.sphere.meridians = o.outline_only ? 0 : o.meridians > 0 ? o.meridians : 12;
    sph->u.sphere.parallels = o.outline_only ? 0 : o.parallels > 0 ? o.parallels : 7;
    return sph;
}
