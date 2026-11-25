// Stroke geometry, checked on the triangulator's output (no GPU, no pixels):
//   - no NaN/inf, no zero-area triangles
//   - widths: rib normals are unit, constant-width strokes have exactly w/2,
//     every vertex lies within the half width (plus overshoot allowance) of the
//     centerline, so there are no miter spikes
//   - no gaps: points at 0.9 x half width across every segment and on the outer
//     bisector of every join are covered by some triangle
// over the stress set (tests/stroke_stress.h) at several draw progresses.
// Also: draw-on reveal, nozzle position/speed, outlines through the stroke path,
// and a timing report for 200 strokes.
#include "anown.h"
#include "check.h"
#include "stroke_internal.h"
#include "stroke_stress.h"
#include "scene_internal.h"
#include "timeline_internal.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static double dist(an_vec2 a, an_vec2 b) { return hypot(a.x - b.x, a.y - b.y); }

// Inside, or within 0.01 px of, triangle abc. Probes often sit exactly on an
// edge shared by two triangles (a fan's last edge is the next quad's first),
// where float rounding at coordinates ~1000 (ulp ~6e-5) can put them a hair
// outside both. Real gaps are whole pixels wide, so 0.01 px separates them.
static bool in_tri(an_vec2 p, an_vec2 a, an_vec2 b, an_vec2 c) {
    const double tol = 0.01;
    const an_vec2 v[3] = { a, b, c };
    const double area2 = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    const double orient = area2 >= 0 ? 1.0 : -1.0;
    for (int i = 0; i < 3; i++) {
        const an_vec2 e0 = v[i], e1 = v[(i + 1) % 3];
        const double ex = e1.x - e0.x, ey = e1.y - e0.y, el = hypot(ex, ey);
        if (el <= 0) return false;
        // signed distance of p from the edge line, positive toward the interior
        const double sd = orient * (ex * (p.y - e0.y) - ey * (p.x - e0.x)) / el;
        if (sd < -tol) return false;
    }
    return true;
}

static bool covered(const ani_mesh *m, an_vec2 p) {
    for (int i = 0; i + 2 < m->count; i += 3) {
        const ani_vertex *v = &m->v[i];
        const float minx = fminf(v[0].x, fminf(v[1].x, v[2].x)), maxx = fmaxf(v[0].x, fmaxf(v[1].x, v[2].x));
        const float miny = fminf(v[0].y, fminf(v[1].y, v[2].y)), maxy = fmaxf(v[0].y, fmaxf(v[1].y, v[2].y));
        if (p.x < minx - 1e-3f || p.x > maxx + 1e-3f || p.y < miny - 1e-3f || p.y > maxy + 1e-3f) continue;
        if (in_tri(p, (an_vec2){ v[0].x, v[0].y }, (an_vec2){ v[1].x, v[1].y }, (an_vec2){ v[2].x, v[2].y }))
            return true;
    }
    return false;
}

typedef struct geo_report {
    int strokes, tris, nan, degenerate, width_bad, spike, gap, normal_bad;
} geo_report;

static ani_boil g_check_boil; // zero = no boil; test_stress_geometry also runs with boil on

static void check_stroke(const stress_case *c, const an_obj *o, float progress, geo_report *r, int verbose) {
    ani_mesh m;
    ani_mesh_init(&m);
    ani_rib_log log = { 0 };
    const ani_stroke_in in = {
        .path = o->path, .world = ani_local(o->pos, o->scale, o->rotation), .width = c->width,
        .progress = progress, .color = AN_BLACK, .brush = &o->brush, .boil = g_check_boil,
    };
    CHECK(ani_stroke_emit(&m, &in, &log));
    r->strokes++;
    r->tris += m.count / 3;
    const double half = 0.5 * c->width;
    double pmax = 1.0;
    for (int i = 0; i < o->brush.pressure_count; i++) pmax = fmax(pmax, o->brush.pressure[i]);
    const bool constant = !o->brush.taper_in && !o->brush.taper_out && !o->brush.pressure_count;
    const double allowance = 1.0 + 2.0 * o->brush.overshoot;

    // triangles: finite, non-degenerate
    for (int i = 0; i + 2 < m.count; i += 3) {
        const ani_vertex *v = &m.v[i];
        for (int k = 0; k < 3; k++) {
            if (!isfinite(v[k].x) || !isfinite(v[k].y)) r->nan++;
        }
        const double area = 0.5 * fabs((v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[1].y - v[0].y) * (v[2].x - v[0].x));
        if (!(area >= 1e-6)) r->degenerate++;
    }
    // ribs: unit normals, widths in range, exact for constant-width strokes
    for (int i = 0; i < log.count; i++) {
        const ani_rib *b = &log.ribs[i];
        if (fabs(hypot(b->n.x, b->n.y) - 1.0) > 1e-4) r->normal_bad++;
        if (b->h < 0 || b->h > half * pmax + 1e-4) r->width_bad++;
        if (constant && fabs(b->h - half) > 1e-4) r->width_bad++;
    }
    // the ribbon follows the brush profile BETWEEN ribs too: the width the
    // triangles interpolate linearly must match the profile at those arc lengths
    for (int i = 0; i + 1 < log.count; i++) {
        const ani_rib *a = &log.ribs[i], *b = &log.ribs[i + 1];
        if (a->sub != b->sub || b->s <= a->s) continue;
        const double sublen = o->path->subs[a->sub].len;
        for (int k = 1; k <= 3; k++) {
            const double t = k / 4.0;
            const double s_mid = a->s + (b->s - a->s) * t;
            const double want = half * ani_brush_factor(&o->brush, s_mid, sublen);
            const double got = a->h + (b->h - a->h) * t;
            if (fabs(got - want) > fmax(0.3, 0.03 * half)) {
                if (verbose && r->width_bad < 3)
                    fprintf(stderr, "  profile miss in '%s' p=%.2f s=%.1f: width %.2f, profile %.2f\n", c->name,
                            progress, s_mid, 2 * got, 2 * want);
                r->width_bad++;
            }
        }
    }
    // no spikes: each vertex is within its allowance of some rib point
    for (int i = 0; i < m.count; i++) {
        const an_vec2 v = { m.v[i].x, m.v[i].y };
        bool ok = false;
        for (int j = 0; j < log.count && !ok; j++) {
            ok = dist(v, log.ribs[j].p) <= log.ribs[j].h * allowance + 0.02;
        }
        // vertices of segment quads sit between two ribs: allow the larger one
        for (int j = 0; j + 1 < log.count && !ok; j++) {
            const ani_rib *a = &log.ribs[j], *b = &log.ribs[j + 1];
            if (a->sub != b->sub) continue;
            const double ax = b->p.x - a->p.x, ay = b->p.y - a->p.y, l2 = ax * ax + ay * ay;
            if (l2 <= 0) continue;
            double t = ((v.x - a->p.x) * ax + (v.y - a->p.y) * ay) / l2;
            t = t < 0 ? 0 : (t > 1 ? 1 : t);
            const an_vec2 q = { (float)(a->p.x + ax * t), (float)(a->p.y + ay * t) };
            ok = dist(v, q) <= fmax(a->h, b->h) + 0.02;
        }
        if (!ok) {
            if (verbose && r->spike < 3) fprintf(stderr, "  spike in '%s' p=%.2f at (%.2f, %.2f)\n", c->name, progress, v.x, v.y);
            r->spike++;
        }
    }
    // no gaps: across each segment, on both bisectors of each join, and past
    // each open end (the round cap)
    const bool tapered = o->brush.taper_in > 0 || o->brush.taper_out > 0;
    for (int i = 0; i < log.count; i++) {
        const ani_rib *a = &log.ribs[i];
        if (a->h < 0.05f) continue;
        an_vec2 probes[16];
        int np = 0;
        const bool first_of_sub = i == 0 || log.ribs[i - 1].sub != a->sub;
        const bool last_of_sub = i + 1 == log.count || log.ribs[i + 1].sub != a->sub;
        const ani_subpath *sp = &o->path->subs[a->sub];
        const bool loop = sp->closed && progress >= 1.0f && !tapered && !(first_of_sub && last_of_sub);
        if (!loop && (first_of_sub || last_of_sub)) {
            // tangent = normal rotated back by 90 degrees: d = (n.y, -n.x)
            const an_vec2 d = { a->n.y, -a->n.x };
            const float k = (first_of_sub ? -1.0f : 1.0f) * a->h * 0.9f;
            if (first_of_sub && last_of_sub) { // a dot: probe all around
                for (int q = 0; q < 4; q++) {
                    const float ang = (float)q * 1.5707963f;
                    probes[np++] = (an_vec2){ a->p.x + cosf(ang) * a->h * 0.9f, a->p.y + sinf(ang) * a->h * 0.9f };
                }
            } else {
                probes[np++] = (an_vec2){ a->p.x + d.x * k, a->p.y + d.y * k };
            }
        }
        probes[np++] = (an_vec2){ a->p.x + a->n.x * a->h * 0.9f, a->p.y + a->n.y * a->h * 0.9f };
        probes[np++] = (an_vec2){ a->p.x - a->n.x * a->h * 0.9f, a->p.y - a->n.y * a->h * 0.9f };
        if (i + 1 < log.count && log.ribs[i + 1].sub == a->sub) {
            const ani_rib *b = &log.ribs[i + 1];
            for (int k = 1; k <= 3; k++) {
                const float t = (float)k / 4.0f;
                const an_vec2 cpt = { a->p.x + (b->p.x - a->p.x) * t, a->p.y + (b->p.y - a->p.y) * t };
                const float hh = (a->h + (b->h - a->h) * t) * 0.9f;
                if (hh < 0.05f) continue;
                probes[np++] = (an_vec2){ cpt.x + a->n.x * hh, cpt.y + a->n.y * hh };
                probes[np++] = (an_vec2){ cpt.x - a->n.x * hh, cpt.y - a->n.y * hh };
            }
        }
        if (i > 0 && log.ribs[i - 1].sub == a->sub && !last_of_sub) {
            // interior join: incoming normal n0, outgoing n1. Probe both bisectors;
            // for a reversal (n0 + n1 ~ 0) probe straight ahead, through the tip.
            const an_vec2 n0 = log.ribs[i - 1].n, n1 = a->n;
            an_vec2 bis = { n0.x + n1.x, n0.y + n1.y };
            const double bl = hypot(bis.x, bis.y);
            if (bl > 1e-3) {
                bis = (an_vec2){ (float)(bis.x / bl), (float)(bis.y / bl) };
            } else {
                bis = (an_vec2){ n0.y, -n0.x }; // incoming direction d0, since n0 = (-d0.y, d0.x)
            }
            probes[np++] = (an_vec2){ a->p.x + bis.x * a->h * 0.9f, a->p.y + bis.y * a->h * 0.9f };
            probes[np++] = (an_vec2){ a->p.x - bis.x * a->h * 0.9f, a->p.y - bis.y * a->h * 0.9f };
        }
        for (int k = 0; k < np; k++) {
            if (!covered(&m, probes[k])) {
                if (verbose && r->gap < 3) fprintf(stderr, "  gap in '%s' p=%.2f at (%.2f, %.2f)\n", c->name, progress, probes[k].x, probes[k].y);
                r->gap++;
            }
        }
    }
    free(log.ribs);
    ani_mesh_free(&m);
}

static void test_stress_geometry(void) {
    an_scene *s = an_scene_create(NULL);
    an_obj *objs[STRESS_MAX];
    stress_case cases[STRESS_MAX];
    const int n = stress_build(s, objs, cases);
    CHECK(n == 20);
    geo_report r = { 0 };
    const float progresses[5] = { 1.0f, 0.13f, 0.5f, 0.77f, 0.999f };
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 5; k++) check_stroke(&cases[i], objs[i], progresses[k], &r, 1);
    }
    printf("  stress set: %d strokes x 5 progresses, %d triangles; nan %d, degenerate %d, "
           "width %d, normals %d, spikes %d, gaps %d\n",
           n, r.tris, r.nan, r.degenerate, r.width_bad, r.normal_bad, r.spike, r.gap);
    CHECK(r.nan == 0);
    CHECK(r.degenerate == 0);
    CHECK(r.width_bad == 0);
    CHECK(r.normal_bad == 0);
    CHECK(r.spike == 0);
    CHECK(r.gap == 0);

    // again with line boil on (amplitude 2 px, 15 px buckets): the centerline is
    // displaced before the ribbon is built, so every guarantee must still hold
    geo_report rb = { 0 };
    for (int i = 0; i < n; i++) {
        g_check_boil = (ani_boil){ .amp = 2.0f, .spacing = 15.0f, .seed = 9, .stream = objs[i]->rng_key, .step = 4 };
        for (int k = 0; k < 5; k++) check_stroke(&cases[i], objs[i], progresses[k], &rb, 1);
    }
    g_check_boil = (ani_boil){ 0 };
    printf("  stress set with boil: %d triangles; nan %d, degenerate %d, width %d, normals %d, spikes %d, gaps %d\n",
           rb.tris, rb.nan, rb.degenerate, rb.width_bad, rb.normal_bad, rb.spike, rb.gap);
    CHECK(rb.nan == 0 && rb.degenerate == 0 && rb.width_bad == 0 && rb.normal_bad == 0 && rb.spike == 0 && rb.gap == 0);
    an_scene_destroy(s);
}

// Raw triangulator input with duplicates and zero-length segments that never
// went through the path builder's close-dedup.
static void test_raw_degenerates(void) {
    ani_arena a;
    ani_arena_init(&a, 0);
    ani_path_builder b;
    ani_pb_init(&b);
    ani_pb_move(&b, (an_vec2){ 10, 10 });
    for (int i = 0; i < 5; i++) ani_pb_line(&b, (an_vec2){ 10, 10 });   // all duplicates
    ani_pb_line(&b, (an_vec2){ 10, 10.0000001f });                       // sub-epsilon move
    ani_pb_move(&b, (an_vec2){ 50, 50 });
    ani_pb_line(&b, (an_vec2){ 150, 50 });
    ani_pb_line(&b, (an_vec2){ 50, 50 });                                // reversal
    ani_pb_line(&b, (an_vec2){ 50, 50 });                                // duplicate
    ani_pb_close(&b);                                                    // closing onto itself
    ani_path *p = ani_pb_finish(&b, &a);
    ani_pb_free(&b);
    CHECK(p != NULL);
    ani_mesh m;
    ani_mesh_init(&m);
    const ani_brush br = { .overshoot = 1.0f };
    for (int k = 0; k <= 20; k++) {
        m.count = 0;
        const ani_stroke_in in = { .path = p, .world = { 1, 0, 0, 1, 0, 0 }, .width = 12,
                                   .progress = (float)k / 20.0f, .color = AN_BLACK, .brush = &br };
        CHECK(ani_stroke_emit(&m, &in, NULL));
        for (int i = 0; i < m.count; i++) CHECK(isfinite(m.v[i].x) && isfinite(m.v[i].y));
        if (k == 0) CHECK(m.count == 0); // nothing drawn at progress 0
    }
    // zero width or zero-length path: nothing, no crash
    const ani_stroke_in zero = { .path = p, .world = { 1, 0, 0, 1, 0, 0 }, .width = 0, .progress = 1, .color = AN_BLACK };
    m.count = 0;
    CHECK(ani_stroke_emit(&m, &zero, NULL) && m.count == 0);
    ani_mesh_free(&m);
    ani_arena_release(&a);
}

// Outlines of rects and circles come from the stroke code, only when asked for.
static void test_outlines(void) {
    an_scene *s = an_scene_create(NULL);
    an_set_chaos(s, 0); // compares against a direct, unboiled ani_stroke_emit call
    an_rect(s, &(an_rect_opts){ .fill = AN_HEX(0xFFE800) });
    ani_mesh m;
    ani_mesh_init(&m);
    CHECK(ani_tess_scene(&m, s, 0) && m.count == 6 + 6); // background + fill quad, no outline
    an_obj *r2 = an_rect(s, &(an_rect_opts){ .fill = AN_HEX(0xFFE800), .stroke = AN_HEX(0x16161A), .stroke_width = 6 });
    CHECK(ani_tess_scene(&m, s, 0));
    const int with_outline = m.count;
    CHECK(with_outline > 6 + 6 + 6);
    // the same outline the stroke code emits for that path
    ani_mesh o;
    ani_mesh_init(&o);
    const ani_stroke_in in = { .path = r2->path, .world = ani_local(r2->pos, r2->scale, r2->rotation), .width = 6,
                               .progress = 1, .color = AN_HEX(0x16161A), .brush = &r2->brush };
    CHECK(ani_stroke_emit(&o, &in, NULL));
    CHECK(with_outline == 6 + 6 + 6 + o.count);
    CHECK(memcmp(m.v + 18, o.v, (size_t)o.count * sizeof(ani_vertex)) == 0);
    // no_fill: outline only
    an_circle(s, &(an_circle_opts){ .no_fill = true, .stroke_width = 3 });
    CHECK(ani_tess_scene(&m, s, 0) && m.count > with_outline);
    ani_mesh_free(&o);
    ani_mesh_free(&m);
    an_scene_destroy(s);
}

// Draw progress and stroke width are ordinary animatable properties.
static void test_draw_and_width_tracks(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 60 });
    an_obj *l = an_line(s, &(an_line_opts){ .from = { 0, 0 }, .to = { 300, 0 }, .undrawn = true, .stroke_width = 10 });
    an_play(s, an_draw(l, &(an_anim_opts){ .duration = 1, .ease = AN_EASE_LINEAR }));
    an_play(s, an_stroke_width_to(l, 20, &(an_anim_opts){ .duration = 1, .ease = AN_EASE_LINEAR }));
    ani_mesh m;
    ani_mesh_init(&m);
    CHECK(ani_tess_scene(&m, s, 0) && m.count == 6); // undrawn: background only
    int prev = 0;
    for (int64_t f = 1; f <= 60; f += 7) { // revealed length grows
        an_seek(s, f);
        CHECK_NEAR(an_obj_get(l).draw, (double)f / 60.0, 1e-6);
    }
    an_seek(s, 90);
    CHECK_NEAR(an_obj_get(l).stroke_width, 15.0, 1e-4);
    (void)prev;
    // an_draw always starts at 0, even if the object was fully drawn
    an_obj *k = an_line(s, &(an_line_opts){ .to = { 100, 0 } });
    an_play(s, an_draw(k, NULL));
    CHECK(k->ch[AN_PROP_DRAW]->tracks[0].from.v[0] == 0.0);
    ani_mesh_free(&m);
    an_scene_destroy(s);
}

// Nozzle: position from arc length at the sampled progress, speed from the
// analytic derivative of the draw easing. Pure: any seek order agrees.
static void test_nozzle(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 60 });
    an_obj *l = an_line(s, &(an_line_opts){ .pos = { 100, 100 }, .to = { 300, 0 }, .undrawn = true });
    an_play(s, an_draw(l, &(an_anim_opts){ .duration = 2, .ease = AN_EASE_LINEAR }));
    an_obj *g = an_group(s, &(an_group_opts){ .pos = { 0, 500 }, .scale = { 2, 2 } });
    an_obj *e = an_line(s, &(an_line_opts){ .parent = g, .to = { 300, 0 }, .undrawn = true });
    an_play(s, an_draw(e, &(an_anim_opts){ .duration = 2 })); // in-out, starts at 2 s

    an_seek(s, 0);
    CHECK(!an_obj_nozzle(l).active);
    an_seek(s, 60); // linear, p = 0.5
    an_nozzle nz = an_obj_nozzle(l);
    CHECK(nz.active);
    CHECK_NEAR(nz.pos.x, 250.0, 1e-3);
    CHECK_NEAR(nz.pos.y, 100.0, 1e-3);
    CHECK_NEAR(nz.speed, 150.0, 1e-3); // 300 px / 2 s
    CHECK_NEAR(nz.dir.x, 1.0, 1e-6);
    CHECK_NEAR(nz.width, 4.0, 1e-6);
    an_seek(s, 120);
    nz = an_obj_nozzle(l);
    CHECK(!nz.active && nz.speed == 0.0f);
    CHECK_NEAR(nz.pos.x, 400.0, 1e-3);

    // in-out at its midpoint: e'(0.5) = 3, so speed = 3 * L / T; parent scale 2
    an_seek(s, 120 + 60);
    nz = an_obj_nozzle(e);
    CHECK(nz.active);
    CHECK_NEAR(nz.pos.x, 300.0, 1e-3); // half of 300 local, times 2
    CHECK_NEAR(nz.pos.y, 500.0, 1e-3);
    CHECK_NEAR(nz.speed, 3.0 * 300.0 / 2.0 * 2.0, 1e-2);
    CHECK_NEAR(nz.width, 8.0, 1e-5);

    // order independence
    an_nozzle fwd[241];
    for (int f = 0; f <= 240; f++) {
        an_seek(s, f);
        fwd[f] = an_obj_nozzle(e);
    }
    int mism = 0;
    for (int f = 240; f >= 0; f -= 3) {
        an_seek(s, f);
        const an_nozzle b = an_obj_nozzle(e);
        if (memcmp(&b, &fwd[f], sizeof b) != 0) mism++;
    }
    CHECK(mism == 0);
    an_scene_destroy(s);
}

// Timing: tessellation of a scene with 200 strokes being drawn on.
static void bench_200_strokes(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 60, .seed = 7 });
    an_anim draws[200];
    an_rng *rng = an_scene_rng(s);
    for (int i = 0; i < 200; i++) {
        an_seg segs[7];
        float x = an_rng_range(rng, 100, 1820), y = an_rng_range(rng, 100, 980);
        segs[0] = AN_MOVE(x, y);
        for (int k = 1; k < 7; k++) {
            const float nx = x + an_rng_range(rng, -120, 120), ny = y + an_rng_range(rng, -120, 120);
            segs[k] = AN_CUBIC(x + an_rng_range(rng, -80, 80), y + an_rng_range(rng, -80, 80),
                               nx + an_rng_range(rng, -80, 80), ny + an_rng_range(rng, -80, 80), nx, ny);
            x = nx;
            y = ny;
        }
        an_obj *o = an_stroke(s, &(an_stroke_opts){
            .segs = segs, .seg_count = 7, .stroke = AN_HEX(0x16161A), .stroke_width = 6, .undrawn = true,
            .brush = { .taper_in = 30, .taper_out = 60, .overshoot = 0.5f } });
        draws[i] = an_draw(o, &(an_anim_opts){ .duration = 1.5f });
    }
    an_stagger(s, 200, draws, 0.02f);
    const int64_t L = an_scene_length(s);
    ani_mesh m;
    ani_mesh_init(&m);
    double total = 0, worst = 0;
    int max_tris = 0;
    for (int64_t f = 0; f <= L; f++) {
        const double t0 = now_ms();
        CHECK(ani_tess_scene(&m, s, f));
        const double dt = now_ms() - t0;
        total += dt;
        if (dt > worst) worst = dt;
        if (m.count / 3 > max_tris) max_tris = m.count / 3;
    }
    const double t0 = now_ms();
    for (int i = 0; i < 20; i++) CHECK(ani_tess_scene(&m, s, L)); // all strokes complete
    const double full = (now_ms() - t0) / 20.0;
    printf("  200 strokes (7 cubics each, taper + overshoot): %lld frames, tessellation avg %.3f ms/frame, "
           "worst %.3f ms, all-drawn %.3f ms/frame, up to %d triangles\n",
           (long long)(L + 1), total / (double)(L + 1), worst, full, max_tris);
    ani_mesh_free(&m);
    an_scene_destroy(s);
}

int main(void) {
    test_stress_geometry();
    test_raw_degenerates();
    test_outlines();
    test_draw_and_width_tracks();
    test_nozzle();
    bench_200_strokes();
    return check_done("test_stroke");
}
