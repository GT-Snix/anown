#include "path_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ANI_PI_D 3.14159265358979323846

static bool grow(void **p, int *cap, int need, size_t elem) {
    if (need <= *cap) return true;
    int n = *cap ? *cap : 64;
    while (n < need) {
        if (n > (1 << 26)) return false;
        n *= 2;
    }
    void *q = realloc(*p, (size_t)n * elem);
    if (!q) return false;
    *p = q;
    *cap = n;
    return true;
}

void ani_pb_init(ani_path_builder *b) { *b = (ani_path_builder){ 0 }; }

void ani_pb_free(ani_path_builder *b) {
    free(b->pts);
    free(b->subs);
    *b = (ani_path_builder){ 0 };
}

static bool push(ani_path_builder *b, an_vec2 p) {
    if (!grow((void **)&b->pts, &b->cap, b->npts + 1, sizeof(an_vec2))) return false;
    b->pts[b->npts++] = p;
    b->subs[b->nsubs - 1].count++;
    return true;
}

bool ani_pb_move(ani_path_builder *b, an_vec2 p) {
    if (!grow((void **)&b->subs, &b->subcap, b->nsubs + 1, sizeof(ani_subpath))) return false;
    b->subs[b->nsubs++] = (ani_subpath){ .first = b->npts };
    b->open = true;
    return push(b, p);
}

static an_vec2 last_point(const ani_path_builder *b) {
    return b->npts ? b->pts[b->npts - 1] : (an_vec2){ 0, 0 };
}

bool ani_pb_line(ani_path_builder *b, an_vec2 p) {
    if (!b->open && !ani_pb_move(b, last_point(b))) return false;
    return push(b, p);
}

static double dist_to_line(an_vec2 p, an_vec2 a, an_vec2 b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len = sqrt(dx * dx + dy * dy);
    if (len < 1e-12) return hypot(p.x - a.x, p.y - a.y);
    return fabs((p.x - a.x) * dy - (p.y - a.y) * dx) / len;
}

static an_vec2 mid(an_vec2 a, an_vec2 b) { return (an_vec2){ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f }; }

// Adaptive subdivision: split at t = 0.5 until both control points are within
// tol of the chord. Depth-limited, so the result is bounded and deterministic.
static bool cubic_rec(ani_path_builder *b, an_vec2 p0, an_vec2 c1, an_vec2 c2, an_vec2 p3,
                      float tol, int depth) {
    const double d = fmax(dist_to_line(c1, p0, p3), dist_to_line(c2, p0, p3));
    if (d <= tol || depth >= 16) return push(b, p3);
    const an_vec2 a = mid(p0, c1), m = mid(c1, c2), c = mid(c2, p3);
    const an_vec2 ab = mid(a, m), bc = mid(m, c), q = mid(ab, bc);
    return cubic_rec(b, p0, a, ab, q, tol, depth + 1) && cubic_rec(b, q, bc, c, p3, tol, depth + 1);
}

bool ani_pb_cubic(ani_path_builder *b, an_vec2 c1, an_vec2 c2, an_vec2 p, float tol) {
    if (!b->open && !ani_pb_move(b, last_point(b))) return false;
    return cubic_rec(b, last_point(b), c1, c2, p, tol, 0);
}

bool ani_pb_quad(ani_path_builder *b, an_vec2 c, an_vec2 p, float tol) {
    const an_vec2 p0 = last_point(b);
    // degree elevation: exact cubic of the same curve
    const an_vec2 c1 = { p0.x + 2.0f / 3.0f * (c.x - p0.x), p0.y + 2.0f / 3.0f * (c.y - p0.y) };
    const an_vec2 c2 = { p.x + 2.0f / 3.0f * (c.x - p.x), p.y + 2.0f / 3.0f * (c.y - p.y) };
    return ani_pb_cubic(b, c1, c2, p, tol);
}

void ani_pb_close(ani_path_builder *b) {
    if (!b->open) return;
    ani_subpath *sp = &b->subs[b->nsubs - 1];
    // drop an explicit final point that repeats the first: the closing segment covers it
    if (sp->count >= 2) {
        const an_vec2 f = b->pts[sp->first], l = b->pts[b->npts - 1];
        if (fabsf(f.x - l.x) < 1e-6f && fabsf(f.y - l.y) < 1e-6f) {
            b->npts--;
            sp->count--;
        }
    }
    sp->closed = true;
    b->open = false;
}

ani_path *ani_pb_finish(ani_path_builder *b, ani_arena *a) {
    ani_path *p = ANI_NEW(a, ani_path);
    if (!p) return NULL;
    p->npts = b->npts;
    p->nsubs = b->nsubs;
    p->pts = ani_arena_alloc(a, sizeof(an_vec2) * (size_t)(b->npts ? b->npts : 1), _Alignof(an_vec2));
    p->s = ani_arena_alloc(a, sizeof(float) * (size_t)(b->npts ? b->npts : 1), _Alignof(float));
    p->subs = ani_arena_alloc(a, sizeof(ani_subpath) * (size_t)(b->nsubs ? b->nsubs : 1), _Alignof(ani_subpath));
    if (!p->pts || !p->s || !p->subs) return NULL;
    if (b->npts) memcpy(p->pts, b->pts, sizeof(an_vec2) * (size_t)b->npts);
    if (b->nsubs) memcpy(p->subs, b->subs, sizeof(ani_subpath) * (size_t)b->nsubs);
    double total = 0.0;
    for (int k = 0; k < p->nsubs; k++) {
        ani_subpath *sp = &p->subs[k];
        double s = 0.0;
        for (int i = 0; i < sp->count; i++) {
            const int j = sp->first + i;
            if (i > 0) s += hypot(p->pts[j].x - p->pts[j - 1].x, p->pts[j].y - p->pts[j - 1].y);
            p->s[j] = (float)s;
        }
        if (sp->closed && sp->count >= 2) {
            const an_vec2 f = p->pts[sp->first], l = p->pts[sp->first + sp->count - 1];
            s += hypot(f.x - l.x, f.y - l.y);
        }
        sp->len = (float)s;
        sp->s0 = (float)total;
        total += s;
    }
    p->total = (float)total;
    return p;
}

// ---- shapes -------------------------------------------------------------------------

ani_path *ani_path_segs(ani_arena *a, const an_seg *segs, int n, float tol) {
    ani_path_builder b;
    ani_pb_init(&b);
    bool ok = true;
    for (int i = 0; ok && i < n; i++) {
        const an_seg *g = &segs[i];
        switch (g->kind) {
        case AN_SEG_MOVE: ok = ani_pb_move(&b, g->p[0]); break;
        case AN_SEG_LINE: ok = ani_pb_line(&b, g->p[0]); break;
        case AN_SEG_QUAD: ok = ani_pb_quad(&b, g->p[0], g->p[1], tol); break;
        case AN_SEG_CUBIC: ok = ani_pb_cubic(&b, g->p[0], g->p[1], g->p[2], tol); break;
        case AN_SEG_CLOSE: ani_pb_close(&b); break;
        }
    }
    ani_path *p = ok ? ani_pb_finish(&b, a) : NULL;
    ani_pb_free(&b);
    return p;
}

ani_path *ani_path_rect(ani_arena *a, an_vec2 size) {
    const float hx = size.x * 0.5f, hy = size.y * 0.5f;
    // starts at the top-left corner and runs clockwise on screen
    const an_seg segs[5] = { AN_MOVE(-hx, -hy), AN_LINE(hx, -hy), AN_LINE(hx, hy), AN_LINE(-hx, hy), AN_CLOSE };
    return ani_path_segs(a, segs, 5, ANI_FLATNESS); // straight segments only: tolerance unused
}

int ani_circle_segments(float radius, float tol) {
    // sagitta r (1 - cos(pi/n)) <= tol  =>  n >= pi / acos(1 - tol/r)
    const double r = fabs((double)radius);
    if (r <= tol) return 8;
    double n = ceil(ANI_PI_D / acos(1.0 - tol / r));
    if (n < 16) n = 16;
    if (n > 2048) n = 2048;
    return (int)n;
}

ani_path *ani_path_circle(ani_arena *a, float radius, float tol) {
    ani_path_builder b;
    ani_pb_init(&b);
    const int n = ani_circle_segments(radius, tol);
    // starts at the top and runs clockwise on screen (y down)
    bool ok = ani_pb_move(&b, (an_vec2){ 0, -radius });
    for (int i = 1; ok && i < n; i++) {
        const double t = -ANI_PI_D / 2 + 2.0 * ANI_PI_D * i / n;
        ok = ani_pb_line(&b, (an_vec2){ (float)(radius * cos(t)), (float)(radius * sin(t)) });
    }
    ani_pb_close(&b);
    ani_path *p = ok ? ani_pb_finish(&b, a) : NULL;
    ani_pb_free(&b);
    return p;
}

ani_path *ani_path_line(ani_arena *a, an_vec2 from, an_vec2 to) {
    const an_seg segs[2] = { AN_MOVE(from.x, from.y), AN_LINE(to.x, to.y) };
    return ani_path_segs(a, segs, 2, ANI_FLATNESS); // straight segments only: tolerance unused
}

ani_path *ani_path_arrow(ani_arena *a, an_vec2 from, an_vec2 to, float head) {
    const double dx = to.x - from.x, dy = to.y - from.y;
    const double len = hypot(dx, dy);
    const double ux = len > 1e-9 ? dx / len : 1.0, uy = len > 1e-9 ? dy / len : 0.0;
    const double back = head, side = head * 0.55;
    const an_vec2 w1 = { (float)(to.x - ux * back - uy * side), (float)(to.y - uy * back + ux * side) };
    const an_vec2 w2 = { (float)(to.x - ux * back + uy * side), (float)(to.y - uy * back - ux * side) };
    // shaft, then the head as one pen stroke through the tip
    const an_seg segs[5] = { AN_MOVE(from.x, from.y), AN_LINE(to.x, to.y),
                             AN_MOVE(w1.x, w1.y), AN_LINE(to.x, to.y), AN_LINE(w2.x, w2.y) };
    return ani_path_segs(a, segs, 5, ANI_FLATNESS); // straight segments only: tolerance unused
}

// ---- queries ------------------------------------------------------------------------

void ani_path_at(const ani_path *p, double s, an_vec2 *pos, an_vec2 *tangent, int *sub, double *s_in_sub) {
    *pos = (an_vec2){ 0, 0 };
    *tangent = (an_vec2){ 1, 0 };
    if (sub) *sub = 0;
    if (s_in_sub) *s_in_sub = 0;
    if (!p || p->nsubs == 0 || p->npts == 0) return;
    if (s < 0) s = 0;
    if (s > p->total) s = p->total;
    // subpath containing s (the last one whose start is <= s)
    int k = 0;
    while (k + 1 < p->nsubs && p->subs[k + 1].s0 <= s) k++;
    const ani_subpath *sp = &p->subs[k];
    double ls = s - sp->s0;
    if (sub) *sub = k;
    if (s_in_sub) *s_in_sub = ls;
    const int nseg = sp->closed ? sp->count : sp->count - 1;
    if (nseg <= 0) {
        *pos = p->pts[sp->first];
        return;
    }
    // walk segments; skip zero-length ones for the tangent
    for (int i = 0; i < nseg; i++) {
        const int a = sp->first + i, b = sp->first + (i + 1) % sp->count;
        const double sa = p->s[a];
        const double sb = (i + 1 == sp->count) ? sp->len : p->s[b];
        const double seg = sb - sa;
        if (ls <= sb || i == nseg - 1) {
            const double t = seg > 1e-12 ? (ls - sa) / seg : 0.0;
            const double tc = t < 0 ? 0 : (t > 1 ? 1 : t);
            pos->x = (float)(p->pts[a].x + (p->pts[b].x - p->pts[a].x) * tc);
            pos->y = (float)(p->pts[a].y + (p->pts[b].y - p->pts[a].y) * tc);
            if (seg > 1e-12) {
                tangent->x = (float)((p->pts[b].x - p->pts[a].x) / seg);
                tangent->y = (float)((p->pts[b].y - p->pts[a].y) / seg);
                return;
            }
            // zero-length: look ahead for a direction
            for (int j = i + 1; j < nseg; j++) {
                const int c = sp->first + j, d = sp->first + (j + 1) % sp->count;
                const double dx = p->pts[d].x - p->pts[c].x, dy = p->pts[d].y - p->pts[c].y;
                const double l = hypot(dx, dy);
                if (l > 1e-12) {
                    *tangent = (an_vec2){ (float)(dx / l), (float)(dy / l) };
                    return;
                }
            }
            return;
        }
    }
}
