// Stroke triangulation. For each subpath (one pen stroke):
//   1. take the centerline points revealed by the draw progress (cutting the
//      last segment at the exact arc length), transform them to scene space
//   2. drop points closer than ANI_DEDUP_PX to the previous one (duplicates,
//      zero-length segments)
//   3. per point: half width = 0.5 * width * world scale * brush factor
//   4. emit a trapezoid per segment, a round join fan on the OUTER side of every
//      interior point, round caps at open ends, an overshoot spur at sharp
//      corners, or a single dot if only one point is left
// Every triangle goes through ani_mesh_tri, which drops non-finite and
// near-zero-area triangles. Inner sides of joins overlap; outer sides never gap.
#include "stroke_internal.h"

#include <math.h>
#include <stdlib.h>

#define ANI_PI_F 3.14159265358979323846f
#define ANI_DEDUP_PX 1e-3f     // points closer than this are merged
#define ANI_ARC_SAGITTA 0.2f   // max distance between a round arc and its polygon, px
#define ANI_COLLINEAR 1e-3f    // joins turning less than this (radians) get no fan
#define ANI_SPUR_TIP 0.55f     // spur tip width relative to the corner's width

static bool grow(void **p, int *cap, int need, size_t elem) {
    if (need <= *cap) return true;
    int n = *cap ? *cap : 128;
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

double ani_brush_factor(const ani_brush *b, double s, double len) {
    if (!b) return 1.0;
    double f = 1.0;
    double tin = b->taper_in, tout = b->taper_out;
    if (tin + tout > len && tin + tout > 0.0) { // both don't fit: shrink proportionally
        const double k = len / (tin + tout);
        tin *= k;
        tout *= k;
    }
    // quadratic ease-out: the pen lands and widens quickly, then levels off
    if (tin > 0.0 && s < tin) {
        const double x = s / tin;
        f *= x * (2.0 - x);
    }
    if (tout > 0.0 && s > len - tout) {
        const double x = (len - s) / tout;
        f *= x <= 0.0 ? 0.0 : x * (2.0 - x);
    }
    if (b->pressure && b->pressure_count > 0) {
        if (b->pressure_count == 1 || len <= 0.0) {
            f *= b->pressure[0];
        } else {
            double u = s / len * (b->pressure_count - 1);
            if (u < 0) u = 0;
            const int i = (int)u >= b->pressure_count - 1 ? b->pressure_count - 2 : (int)u;
            const double t = u - i;
            f *= b->pressure[i] + (b->pressure[i + 1] - b->pressure[i]) * t;
        }
    }
    return f < 0.0 ? 0.0 : f;
}

static an_vec2 v_add(an_vec2 a, an_vec2 b) { return (an_vec2){ a.x + b.x, a.y + b.y }; }
static an_vec2 v_sub(an_vec2 a, an_vec2 b) { return (an_vec2){ a.x - b.x, a.y - b.y }; }
static an_vec2 v_mul(an_vec2 a, float k) { return (an_vec2){ a.x * k, a.y * k }; }
static an_vec2 v_rot(an_vec2 v, float a) {
    const float c = cosf(a), s = sinf(a);
    return (an_vec2){ v.x * c - v.y * s, v.x * s + v.y * c };
}

// Round arc: triangles fanning from c, starting at direction va (unit), turning
// by `angle` (signed), radius r.
static bool fan(ani_mesh *m, an_vec2 c, an_vec2 va, float angle, float r, an_color col) {
    if (!(r > 0.0f) || !(fabsf(angle) > 0.0f)) return true;
    const float ratio = ANI_ARC_SAGITTA / r;
    const float step = ratio >= 1.0f ? ANI_PI_F / 2 : 2.0f * acosf(1.0f - ratio);
    int n = (int)ceilf(fabsf(angle) / step);
    if (n < 1) n = 1;
    if (n > 128) n = 128;
    an_vec2 prev = v_add(c, v_mul(va, r));
    for (int i = 1; i <= n; i++) {
        const an_vec2 cur = v_add(c, v_mul(v_rot(va, angle * (float)i / (float)n), r));
        if (!ani_mesh_tri(m, c, prev, cur, col)) return false;
        prev = cur;
    }
    return true;
}

static bool quad(ani_mesh *m, an_vec2 a, an_vec2 b, an_vec2 c, an_vec2 d, an_color col) {
    return ani_mesh_tri(m, a, b, c, col) && ani_mesh_tri(m, a, c, d, col);
}

// Scratch for one subpath's revealed points (scene space). Static-free: owned
// by the call, so emitting is reentrant and keeps no state between calls.
typedef ani_pts pts_buf; // scratch lives in the mesh (ani_mesh.pts_a/pts_b), reused

static bool pts_push(pts_buf *b, an_vec2 q, float h, float s) {
    if (b->n >= b->cap) {
        int cap = b->cap;
        if (!grow((void **)&b->q, &cap, b->n + 1, sizeof(an_vec2))) return false;
        int cap2 = b->cap;
        if (!grow((void **)&b->h, &cap2, b->n + 1, sizeof(float))) return false;
        int cap3 = b->cap;
        if (!grow((void **)&b->s, &cap3, b->n + 1, sizeof(float))) return false;
        b->cap = cap;
    }
    // merge duplicates and zero-length segments
    if (b->n > 0) {
        const an_vec2 d = v_sub(q, b->q[b->n - 1]);
        if (d.x * d.x + d.y * d.y < ANI_DEDUP_PX * ANI_DEDUP_PX) {
            if (h > b->h[b->n - 1]) b->h[b->n - 1] = h; // keep the wider
            return true;
        }
    }
    b->q[b->n] = q;
    b->h[b->n] = h;
    b->s[b->n] = s;
    b->n++;
    return true;
}

// Width is only known where we sample it, and a long straight segment has just
// two points. Where the brush varies (taper zones; everywhere with a pressure
// curve), insert samples every ANI_PROFILE_STEP local units strictly between
// (a, sa) and (b, sb), so the ribbon follows the profile instead of linearly
// interpolating across the whole segment.
#define ANI_PROFILE_STEP 3.0

static bool brush_varies(const ani_brush *br, double s, double len) {
    if (!br) return false;
    if (br->pressure && br->pressure_count > 1) return true;
    double tin = br->taper_in, tout = br->taper_out;
    if (tin + tout > len && tin + tout > 0.0) {
        const double k = len / (tin + tout);
        tin *= k;
        tout *= k;
    }
    return (tin > 0.0 && s < tin) || (tout > 0.0 && s > len - tout);
}

// Insert samples strictly between (a, sa) and (pb, sb) on the ORIGINAL path
// segment, and strictly below `limit` (the draw cut, or sb): taper/pressure
// samples every ANI_PROFILE_STEP where the brush varies, and boil samples every
// `boil_sp` (0 = none). Interpolating over the original segment, not toward the
// cut point, makes a sample's position independent of how far the stroke is
// drawn: bit for bit, so nothing crawls.
// Boil displacement streamed into the gather pass (defined below).
typedef struct boil_stream boil_stream;
static bool u_push(pts_buf *b, boil_stream *st, an_vec2 q, float h, float s);

static bool push_samples(pts_buf *b, ani_affine w, double width_px, const ani_brush *br, double len,
                         an_vec2 a, double sa, an_vec2 pb, double sb, double limit, double boil_sp, boil_stream *st) {
    const double end = (limit < sb ? limit : sb) - 1e-6;
    if (!(sb > sa) || end <= sa) return true;
    const bool prof = br != NULL;
    double np = prof ? floor(sa / ANI_PROFILE_STEP) * ANI_PROFILE_STEP + ANI_PROFILE_STEP : INFINITY;
    double nb = boil_sp > 0.0 ? (floor(sa / boil_sp) + 1.0) * boil_sp : INFINITY;
    for (;;) {
        const double s = np < nb ? np : nb;
        if (!(s < end)) break;
        const bool is_p = s == np, is_b = s == nb;
        if (is_p) np += ANI_PROFILE_STEP;
        if (is_b) nb += boil_sp;
        if (!is_b && !brush_varies(br, s, len)) continue; // a profile sample the brush doesn't need
        const double t = (s - sa) / (sb - sa);
        const an_vec2 lp = { (float)(a.x + (pb.x - a.x) * t), (float)(a.y + (pb.y - a.y) * t) };
        const float h = (float)(0.5 * width_px * ani_brush_factor(br, s, len));
        if (!u_push(b, st, ani_affine_apply(w, lp), h, (float)s)) return false;
    }
    return true;
}

static an_vec2 normal_of(an_vec2 a, an_vec2 b, an_vec2 *dir) {
    const an_vec2 d = v_sub(b, a);
    const float len = sqrtf(d.x * d.x + d.y * d.y); // > ANI_DEDUP_PX after merging
    *dir = (an_vec2){ d.x / len, d.y / len };
    return (an_vec2){ -dir->y, dir->x };
}

static bool log_rib(ani_rib_log *log, an_vec2 p, an_vec2 n, float h, float s, int sub) {
    if (!log) return true;
    if (!grow((void **)&log->ribs, &log->cap, log->count + 1, sizeof(ani_rib))) return false;
    log->ribs[log->count++] = (ani_rib){ p, n, h, s, sub };
    return true;
}

// Round join on the outer side at q, between incoming direction d0 and
// outgoing d1 (normals n0, n1), plus an overshoot spur for sharp corners.
static bool join(ani_mesh *m, an_vec2 q, an_vec2 d0, an_vec2 n0, an_vec2 d1, float h,
                 float overshoot, an_color col) {
    const float cross = d0.x * d1.y - d0.y * d1.x;
    const float dot = d0.x * d1.x + d0.y * d1.y;
    const float angle = atan2f(cross, dot); // signed turn, (-pi, pi]
    if (fabsf(angle) >= ANI_COLLINEAR) {
        // turning toward +n opens a gap on the -n side, and vice versa
        const float side = angle > 0.0f ? -1.0f : 1.0f;
        if (!fan(m, q, v_mul(n0, side), angle, h, col)) return false;
    }
    if (overshoot > 0.0f && fabsf(angle) > ANI_OVERSHOOT_MIN_ANGLE) {
        float f = (fabsf(angle) - ANI_OVERSHOOT_MIN_ANGLE) / (ANI_PI_F / 2 - ANI_OVERSHOOT_MIN_ANGLE);
        if (f > 1.0f) f = 1.0f;
        const float len = overshoot * 2.0f * h * f;
        if (len > 0.5f) {
            const an_vec2 tip = v_add(q, v_mul(d0, len));
            const float ht = h * ANI_SPUR_TIP;
            if (!quad(m, v_add(q, v_mul(n0, h)), v_add(tip, v_mul(n0, ht)),
                      v_sub(tip, v_mul(n0, ht)), v_sub(q, v_mul(n0, h)), col))
                return false;
            if (!fan(m, tip, v_mul(n0, -1.0f), ANI_PI_F, ht, col)) return false; // round tip
        }
    }
    return true;
}

static bool emit_polyline(ani_mesh *m, const pts_buf *b, bool loop, float overshoot, an_color col,
                          ani_rib_log *log, int sub) {
    const int n = b->n;
    if (n == 0) return true;
    if (n == 1) { // a dot
        if (!log_rib(log, b->q[0], (an_vec2){ 0, 1 }, b->h[0], b->s[0], sub)) return false;
        return fan(m, b->q[0], (an_vec2){ 0, -1 }, 2.0f * ANI_PI_F, b->h[0], col);
    }
    if (loop && n < 3) loop = false;
    const int nseg = loop ? n : n - 1;
    for (int i = 0; i < nseg; i++) {
        const int j = (i + 1) % n;
        an_vec2 d;
        const an_vec2 nn = normal_of(b->q[i], b->q[j], &d);
        if (!log_rib(log, b->q[i], nn, b->h[i], b->s[i], sub)) return false;
        if (!quad(m, v_add(b->q[i], v_mul(nn, b->h[i])), v_add(b->q[j], v_mul(nn, b->h[j])),
                  v_sub(b->q[j], v_mul(nn, b->h[j])), v_sub(b->q[i], v_mul(nn, b->h[i])), col))
            return false;
    }
    if (!loop) {
        an_vec2 d;
        const an_vec2 nn = normal_of(b->q[n - 2], b->q[n - 1], &d);
        if (!log_rib(log, b->q[n - 1], nn, b->h[n - 1], b->s[n - 1], sub)) return false;
    }
    // joins
    const int first_join = loop ? 0 : 1, last_join = loop ? n - 1 : n - 2;
    for (int i = first_join; i <= last_join; i++) {
        const int p = (i - 1 + n) % n, q = (i + 1) % n;
        an_vec2 d0, d1;
        const an_vec2 n0 = normal_of(b->q[p], b->q[i], &d0);
        normal_of(b->q[i], b->q[q], &d1);
        if (!join(m, b->q[i], d0, n0, d1, b->h[i], overshoot, col)) return false;
    }
    if (!loop) { // round caps
        an_vec2 d;
        const an_vec2 n0 = normal_of(b->q[0], b->q[1], &d);
        if (!fan(m, b->q[0], n0, ANI_PI_F, b->h[0], col)) return false;
        const an_vec2 n1 = normal_of(b->q[n - 2], b->q[n - 1], &d);
        if (!fan(m, b->q[n - 1], v_mul(n1, -1.0f), ANI_PI_F, b->h[n - 1], col)) return false;
    }
    return true;
}

// ---- line boil ------------------------------------------------------------------------

static double boil_h(const ani_boil *b, int sub, int64_t k) {
    const uint64_t counter = b->step << 32 | ((uint64_t)(sub & 0xFF) << 24) | ((uint64_t)k & 0xFFFFFF);
    return 2.0 * (double)an_hash_float(b->seed, b->stream, counter) - 1.0;
}

// Bucket spacing actually used: closed subpaths tile the loop with a whole
// number of buckets so k wraps without a jump at the seam.
static double boil_spacing(const ani_boil *b, double len, bool closed, int64_t *nbuckets) {
    double sp = b->spacing > 0.0f ? (double)b->spacing : 15.0;
    *nbuckets = 0;
    if (closed && len > 0.0) {
        int64_t n = (int64_t)floor(len / sp + 0.5);
        if (n < 1) n = 1;
        *nbuckets = n;
        sp = len / (double)n;
    }
    return sp;
}

// Per-subpath noise: spacing, wrap count, and a cache of the last two bucket
// values. Points arrive in increasing arc length, so consecutive points almost
// always share a bucket pair: one an_hash per bucket instead of two per point.
// The arithmetic is exactly ani_boil_disp's, so results are bit-identical.
typedef struct boil_noise {
    const ani_boil *b;
    int sub;
    double len, sp;
    int64_t nb;
    bool closed;
    int64_t k0, k1; // cached bucket indices (after wrap), -1 = empty
    double h0, h1;
} boil_noise;

static void noise_init(boil_noise *z, const ani_boil *b, int sub, double len, bool closed) {
    *z = (boil_noise){ .b = b, .sub = sub, .len = len, .closed = closed, .k0 = -1, .k1 = -1 };
    z->sp = boil_spacing(b, len, closed, &z->nb);
}

static double noise_h(boil_noise *z, int64_t k) {
    if (k == z->k0) return z->h0;
    if (k == z->k1) return z->h1;
    const double h = boil_h(z->b, z->sub, k);
    z->k0 = z->k1; // keep the two most recent
    z->h0 = z->h1;
    z->k1 = k;
    z->h1 = h;
    return h;
}

static double noise_disp(boil_noise *z, double s) {
    const double u = s / z->sp;
    int64_t k = (int64_t)floor(u);
    const double f = u - (double)k;
    int64_t k1 = k + 1;
    if (z->nb > 0) { // wrap
        k = ((k % z->nb) + z->nb) % z->nb;
        k1 = ((k1 % z->nb) + z->nb) % z->nb;
    }
    const double t = f * f * (3.0 - 2.0 * f); // smoothstep
    const double h0 = noise_h(z, k), h1 = noise_h(z, k1);
    return (double)z->b->amp * (h0 + (h1 - h0) * t);
}

double ani_boil_disp(const ani_boil *b, int sub, double s, double len, bool closed) {
    if (!(b->amp > 0.0f)) return 0.0;
    boil_noise z;
    noise_init(&z, b, sub, len, closed);
    return noise_disp(&z, s);
}

// Displace point `cur` (undisplaced) along the normal of the undisplaced
// polyline (prev -> cur -> next, either may be absent). Same float operations,
// in the same order, as the original two-pass version.
static an_vec2 displaced(boil_noise *z, an_vec2 cur, float s, const an_vec2 *prev, const an_vec2 *next) {
    an_vec2 nrm = { 0, 0 };
    if (prev) {
        const an_vec2 a = *prev, c = cur;
        const float dx = c.x - a.x, dy = c.y - a.y, l = sqrtf(dx * dx + dy * dy);
        if (l > 0) nrm = (an_vec2){ nrm.x - dy / l, nrm.y + dx / l };
    }
    if (next) {
        const an_vec2 c = cur, d = *next;
        const float dx = d.x - c.x, dy = d.y - c.y, l = sqrtf(dx * dx + dy * dy);
        if (l > 0) nrm = (an_vec2){ nrm.x - dy / l, nrm.y + dx / l };
    }
    const float nl = sqrtf(nrm.x * nrm.x + nrm.y * nrm.y);
    an_vec2 q = cur;
    if (nl > 1e-6f) {
        const float d = (float)noise_disp(z, s);
        q.x += nrm.x / nl * d;
        q.y += nrm.y / nl * d;
    }
    return q;
}

// Write a displaced point into output slot b->... [o], merging duplicates exactly
// like pts_push does.
static void out_push(pts_buf *b, int *o, an_vec2 q, float h, float s) {
    if (*o > 0) {
        const an_vec2 d = v_sub(q, b->q[*o - 1]);
        if (d.x * d.x + d.y * d.y < ANI_DEDUP_PX * ANI_DEDUP_PX) {
            if (h > b->h[*o - 1]) b->h[*o - 1] = h;
            return;
        }
    }
    b->q[*o] = q;
    b->h[*o] = h;
    b->s[*o] = s;
    (*o)++;
}

// Displacement streamed into the gather pass, one point behind it. Undisplaced
// points occupy [next, n); displaced output is written in place to [0, o) with
// o <= next, so the gather's duplicate check (against the last undisplaced point)
// and every not-yet-displaced neighbor stay intact.
struct boil_stream {
    bool on;
    boil_noise z;
    int next, o;
    an_vec2 prev_u; // undisplaced position of point next - 1
};

static void stream_displace(pts_buf *b, boil_stream *st, bool has_next) {
    const int i = st->next;
    const an_vec2 cur = b->q[i];
    const an_vec2 nx = has_next ? b->q[i + 1] : (an_vec2){ 0, 0 };
    const float h = b->h[i], s = b->s[i];
    const an_vec2 q = displaced(&st->z, cur, s, i > 0 ? &st->prev_u : NULL, has_next ? &nx : NULL);
    st->prev_u = cur;
    st->next++;
    out_push(b, &st->o, q, h, s);
}

static bool u_push(pts_buf *b, boil_stream *st, an_vec2 q, float h, float s) {
    const int before = b->n;
    if (!pts_push(b, q, h, s)) return false;
    // a new point (not merged) finalizes the one before it: displace that now
    if (st && st->on && b->n > before && b->n >= 2) {
        while (st->next <= b->n - 2) stream_displace(b, st, true);
    }
    return true;
}

static void stream_finish(pts_buf *b, boil_stream *st) {
    if (!st || !st->on) return;
    while (st->next < b->n) stream_displace(b, st, st->next + 1 < b->n);
    b->n = st->o;
}

// Closed seamless loops: the first point's normal needs the last point, so the
// whole loop is displaced after gathering, in place (no copy).
static void displace_loop_inplace(pts_buf *b, boil_noise *z) {
    const int m = b->n;
    if (m == 0) return;
    const an_vec2 u_first = b->q[0];
    an_vec2 prev = b->q[m - 1];
    int o = 0;
    for (int i = 0; i < m; i++) {
        const an_vec2 cur = b->q[i];
        const an_vec2 nx = (i + 1 < m) ? b->q[i + 1] : u_first;
        const float h = b->h[i], s = b->s[i];
        const an_vec2 q = displaced(z, cur, s, &prev, &nx);
        prev = cur;
        out_push(b, &o, q, h, s);
    }
    b->n = o;
}

static bool ensure(pts_buf *b, int need) {
    if (need <= b->cap) return true;
    int c1 = b->cap, c2 = b->cap, c3 = b->cap;
    if (!grow((void **)&b->q, &c1, need, sizeof(an_vec2)) || !grow((void **)&b->h, &c2, need, sizeof(float)) ||
        !grow((void **)&b->s, &c3, need, sizeof(float)))
        return false;
    b->cap = c1;
    return true;
}

int ani_boil_ring(ani_mesh *mesh, const ani_path *p, ani_affine w, const ani_boil *b, const an_vec2 **out) {
    *out = NULL;
    if (!p || p->nsubs == 0 || p->subs[0].count < 3) return 0;
    const ani_subpath *sp = &p->subs[0];
    pts_buf *src = &mesh->pts_a, *dst = &mesh->pts_b;
    src->n = 0;
    for (int i = 0; i < sp->count; i++) {
        if (!pts_push(src, ani_affine_apply(w, p->pts[sp->first + i]), 0.0f, p->s[sp->first + i])) return -1;
    }
    // insert bucket points between consecutive (deduplicated) ring points, in
    // scene space, then displace the loop
    boil_noise z;
    noise_init(&z, b, 0, sp->len, true);
    const int n = src->n;
    dst->n = 0;
    for (int i = 0; i < n; i++) {
        if (!ensure(dst, dst->n + 1)) return -1;
        dst->q[dst->n] = src->q[i];
        dst->h[dst->n] = src->h[i];
        dst->s[dst->n] = src->s[i];
        dst->n++;
        const int j = (i + 1) % n;
        const double sa = src->s[i], sb = (j == 0) ? sp->len : src->s[j];
        for (double s = (floor(sa / z.sp) + 1.0) * z.sp; s < sb - 1e-6; s += z.sp) {
            const double t = (s - sa) / (sb - sa);
            if (!ensure(dst, dst->n + 1)) return -1;
            dst->q[dst->n] = (an_vec2){ (float)(src->q[i].x + (src->q[j].x - src->q[i].x) * t),
                                        (float)(src->q[i].y + (src->q[j].y - src->q[i].y) * t) };
            dst->h[dst->n] = (float)(src->h[i] + (src->h[j] - src->h[i]) * t);
            dst->s[dst->n] = (float)s;
            dst->n++;
        }
    }
    displace_loop_inplace(dst, &z);
    *out = dst->q;
    return dst->n;
}

bool ani_stroke_emit(ani_mesh *m, const ani_stroke_in *in, ani_rib_log *log) {
    const ani_path *p = in->path;
    if (!p || p->npts == 0 || !(in->progress > 0.0f) || !(in->width > 0.0f)) return true;
    const ani_affine w = in->world;
    const float wscale = sqrtf(fabsf(w.a * w.d - w.b * w.c));
    const double cut = (in->progress >= 1.0f ? 1.0 : (double)in->progress) * p->total;
    const bool full = in->progress >= 1.0f;
    const ani_brush *br = in->brush;
    const bool tapered = br && (br->taper_in > 0.0f || br->taper_out > 0.0f);
    const float overshoot = br ? br->overshoot : 0.0f;
    const bool boil = in->boil.amp > 0.0f;

    pts_buf *b = &m->pts_a; // reused across strokes and frames
    bool ok = true;
    for (int k = 0; ok && k < p->nsubs; k++) {
        const ani_subpath *sp = &p->subs[k];
        if (!full && cut <= sp->s0 && !(cut == 0.0 && k == 0)) break; // not reached yet
        if (sp->count == 0) continue;
        const double lcut = full ? sp->len : fmin(sp->len, cut - sp->s0);
        const bool whole = lcut >= sp->len;
        // a closed subpath is a seamless loop only when fully drawn and untapered
        const bool loop = sp->closed && whole && !tapered;
        b->n = 0;
        const double len = sp->len;
        boil_stream st = { 0 };
        if (boil) noise_init(&st.z, &in->boil, k, len, sp->closed);
        const double bsp = boil ? st.z.sp : 0.0;
        // open strokes displace while gathering; seamless loops afterwards
        st.on = boil && !loop;
        boil_stream *stp = boil ? &st : NULL;
        // revealed points, plus the closing point and the exact cut point
        const int npts = sp->count + (sp->closed ? 1 : 0);
        an_vec2 prev_lp = { 0, 0 };
        double prev_s = 0.0;
        for (int i = 0; ok && i < npts; i++) {
            const int j = sp->first + (i % sp->count);
            const double si = (i == sp->count) ? sp->len : p->s[j];
            if (i == sp->count && loop) {
                // the loop closes itself; only the closing segment's samples
                ok = push_samples(b, w, in->width * wscale, br, len, prev_lp, prev_s, p->pts[j], si, si, bsp, stp);
                break;
            }
            an_vec2 lp = p->pts[j];
            double s_here = si;
            bool stop = false;
            if (si > lcut && i > 0) { // cut inside the segment (i-1, i)
                const int jp = sp->first + (i - 1);
                const double sp0 = p->s[jp];
                const double t = (si - sp0) > 0 ? (lcut - sp0) / (si - sp0) : 0.0;
                lp = (an_vec2){ (float)(p->pts[jp].x + (lp.x - p->pts[jp].x) * t),
                                (float)(p->pts[jp].y + (lp.y - p->pts[jp].y) * t) };
                s_here = lcut;
                stop = true;
            }
            // samples on the original segment (prev -> this path point), up to the cut
            if (i > 0) ok = push_samples(b, w, in->width * wscale, br, len, prev_lp, prev_s, p->pts[j], si, s_here, bsp, stp);
            const float h = (float)(0.5 * in->width * wscale * ani_brush_factor(br, s_here, len));
            if (ok) ok = u_push(b, stp, ani_affine_apply(w, lp), h, (float)s_here);
            prev_lp = lp;
            prev_s = s_here;
            if (stop) break;
        }
        if (ok && boil) {
            if (loop) displace_loop_inplace(b, &st.z);
            else stream_finish(b, &st);
        }
        if (ok) ok = emit_polyline(m, b, loop, overshoot, in->color, log, k);
        if (!whole) break;
    }
    return ok;
}
